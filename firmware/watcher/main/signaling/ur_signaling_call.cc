/* humanCall 人际通话外观实现：借用共享 MQTT 连接驱动 HumanCallClient，
 * 媒体侧用 libpeer 普通两轨 SDP（音频 sendrecv + 视频 sendrecv）复刻通话链路；
 * 下行视频经 CallVideoRenderer 解码为 RGB565 送板级 UI，上行视频复用 aiVision
 * 管线（UrSignalingVideo）。音频与 AI 语音共享同一 PCM 桥与 Opus 端口——两者
 * 经 Busy/hostBusy 互斥，绝不并发采集。
 * 内存纪律（真机实测教训）：重组件（HumanCallClient/媒体端口/Peer/SDP）全部
 * 懒构造——Attach 只存指针，第一条 humanCall 邀请到达才建腿，通话归位后销毁；
 * SDP 与下行帧等 >1KB 缓冲一律落 PSRAM，空闲内部 SRAM 不被通话栈占用。
 * 来电让位（interrupt）语义：AI 会话占用时先请求宿主结束 AI 会话（SetHostYield），
 * 保留邀请快照周期检查让位进度，超时/不受理才回 sessionReject busy。
 * 全部状态推进收敛在信令任务的 Poll；板级 UI 经 FreeRTOS 命令队列转交。
 * 仅 WebRTC 形态编译。
 * 排列顺序：Impl（数据与声明）→ PeerMedia（媒体端口实现）→ Impl 方法定义。 */
#include "ur_signaling_call.h"
#include "ur_signaling_call_poll.h"
#include "ur_signaling_pending_tombstones.h"

#include "application.h"
#include "audio_codec.h"
#include "board.h"

#if CONFIG_UR_AI_SIGNALING_WEBRTC

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "peer_connection.h"

#include "ur/device/envelope.hpp"
#include "ur/device/human_call.hpp"
#include "ur/device/udp_av.hpp"

#if CONFIG_UR_AI_SIGNALING_WEBRTC_VIDEO
#include "ur_signaling_call_jpeg.h"
#include "ur_signaling_call_video.h"
#if CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER
#include "watcher_udp_video_gate.h"
#endif
#endif
#include "ur_signaling_cipher.h"
#include "ur_signaling_udp_port.h"
#if CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER
#include "watcher_call_audio.h"
#endif
#if CONFIG_UR_AI_SIGNALING_WEBRTC_VIDEO && CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER
#include "ur_av_probe.h"
#endif

namespace {
constexpr const char* kTag = "ur_sig_call";
/* 下行 opus 帧缓存上限：20ms 帧 50fps，100 帧=2 秒缓冲，超限丢最旧保实时。 */
constexpr std::size_t kDownlinkLimit = 100;
constexpr int kIn16kSamples = 320;
/* Ui 命令队列深度：覆盖 UI 并发按键；满时丢弃（幂等按键可重按）。 */
constexpr UBaseType_t kUiQueueLength = 8;
/* Connected 后 ICE 掉线的本地挂断阈值：平台侧无对应下行事件时的兜底。 */
constexpr std::uint64_t kIceDropHangupMs = 10000;
/* 让位检查周期与等待上限：AI 会话结束（Ending→Idle）通常数百毫秒，
 * 500ms 轮询足够平滑；10 秒仍未让位则回 busy（平台邀请期 30 秒足够）。 */
constexpr std::uint64_t kYieldCheckMs = 500;
constexpr std::uint64_t kYieldTimeoutMs = 10000;
/* 建腿看门狗：从邀请交付起 45 秒内未进入媒体连通（含协商/连接各阶段）即强制
 * 销毁——HumanCallClient 的 30 秒阶段期限依赖其内部状态推进，真机曾出现腿
 * 长时间滞留（僵尸腿）导致后续来电被 0.1 秒 busy 拒绝，看门狗兜底根治。 */
constexpr std::uint64_t kEstablishWatchdogMs = 45000;

/* MonotonicMs 返回可控单调毫秒时钟，供状态机期限与本地看门狗使用。 */
std::uint64_t MonotonicMs() { return static_cast<std::uint64_t>(esp_timer_get_time() / 1000); }

// SetHumanCallAudioProcessing 由应用任务调用；enable控制接管，use_aec选择本次AEC预算，归位恢复原状态。
// 状态保存在应用任务的进程期作用域，调度闭包不借用可能已销毁的媒体腿对象。
void SetHumanCallAudioProcessing(bool enable, [[maybe_unused]] bool use_aec = true) {
    static bool engaged = false;         // 只由应用任务读写，重复启动不得覆盖最初快照。
    static bool restore_wake = false;    // 接管前的唤醒状态，挂断后恢复已有检测能力。
    static bool previous_aec = false;    // 接管前AEC设置，不能把人际策略残留给待机或AI。
    auto& app = Application::GetInstance();
    auto& audio = app.GetAudioService();
    if (enable) {
        if (!engaged) {
            restore_wake = audio.IsWakeWordRunning();
            previous_aec = audio.IsDeviceAecEnabled();
            engaged = true;
            app.SetHumanCallAudioOwned(true);
            ESP_LOGI(kTag, "call audio ownership wake_before=%d aec_before=%d", restore_wake,
                     previous_aec);
        }
        Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        audio.EnableWakeWordDetection(false);
#if CONFIG_UR_AI_SIGNALING_DEVICE_AEC || CONFIG_UR_SOFTWARE_AEC
        // 人际通话按已编译的回采能力启用AEC，不依赖普通AI是否声明全双工能力。
        audio.EnableDeviceAec(use_aec);
#endif
        audio.EnableLowLatencyCapture(true);  // 本地原生块及时交付，网络继续20ms Opus。
        audio.EnableVoiceProcessing(true);
        return;
    }
    if (!engaged)
        return;
    engaged = false;
    app.SetHumanCallAudioOwned(false);
    audio.EnableLowLatencyCapture(false);  // 始终归还默认聚帧，迟到关闭也不遗留人际设置。
    // AI已取得应用音频时，迟到的人际停止不能关闭其处理、AEC或WiFi性能模式。
    if (app.GetDeviceState() != kDeviceStateIdle)
        return;
    audio.EnableVoiceProcessing(false);
#if CONFIG_UR_AI_SIGNALING_DEVICE_AEC || CONFIG_UR_SOFTWARE_AEC
    audio.EnableDeviceAec(previous_aec);
#endif
    if (restore_wake)
        audio.EnableWakeWordDetection(true);
    Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
    ESP_LOGI(kTag, "call audio restored wake=%d", audio.IsWakeWordRunning());
}

/* RandomCallInstance 生成本进程唯一的通话客户端请求前缀（32 个十六进制字符）。 */
std::string RandomCallInstance() {
    std::string instance;
    instance.reserve(32);
    while (instance.size() < 32) {
        const uint32_t random = esp_random();
        char buffer[9] = {0};
        snprintf(buffer, sizeof(buffer), "%08" PRIx32, random);
        instance.append(buffer);
    }
    return instance;
}

/* kEmptySession 为无在途腿时的会话空值引用，避免三元表达式产生临时拷贝。 */
const std::string kEmptySession;

/* PsramBuffer 是 >1KB 大缓冲的 PSRAM 封装（8bit SPIRAM）：
 * SDP offer/answer、下行 opus 帧等一律经它落 PSRAM，避免挤占内部 SRAM。
 * 分配失败保持空（size()==0），调用方按资源不足处理。支持移动语义，
 * 可直接作为 deque 元素使用。 */
class PsramBuffer {
public:
    PsramBuffer() = default;
    PsramBuffer(const PsramBuffer&) = delete;
    PsramBuffer& operator=(const PsramBuffer&) = delete;
    /* 移动构造/赋值转移 PSRAM 所有权，源置空。 */
    PsramBuffer(PsramBuffer&& other) noexcept : data_(other.data_), size_(other.size_) {
        other.data_ = nullptr;
        other.size_ = 0;
    }
    PsramBuffer& operator=(PsramBuffer&& other) noexcept {
        if (this != &other) {
            Reset();
            data_ = other.data_;
            size_ = other.size_;
            other.data_ = nullptr;
            other.size_ = 0;
        }
        return *this;
    }
    ~PsramBuffer() { Reset(); }
    /* Assign 分配 len 字节 PSRAM 并拷贝数据；失败返回 false 且保持空。 */
    bool Assign(const void* data, std::size_t len) {
        Reset();
        if (data == nullptr || len == 0) {
            return false;
        }
        void* memory = heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (memory == nullptr) {
            return false;
        }
        std::memcpy(memory, data, len);
        data_ = static_cast<std::uint8_t*>(memory);
        size_ = len;
        return true;
    }
    /* Reset 立即释放 PSRAM；幂等。 */
    void Reset() {
        if (data_ != nullptr) {
            heap_caps_free(data_);
            data_ = nullptr;
        }
        size_ = 0;
    }
    /* data 只读访问缓冲首地址；空缓冲返回 nullptr。 */
    const std::uint8_t* data() const { return data_; }
    /* size 返回有效字节数。 */
    std::size_t size() const { return size_; }

private:
    std::uint8_t* data_ = nullptr;
    std::size_t size_ = 0;
};
}  // namespace

/* Impl 聚合通话腿、媒体端口与 UI 交互原语；除 Ui* 入口与 SetUiHooks 外，
 * 一切方法只在信令任务内调用。先声明后定义：方法体需要 PeerMedia 完整类型。 */
struct UrSignalingCall::Impl {
public:
    /* Ui 命令枚举：任意任务投递，信令任务消费。 */
    enum UiCommand {
        kUiAccept = 1,
        kUiReject = 2,
        kUiHangUp = 3,
        kUiToggleMic = 4,
        kUiToggleCamera = 5,
        kUiDial = 6,  // 已授权设备目标的主动呼叫，和来电操作共用任务队列。
    };
    /* QueuedUiCommand 将投递时的来电代次与动作一起交给信令线程。 */
    struct QueuedUiCommand {
        int command;
        std::uint32_t generation;
        char target[129]{};  // 有界产品:设备标识，队列按值持有，不借用调用者字符串。
        bool voice_only = false;  // 纯语音主动呼叫不预约相机。
    };

    Impl();
    ~Impl();

    /* Attach 绑定共享 MQTT 端口与身份；只保存轻量指针，不构造任何重组件
     * （真机实测全量常驻会吃掉约 11KB 内部 SRAM）。可重复调用。 */
    void Attach(ur::device::UrSignalingMqttPort* port, const std::string& identity);
    // Detach 先完成媒体退出屏障，再解除宿主借用，不能留下对旧任务的编码通知。
    void Detach();
    /* SetAudioBridge 注入共享 PCM 桥与 Opus 端口；须在 Attach 前就绪。 */
    void SetAudioBridge(ur::device::UrSignalingPcm* pcm, ur::device::UrSignalingOpus* codec);
    /* SetHostBusy 注入 AI 腿占用查询；来电先让位、超时才回 busy。 */
    void SetHostBusy(std::function<bool()> host_busy);
    /* SetHostYield 注入宿主让位请求（信令任务内同步、非阻塞）。 */
    void SetHostYield(std::function<bool()> yield);
#if CONFIG_UR_AI_SIGNALING_WEBRTC_VIDEO
    /* SetVideoPipeline 注入上行视频管线与启动闭包。 */
    void SetVideoPipeline(ur::device::UrSignalingVideo* video, std::function<bool()> starter);
#endif
    /* SetHooks 互斥替换 UI 回调；回调可在信令任务或解码任务上下文触发。 */
    void SetHooks(UiHooks hooks);
    /* Handle 按用途与会话归属分派下行信令；返回 true 表示已消费。 */
    bool Handle(std::string_view topic, std::string_view body, std::uint64_t now,
                std::uint64_t wall);
    /* Poll 推进状态机、媒体与 UI 投影；信令任务主循环每轮调用。 */
    void Poll(std::uint64_t now, std::uint64_t wall);
    /* Busy 表示人际腿占用媒体或正在等待接听；AI 会话建立须避让。 */
    bool Busy() const;
#if CONFIG_UR_SPARKBOT_VOICE_MODEL_TEST
    bool VoiceAnswerGuardActive() const { return voice_guard_.load(); }
#endif
    /* EnsureUiQueue 在接受来电前准备命令队列，失败时不得进入无法操作的等待状态。 */
    bool EnsureUiQueue();
    /* PostUi 投递一条 UI 命令；满队列丢弃（按键幂等）。 */
    void PostUi(int command);
    /* PostUiForGeneration 按状态回调提供的原代次投递，防止输入线程延迟时跨来电。 */
    void PostUiForGeneration(int command, std::uint32_t generation);
    // PostDial 只在队列上提交一个在途拨号意图，不能跨任务访问媒体客户端。
    bool PostDial(const std::string& target, bool voice_only);

private:
    /* PeerMedia 是 CallMediaPort 的 libpeer 实现，见下方完整定义。 */
    class PeerMedia;

    /* ---- 注入的宿主资源（信令协议持有，进程内长期有效）---- */
    ur::device::UrSignalingMqttPort* port_ = nullptr;
    std::string identity_;
    std::string instance_;
    ur::device::UrSignalingPcm* pcm_ = nullptr;
    ur::device::UrSignalingOpus* codec_ = nullptr;
    std::function<bool()> host_busy_;
    std::function<bool()> host_yield_;
#if CONFIG_UR_AI_SIGNALING_WEBRTC_VIDEO
    ur::device::UrSignalingVideo* video_ = nullptr;
    std::function<bool()> video_starter_;
#endif

    /* ---- UI 回调与命令队列 ---- */
    UiHooks hooks_;
    SemaphoreHandle_t hooks_mutex_ = nullptr;
    QueueHandle_t ui_queue_ = nullptr;
    std::atomic<std::uint32_t> ui_generation_{0};  // 终态使已经投递的旧命令失效。
    std::atomic<bool> dial_queued_{false};  // 排队、AI让位及主叫通话共用同一个占用标志。
    std::atomic<bool> dial_ready_{false};  // 宿主绑定后才受理跨任务拨号，解绑立即关闭入口。
    std::optional<QueuedUiCommand> pending_dial_;  // 仅信令任务持有等待AI释放的拨号快照。
    std::uint64_t dial_requested_at_ = 0;  // AI让位等待起点，超时后不迟到拨号。
    // AdvanceDial 仅在宿主实际空闲后建腿，不能把让位受理当成音频已释放。
    void AdvanceDial(std::uint64_t now);
#if CONFIG_UR_SPARKBOT_VOICE_MODEL_TEST
    std::atomic<bool> voice_guard_{false}, voice_allowed_{false};
    std::atomic<unsigned> voice_app_jobs_{0};  // 等待所有应用任务及模型释放后才能接听。
    bool voice_accept_pending_ = false;         // 仅信令任务访问。
    bool voice_guard_end_pending_ = false;      // 模型与排队任务释放后才允许 AI 唤醒。
    std::uint32_t voice_accept_generation_ = 0;
    void StartVoiceAnswerTest();
    void StopVoiceAnswerTest(bool end_guard);
    void AdvanceVoiceAnswerAccept(std::uint64_t now);
    void AdvanceVoiceAnswerGuard();
#endif

    /* ---- 通话腿与媒体 ---- */
    std::unique_ptr<PeerMedia> media_;
    std::unique_ptr<ur::device::HumanCallClient> client_;

    /* PendingInvite 为等待宿主让位的来电快照：AI 会话占用时先不回 busy，
     * 保留原始报文，让位后原样交给新建的腿。同期只保留首条；相同 token 或
     * 会话的重投须核对完整身份，其他邀请明确返回占线。 */
    struct PendingInvite {
        std::string token;            /* 邀请 msgToken，用于 QoS1 重投去重。 */
        std::string session;          /* 原会话标识，防止 token 被挪作其他通话。 */
        std::string body;             /* 原始信令报文，让位后原样交付。 */
        std::uint64_t expires = 0;    /* 平台原始截止时间，不随重投延长。 */
        std::size_t retained_bytes = 0; /* 预留终态载荷预算，不额外复制参数。 */
        std::uint64_t started = 0;    /* 让位等待起点（单调毫秒）。 */
        std::uint64_t last_check = 0; /* 上次让位检查时刻。 */
    };
    std::optional<PendingInvite> pending_invite_;
    PendingCallTombstones pending_tombstones_;  // 等待期终态按原截止保留，信令任务独占。
    std::string pending_tombstones_identity_;   // 身份变更时不能复用其他设备的终态。
    /* pending_accept_ 为让位等待期用户已按接听的排队标志：建腿进入 Ringing
     * 后自动执行一次 accept。 */
    bool pending_accept_ = false;
    /* leg_started_ms_ 为本腿邀请交付时刻（单调毫秒），供建腿看门狗计时。 */
    std::uint64_t leg_started_ms_ = 0;
    /* last_client_state_ 仅用于通话状态机变化日志（信令任务线程内读写）。 */
    ur::device::HumanCallState last_client_state_ = ur::device::HumanCallState::Idle;

    /* ---- 单轮通话旗标（信令任务内读写）---- */
    UiState ui_state_ = UiState::Hidden;
    bool hidden_pending_ = false;
    bool offer_sent_ = false;
    bool connected_reported_ = false;
    bool media_active_ = false;
    bool voice_call_ = false;  // 当前已接听类型；纯语音禁止相机和视频编解码。
    bool mic_paused_ = false;
    bool camera_paused_ = false;
    bool udp_camera_owned_ = false;  // 仅停止本次UDP通话成功接管的相机流。
    std::uint64_t playback_epoch_ = 0;
    std::uint64_t epoch_counter_ = 0;
    std::uint64_t ice_down_since_ = 0;
#if CONFIG_UR_AI_SIGNALING_WEBRTC_VIDEO
    bool renderer_started_ = false;
    bool remote_video_selected_ = false;  // 只由严格邀请中的video-in轨启用，三轨通话保持关闭。
    ur::device::AudioCaptureGate audio_capture_gate_;  // 真实20ms上行吞吐窗口。
    bool remote_video_pause_sent_ = false;             // 平台停止原接收轨的控制只交付一次。
    std::atomic<bool> remote_video_disabled_{false};   // 连续慢帧或音频缺帧后本次通话永久停显。
    std::atomic<std::uint64_t> remote_frame_at_{0};    // 最近交给UI的新帧，用于500ms过期清屏。
    ur::device::RemoteVideoGate remote_video_gate_;    // 解码线程内使用；启动线程前重置。
    std::uint64_t media_started_at_ = 0;               // 音频预热窗口起点，避免刚接通误停显。
    std::uint64_t audio_capture_at_ = 0;               // 最近成功发送的麦克风帧，信令任务内更新。
    /* 下行解码器延后启动时刻：先让相机上行完成首批 SPI DMA 分配。 */
    std::uint64_t renderer_start_at_ = 0;
    int video_uplink_frames_ = 0;
#endif

    /* HostBusy 查询 AI 语音腿是否存活（信令任务内调用）。 */
    bool HostBusy() const {
#if CONFIG_UR_AI_SIGNALING_WEBRTC_VIDEO && CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER
        if (UrAVProbeActive())
            return true;
#endif
        return host_busy_ ? host_busy_() : false;
    }
    /* RequestHostYield 请求宿主结束 AI 会话（信令任务内同步、非阻塞）；
     * 返回 false 表示宿主不受理让位。 */
    bool RequestHostYield() { return host_yield_ ? host_yield_() : false; }
#if CONFIG_UR_AI_SIGNALING_WEBRTC_VIDEO
    /* VideoUplinkReady 按所选媒体检查相机能力，不把 JPEG 流误当作 H264 管线。 */
    bool VideoUplinkReady() const {
#if CONFIG_UR_HUMAN_CALL_UDP_AV
        auto* camera = Board::GetInstance().GetCamera();
        return camera && camera->SupportsCompressedStream();
#else
        return video_ != nullptr && video_starter_ != nullptr;
#endif
    }
#else
    /* 纯音频构建无上行视频能力：平台视频邀请固定含 video-1，不能接听。 */
    bool VideoUplinkReady() const { return false; }
#endif

    /* EnsureLeg 懒构造通话腿（UI 队列/媒体端口/客户端）；成功即处于 Available。
     * 任何一步内存不足返回 false，不留半初始化状态。 */
    bool EnsureLeg(std::uint64_t now, bool outgoing_voice = false);
    /* TeardownLeg 销毁通话腿并释放全部重组件；幂等，Idle/Available 后调用。 */
    void TeardownLeg();
    /* RejectInvite 直接发布 sessionReject（reason 为 busy/declined），用于
     * 腿无法构造或让位超时时让平台立即释放邀请，不等 30 秒超时。 */
    void RejectInvite(const ur::device::Envelope& invite, const char* reason);
    /* StartYieldWait 记录待让位邀请快照并首次请求宿主让位。 */
    void StartYieldWait(const ur::device::Envelope& invite, std::string_view body,
                        std::uint64_t now, std::uint64_t expires);
    // RetirePending 在释放等待快照前保留原身份，槽满时以原截止封锁新邀请。
    void RetirePending(PendingCallTombstones::Decision decision);
    /* AdvanceYieldWait 每 ~500ms 检查宿主是否已让位：已让位即建腿交付邀请；
     * 仍占用则再次请求让位（应用层可能重开 AI 会话）；超时回 busy。 */
    void AdvanceYieldWait(std::uint64_t now, std::uint64_t wall);
    /* DeliverInvite 建腿并把邀请原文交给状态机（进入 Ringing）；内部处理
     * 建腿失败回 busy、取代等待快照及让位期排队的接听补执行。 */
    bool DeliverInvite(std::string_view body, std::uint64_t now, std::uint64_t wall);
    /* DrainUiCommands 在信令任务内执行排队的 UI 命令。 */
    void DrainUiCommands(std::uint64_t now);
    /* ApplyUiCommand 执行单条 UI 命令；未通话或状态不符时为安全空操作。 */
    void ApplyUiCommand(int command, std::uint64_t now);
    /* DriveNegotiation 把本地媒体事实（offer、ICE/DTLS 连通）上报状态机。 */
    void DriveNegotiation(std::uint64_t now);
    /* WatchIceDrop 在 Connected 后 ICE 断开超时后本地挂断，防止无限占用媒体。 */
    void WatchIceDrop(std::uint64_t now);
    /* WatchEstablish 建腿看门狗：45 秒内未媒体连通即强制挂断并销毁腿，
     * 兜底状态机期限未触发的僵尸腿（真机实测发生过，机制待日志定位）。 */
    void WatchEstablish(std::uint64_t now);
    /* ProjectState 把 HumanCallState 归并为 UiState 并投递状态回调；
     * 通话结束先报 Ended，下一轮再归 Hidden。 */
    void ProjectState();
    /* RecoverIfFailed 在失败终态后把腿归 Idle，走正常 Hidden 收尾统一销毁。 */
    void RecoverIfFailed(std::uint64_t now);
    /* StartMedia 在 ICE/DTLS 双向连通后启用采集、播放、渲染；上行视频错峰。 */
    void StartMedia();
    std::uint64_t video_uplink_pending_at_ = 0;  // 相机上行延迟启动时刻（毫秒单调钟）。
    std::uint64_t now_ms_ = 0;                   // 最近一次 Poll 的毫秒单调钟快照。
    /* StopMedia 停用采集、播放、渲染与上行视频；幂等。 */
    void StopMedia();
    /* OnMediaClosed 由 PeerMedia::close 回调：媒体端口已销毁，联动停采集。 */
    void OnMediaClosed() { StopMedia(); }
#if CONFIG_UR_AI_SIGNALING_WEBRTC_VIDEO
    /* StartRenderer 启动下行视频解码任务并把帧回调接到 UI 钩子。 */
    void StartRenderer();
    /* StopRenderer 停止解码任务；帧回调此后不再触发。 */
    void StopRenderer();
    /* StartVideoUplink 经注入的启动闭包拉起相机管线。 */
    void StartVideoUplink();
    /* RequestVideoIDR 转发对端关键帧请求到上行视频管线（PLI/FIR 链路终点）。 */
    void RequestVideoIDR();
    /* StopVideoUplink 停止相机取流任务（编码器随之释放）。 */
    void StopVideoUplink();
#endif
    /* FireState 在互斥下投递状态回调（信令任务上下文）。 */
    void FireState(UiState state);
    /* FireRemoteFrame 在互斥下投递远端视频帧（解码任务上下文，缓冲仅回调期间有效）。 */
    void FireRemoteFrame(const std::uint8_t* rgb565, int width, int height);
    /* NotifyTrackState 投递本端麦克风/摄像头轨开关状态。 */
    void NotifyTrackState();
    /* MapState 把通话状态机阶段归并为页面粗粒度状态。 */
    static UiState MapState(ur::device::HumanCallState state);
};

/* PeerMedia 是 CallMediaPort 的 libpeer 实现：普通两轨 SDP（音频 sendrecv +
 * 视频 sendrecv，不用 aiVision 的三轨分离模式）。生命周期与通话腿一致，
 * Watcher在信令任务内串行编解码，回采归独立音频服务，避免视频期间PSRAM栈与算法任务争用。
 * 其他软件回采板型使用独立有界编解码任务。 */
class UrSignalingCall::Impl::PeerMedia final : public ur::device::CallMediaPort {
public:
    /* PeerState 镜像 libpeer ICE 连接状态，供 connected() 查询。 */
    enum class PeerState { kIdle, kConnecting, kConnected, kFailed };

    explicit PeerMedia(Impl& owner) : owner_(owner) {}
    ~PeerMedia() override { close(); }
    PeerMedia(const PeerMedia&) = delete;
    PeerMedia& operator=(const PeerMedia&) = delete;

    /* available 表示整组资源可预约：本端口空闲且 AI 语音腿不存活。 */
    bool available() const override { return !engaged_ && !owner_.HostBusy(); }
    // video_receive_supported 仅存在独立解码器时声明接收，配置仍由本次邀请轨授予。
    bool video_receive_supported() const override {
#if CONFIG_UR_AI_SIGNALING_WEBRTC_VIDEO
        return ur::device::GetCallVideoRenderer() != nullptr;
#else
        return false;
#endif
    }
    // configure_video_receive 在接听前冻结选定轨，不能由迟到RTP自行开启画面。
    bool configure_video_receive(bool enabled) override {
#if CONFIG_UR_AI_SIGNALING_WEBRTC_VIDEO
        if (enabled && !video_receive_supported())
            return false;
        owner_.remote_video_selected_ = enabled;
        return true;
#else
        return !enabled;
#endif
    }
    // voice_supported 显式支持无相机的纯语音候选，不借用视频资源预约。
    bool voice_supported() const override { return true; }
    // prepare_voice 只预约音频资源，固定参数与公共voice合同一致。
    bool prepare_voice(const ur::device::CallVoiceProfile& profile) override {
        if (engaged_ || owner_.HostBusy() || profile.sample_rate != 48000 ||
            profile.channels != 1 || profile.frame_duration_ms != 20)
            return false;
        owner_.voice_call_ = true;
        engaged_ = true;
        return true;
    }
    // begin_voice_candidate 只建立音频媒体，不因voice合同包含RTP时钟而升采样AFE输入。
    bool begin_voice_candidate(const ur::device::CallVoiceProfile&) override { return begin(); }
    /* prepare 在接听前预约资源；无上行视频能力时拒绝（平台邀请固定 video-1）。 */
    bool prepare() override {
        if (engaged_ || owner_.HostBusy() || !owner_.VideoUplinkReady()) {
            return false;
        }
        owner_.voice_call_ = false;
        engaged_ = true;
        return true;
    }
    /* begin 创建 libpeer Peer 并生成完整 offer（候选同步收集完成）。 */
    bool begin() override {
        if (!engaged_ || pc_ != nullptr) {
            return false;
        }
        call_codec_ = std::make_unique<ur::device::UrSignalingOpus>();
        if (!call_codec_->configure_for_call(ur::device::AudioFormat{16000, 1, 20}))
            return false;
#if CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER
        call_audio_.Reset();
        call_capture_audio_.Reset();
        ESP_LOGI(kTag, "Watcher call speech playback enabled");
#endif
#if CONFIG_UR_SOFTWARE_AEC && !CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER
        if (!EnsureCallDecoder() || !EnsureCallEncoder()) {
            StopCallEncoder();
            StopCallDecoder();
            return false;
        }
#endif
        if (udp_mode_)
            return true;
        return OpenPeer();
    }
    // configure_udp_av 只接受本次本地接听后选择，旧WebRTC仍走原Peer。
    bool configure_udp_av(bool enabled) override {
#if CONFIG_UR_HUMAN_CALL_UDP_AV
        if (!enabled || pc_ || udp_channel_)
            return false;
        udp_mode_ = enabled;
        return true;
#else
        return !enabled;
#endif
    }
    // IsUDP 给宿主选择对应相机/解码管线；不能从收到数据自行改变。
    bool IsUDP() const { return udp_mode_; }
    // StartUdpVideo 在相机就绪后启动独立视频任务，避免音频泵的调度间隙限制取帧。
    bool StartUdpVideo() {
#if CONFIG_UR_AI_SIGNALING_WEBRTC_VIDEO
        if (!udp_mode_ || !udp_channel_)
            return false;
        if (udp_video_task_)
            return true;
        udp_video_stopped_ = xSemaphoreCreateBinary();
        if (!udp_video_stopped_)
            return false;
        udp_video_stop_.store(false);
        if (xTaskCreatePinnedToCoreWithCaps(
                [](void* context) {
                    auto* self = static_cast<PeerMedia*>(context);
                    const auto stopped = self->udp_video_stopped_;
                    self->RunUdpVideo();
                    xSemaphoreGive(stopped);
                    vTaskDeleteWithCaps(nullptr);
                },
                "ur_udp_video", 12 * 1024, this, 4, &udp_video_task_,
                // 视频发送与相机任务同核，避免核1的信令和音频被轮询打断。
                0,
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
            vSemaphoreDelete(udp_video_stopped_);
            udp_video_stopped_ = nullptr;
            udp_video_task_ = nullptr;
            return false;
        }
        return true;
#else
        return false;
#endif
    }
    // StopUdpVideo 等待在途访问完成，调用者之后才能关闭相机或套接字。
    void StopUdpVideo() {
#if CONFIG_UR_AI_SIGNALING_WEBRTC_VIDEO
        if (!udp_video_task_)
            return;
        udp_video_stop_.store(true);
        xSemaphoreTake(udp_video_stopped_, portMAX_DELAY);
        vSemaphoreDelete(udp_video_stopped_);
        udp_video_stopped_ = nullptr;
        udp_video_task_ = nullptr;
#endif
    }
    // RunUdpVideo 仅发送最新JPEG；共享加密序号和套接字各由短锁保护，音频不等待整帧。
    void RunUdpVideo() {
#if CONFIG_UR_AI_SIGNALING_WEBRTC_VIDEO
        // 坦克的 DVP 摄像头与 Wi-Fi 共用 CPU 0；实测连续取帧可饿死空闲任务，
        // 先限制上行节拍保留调度余量。Watcher 沿用既有真机验证的 90ms。
#if CONFIG_BOARD_TYPE_ESP_SPARKBOT
        constexpr uint64_t kVideoIntervalMs = 1500;
#else
        constexpr uint64_t kVideoIntervalMs = 90;
#endif
        uint64_t last = 0;
        uint32_t camera_seq = 0;
        while (!udp_video_stop_.load()) {
#if CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER
            // 慢帧超过90ms节拍时也需阻塞一tick，给控制台及空闲任务留下调度窗口。
            vTaskDelay(1);
#endif
            const auto now = static_cast<uint64_t>(esp_timer_get_time() / 1000);
            if (last && now - last < kVideoIntervalMs) {
                // 100Hz tick 下 3ms 会舍入成零，循环让出至少一 tick 才能运行空闲任务。
                vTaskDelay(std::max<TickType_t>(1, pdMS_TO_TICKS(3)));
                continue;
            }
            auto* camera = Board::GetInstance().GetCamera();
            std::vector<uint8_t> image;
            if (!camera || !camera->TakeStreamJPEG(image, camera_seq)) {
                vTaskDelay(std::max<TickType_t>(1, pdMS_TO_TICKS(3)));
                continue;
            }
            last = now;
            std::vector<std::vector<uint8_t>> fragments;
            bool sealed;
            {
                // JPEG整批先预留互不重叠的CTR序号，再允许音频分配新序号。
                std::lock_guard<std::mutex> lock(udp_av_mutex_);
                sealed = udp_channel_ && udp_channel_->jpeg(image.data(), image.size(), fragments);
            }
            if (!sealed)
                continue;
            for (const auto& fragment : fragments) {
                if (udp_video_stop_.load())
                    break;
                bool ok;
                {
                    std::lock_guard<std::mutex> lock(udp_port_mutex_);
                    ok = udp_port_.send(fragment);
                }
                if (!ok)
                    break;
            }
        }
#endif
    }
    // ResetPlayback 作废两侧在途编码/解码，迟到结果不能进入重连后的媒体代次。
    void ResetPlayback() {
        downlink_.clear();
#if CONFIG_UR_SOFTWARE_AEC && !CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER
        ResetCallEncoding();
        {
            std::lock_guard<std::mutex> lock(call_decode_mutex_);
            ++call_decode_generation_;
            call_decode_waiting_.clear();
            call_decode_ready_.clear();
        }
        call_decode_cv_.notify_all();
#endif
    }
    // RestartCapture 只重启本地采集缓冲；恢复麦克风前丢弃静音期间的旧帧，不清空反向播放。
    bool RestartCapture() {
        if (!owner_.media_active_ || !owner_.pcm_)
            return false;
#if CONFIG_UR_SOFTWARE_AEC && !CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER
        ResetCallEncoding();  // 作废旧代次和待发结果，迟到编码不能越过恢复边界。
#endif
        owner_.pcm_->end_capture();
        return owner_.pcm_->begin_capture("mic-1");
    }

    // AudioUpFrames 返回本腿实际发送计数，供低吞吐停显判断。
    unsigned AudioUpFrames() const { return audio_up_; }
    // apply_udp_av_answer 核心已核验参数；安装后只发送静音探测，收到合法下行才报告连通。
    bool apply_udp_av_answer(const ur::device::CallUDPAVParameters& params) override {
        if (!udp_mode_ || !call_codec_ || udp_channel_ || !udp_cipher_.install(params.key) ||
            !udp_port_.open("0.0.0.0", 0) || !udp_port_.set_peer(params.server, params.port))
            return false;
        udp_channel_ = std::make_unique<ur::device::UdpAV>(udp_cipher_, params.up, params.down);
#if CONFIG_UR_AI_SIGNALING_WEBRTC_VIDEO && CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER
        watcher_video_gate_.Reset();
#endif
        state_ = PeerState::kConnecting;
        return true;
    }
    /* apply_answer 接受平台 mediaAnswer 的完整 answer SDP；
     * 拷贝落 PSRAM，避免数 KB 应答挤占内部 SRAM。 */
    bool apply_answer(std::string_view sdp) override {
        if (pc_ == nullptr || sdp.empty()) {
            return false;
        }
        if (!answer_sdp_.Assign(sdp.data(), sdp.size())) {
            ESP_LOGE(kTag, "call answer psram alloc failed (%d bytes)",
                     static_cast<int>(sdp.size()));
            return false;
        }
        peer_connection_set_remote_description(
            pc_, reinterpret_cast<const char*>(answer_sdp_.data()), SDP_TYPE_ANSWER);
        state_ = PeerState::kConnecting;
        /* 诊断：answer 内 ICE 候选行数（libpeer 配对输入）。真机曾出现通话 peer
         * 永远停在 CHECKING——候选数正常则指向平台候选地址/端口不可达。 */
        int candidates = 0;
        for (std::size_t pos = 0; (pos = sdp.find("a=candidate:", pos)) != std::string_view::npos;
             ++pos) {
            ++candidates;
        }
        ESP_LOGI(kTag, "call answer applied (%d bytes, %d candidates, remote video ssrc=%u)",
                 static_cast<int>(sdp.size()), candidates,
                 static_cast<unsigned>(peer_connection_remote_video_ssrc(pc_)));
        return true;
    }
    /* connected 只在 ICE/DTLS 全部连通后为真。 */
    bool connected() const override {
        return (pc_ != nullptr || udp_channel_ != nullptr) && state_ == PeerState::kConnected;
    }
    /* close 幂等销毁 Peer 与重采样器，并联动宿主停采集。 */
    void close() override {
        owner_.OnMediaClosed();
#if CONFIG_UR_AI_SIGNALING_WEBRTC_VIDEO
        jpeg_output_.reset();
#endif
        StopUdpVideo();
        udp_port_.close();
        udp_channel_.reset();
        udp_cipher_.clear();
#if CONFIG_UR_SOFTWARE_AEC && !CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER
        StopCallEncoder();
        StopCallDecoder();
#endif
        call_codec_.reset();
        engaged_ = false;
        offer_sdp_.Reset();
        answer_sdp_.Reset();
        offer_taken_ = false;
        if (pc_ != nullptr) {
            peer_connection_destroy(pc_);
            pc_ = nullptr;
        }
        downlink_.clear();
        decode_in_.clear();
        decode_in_.shrink_to_fit();
        state_ = PeerState::kIdle;
        owner_.OnMediaClosed();
    }
    /* Poll 推进 libpeer 状态机并泵双向音频与上行视频；仅信令任务调用。
     * 排空策略：libpeer 每轮 loop 只收一个包（agent_socket_recv 单次
     * recvfrom），10ms 一拍即 100pps 上限。通话中平台→设备为浏览器音频
     * (~50pps) + 视频 FU-A 分片（可达 100+pps）双向流，收包能力不足时
     * socket 队列溢出丢包——libpeer 自身 STUN 保活响应（3s 一发）被挤掉
     * 会触发 15s keepalive 超时误判 CLOSED（真机 ~30s 静默断链根因）。
     * CONNECTED 后禁用本任务短select等待，在包数和10ms耗时双预算内先排空，再泵音频，
     * 避免1ms select被lwIP取整后连续累积，也保护仅六包队列中的STUN；
     * CHECKING 期保持单轮，避免 conncheck 计数被加速导致候选对过早判失败。 */
    void Poll() {
#if CONFIG_UR_SOFTWARE_AEC && !CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER
        PumpCallEncoded();
#endif
        if (udp_mode_) {
            // 同窗记录媒体泵耗时和两次进入间隔，区分算法内等待与外层调度空窗。
            const auto started = static_cast<uint64_t>(esp_timer_get_time());
            if (udp_poll_at_us_)
                udp_poll_gap_max_us_ = std::max(udp_poll_gap_max_us_, started - udp_poll_at_us_);
            udp_poll_at_us_ = started;
            ++udp_poll_calls_;
            PollUDP();
            const auto elapsed = static_cast<uint64_t>(esp_timer_get_time()) - started;
            udp_poll_us_ += elapsed;
            udp_poll_max_us_ = std::max(udp_poll_max_us_, elapsed);
            TraceAudio();
            return;
        }
        if (pc_ == nullptr) {
            return;
        }
        const auto receive_started = esp_timer_get_time();  // 首次空收包等待也占本轮预算。
        PollPeerOnce();
        if (peer_connection_get_state(pc_) == PEER_CONNECTION_CONNECTED) {
            // 先领取突发中的控制包；编解码工作后置，采集预算仍覆盖同等长度的媒体突发。
            // 已连接时由PollPeerOnce禁用短等待；所有排空尝试仍共用10ms预算。
            for (int drain = 1; drain < 8 && state_ == PeerState::kConnected &&
                                esp_timer_get_time() - receive_started < 10000; ++drain)
                PollPeerOnce();
            for (int frame = 0; frame < 8 && state_ == PeerState::kConnected; ++frame) {
                // 编码之间继续排最多四包，避免音视频突发填满六包邮箱并挤掉STUN。
                for (int drain = 0; drain < 4 && state_ == PeerState::kConnected; ++drain)
                    PollPeerOnce();
                PumpCapture();
                PumpPlayback(4);
            }
            /* 下行解码器等 IDR 期间的 PLI 请求：libpeer 单线程模型内发送，
             * 平台侧桥已装配设备 PLI → 浏览器 RequestKeyFrame 的转发。 */
#if CONFIG_UR_AI_SIGNALING_WEBRTC_VIDEO
            auto* renderer = ur::device::GetCallVideoRenderer();
            if (renderer != nullptr && renderer->TakeKeyframeRequest()) {
                const int pli_rc = peer_connection_request_downlink_keyframe(pc_);
                if (pli_rc > 0) {
                    /* sendto 成功返回实际发送字节数，0 不是成功。 */
                    ESP_LOGI(kTag, "downlink keyframe PLI sent (%d bytes)", pli_rc);
                } else {
                    /* 失败通常是未连通或 answer SDP 未学习到远端视频 SSRC。 */
                    ESP_LOGW(kTag, "downlink keyframe PLI failed rc=%d ssrc=%u", pli_rc,
                             static_cast<unsigned>(peer_connection_remote_video_ssrc(pc_)));
                }
            }
#endif
        }
        PumpPlayback(8);
#if CONFIG_UR_AI_SIGNALING_H264_UPLINK
        PumpVideo();
#endif
        TraceAudio();
    }
    /* TakeOffer 一次性交出本地完整 offer SDP 的只读视图；
     * 缓冲归本端口所有（PSRAM），有效期为本次腿生命周期。 */
    std::optional<std::string_view> TakeOffer() {
        if (udp_mode_ && call_codec_ && !offer_taken_) {
            offer_taken_ = true;
            return std::string_view("udpAv");
        }
        if (offer_sdp_.size() == 0 || offer_taken_) {
            return std::nullopt;
        }
        offer_taken_ = true;
        return std::string_view(reinterpret_cast<const char*>(offer_sdp_.data()),
                                offer_sdp_.size());
    }

private:
    // PollPeerOnce 只在已连接人际媒体中禁用短select等待，保留原握手及保活状态机。
    void PollPeerOnce() {
        ur::device::CallPeerPollScope receive_scope(state_ == PeerState::kConnected);
        peer_connection_loop(pc_);
    }
    // EncodeCall 在Watcher人际上行抑制带外底噪，并记录整形和编码耗时。
    bool EncodeCall(std::vector<int16_t>& pcm, std::vector<uint8_t>& opus) {
        const auto started = esp_timer_get_time();  // 只度量当前算法及抢占等待，不称为端到端延迟。
#if CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER
        call_capture_audio_.Process(pcm);
#endif
        const bool ok = call_codec_->encode(pcm, opus);
        std::lock_guard<std::mutex> lock(decode_trace_mutex_);
        last_encode_core_ = xPortGetCoreID();
        encode_time_us_ += esp_timer_get_time() - started;
        ++encode_attempts_;
        if (!ok)
            ++encode_failures_;
        return ok;
    }
    // DecodeCall 解码本通话并记录整形在内的耗时；Watcher下行按已验参数约束人声峰值。
    bool DecodeCall(const std::vector<uint8_t>& opus, std::vector<int16_t>& pcm) {
        const auto started = esp_timer_get_time();
        const bool ok = call_codec_->decode(opus, pcm);
#if CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER
        if (ok)
            call_audio_.Process(pcm);
#endif
        std::lock_guard<std::mutex> lock(decode_trace_mutex_);
        last_decode_core_ = xPortGetCoreID();
        decode_time_us_ += esp_timer_get_time() - started;
        ++decode_attempts_;
        if (!ok)
            ++decode_failures_;
        return ok;
    }
    // ReadCapture 区分采集队列及组帧等待与上行编码成本，不改变原PCM格式。
    ur::device::PcmResult ReadCapture(std::vector<int16_t>& pcm) {
        const auto started = esp_timer_get_time();
        const auto result = owner_.pcm_->read_frame(pcm);
        std::lock_guard<std::mutex> lock(decode_trace_mutex_);
        capture_read_us_ += esp_timer_get_time() - started;
        ++capture_reads_;
        return result;
    }
    // WriteCallPCM 统计重采样、入队及票据移交耗时，完成仍以驱动回执为准。
    bool WriteCallPCM(const std::vector<int16_t>& pcm) {
        const auto started = esp_timer_get_time();
        const bool ok = owner_.pcm_->write_frame(pcm, owner_.playback_epoch_);
        playback_submit_us_ += esp_timer_get_time() - started;
        ++playback_submits_;
        return ok;
    }
    // SendCallUDP 只统计音频套接字发送，JPEG及探测不混入上行音频成本。
    bool SendCallUDP(const std::vector<uint8_t>& packet) {
        const auto started = esp_timer_get_time();
        bool ok;
        {
            std::lock_guard<std::mutex> lock(udp_port_mutex_);
            ok = udp_port_.send(packet);
        }
        audio_send_us_ += esp_timer_get_time() - started;
        ++audio_sends_;
        return ok;
    }
    // SendEncodedCall 只由媒体事件线程调用，维持libpeer单线程和UDP原安全参数。
    bool SendEncodedCall(std::vector<uint8_t>& opus) {
        if (!owner_.media_active_ || owner_.mic_paused_ || state_ != PeerState::kConnected)
            return false;
        bool sent = false;  // 真正发送成功才更新音频上行水位，算法成功不冒充网络成功。
        if (udp_mode_) {
            std::vector<uint8_t> packet;
            bool sealed = false;
            {
                std::lock_guard<std::mutex> lock(udp_av_mutex_);
                sealed = udp_channel_ && udp_channel_->audio(opus, packet);
            }
            sent = sealed && SendCallUDP(packet);
        } else {
            sent = pc_ && peer_connection_send_audio(pc_, opus.data(), opus.size()) == 0;
        }
        if (sent) {
#if CONFIG_UR_AI_SIGNALING_WEBRTC_VIDEO
            owner_.audio_capture_at_ = esp_timer_get_time() / 1000;
#endif
            if (++audio_up_ % 250 == 0)
                ESP_LOGI(kTag, "call uplink audio #%d", audio_up_);
        } else {
            ++audio_up_fail_;
        }
        return sent;
    }

    // TraceAudio 每五秒汇总计数与耗时；Watcher UDP 常规统计降为调试级别，避免串口阻塞媒体线程。
    // 异常计数仍以告警级别报告；完成水位来自驱动票据，不以接包或入队冒充播放。
    void TraceAudio() {
        if (!owner_.media_active_ || !owner_.pcm_) return;
        const auto now = esp_timer_get_time() / 1000;
        if (!audio_trace_at_) { audio_trace_at_ = now; return; }
        if (now - audio_trace_at_ < 5000) return;
        unsigned encode_n = 0, encode_failures = 0, decode_n = 0,
                 decode_failures = 0;  // 诊断快照在短锁内领取，日志不阻塞解码。
        std::uint64_t encode_us = 0, decode_us = 0, read_us = 0;
        unsigned read_n = 0;                     // 采集由工作任务推进，快照与清零都须持诊断锁。
        int encode_core = -1, decode_core = -1;  // 实际执行核，不由任务创建核推断。
        {
            std::lock_guard<std::mutex> trace_lock(decode_trace_mutex_);
            read_n = capture_reads_;
            read_us = capture_read_us_;
            capture_reads_ = 0;
            capture_read_us_ = 0;
            encode_n = encode_attempts_;
            encode_failures = encode_failures_;
            encode_us = encode_time_us_;
            encode_core = last_encode_core_;
            decode_core = last_decode_core_;
            encode_attempts_ = encode_failures_ = 0;
            encode_time_us_ = 0;
            decode_n = decode_attempts_;
            decode_failures = decode_failures_;
            decode_us = decode_time_us_;
            decode_attempts_ = decode_failures_ = 0;
            decode_time_us_ = 0;
        }
        // 只限制已实测受影响的 Watcher UDP；其他板型与 RTC 沿用原有信息级统计。
        esp_log_level_t trace_level = ESP_LOG_INFO;
#if CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER
        if (udp_mode_)
            trace_level = ESP_LOG_DEBUG;
#endif
        if (trace_level == ESP_LOG_DEBUG && (encode_failures || decode_failures ||
                                             audio_up_fail_ != audio_trace_reported_failures_)) {
            ESP_LOGW(kTag, "call audio failures encode=%u decode=%u uplink_total=%d",
                     encode_failures, decode_failures, audio_up_fail_);
        }
        audio_trace_reported_failures_ = audio_up_fail_;
        const auto& pcm = owner_.pcm_->underlying();  // 同一信令线程的只读水位。
        ESP_LOG_LEVEL_LOCAL(
            trace_level, kTag,
            "call audio span_ms=%llu encode_n=%u encode_avg_us=%llu decode_n=%u decode_avg_us=%llu "
            "encode_fail=%u decode_fail=%u wire_wait=%u play_done=%u play_pending=%u "
            "uplink_ok=%d uplink_fail=%d",
            static_cast<unsigned long long>(now - audio_trace_at_), encode_n,
            static_cast<unsigned long long>(encode_n ? encode_us / encode_n : 0), decode_n,
            static_cast<unsigned long long>(decode_n ? decode_us / decode_n : 0), encode_failures,
            decode_failures, static_cast<unsigned>(downlink_.size()),
            static_cast<unsigned>(pcm.completed_playback_frames()),
            static_cast<unsigned>(pcm.pending_playback_frames()), audio_up_, audio_up_fail_);
        // 同时记录事件线程实际执行核与固定核；不能由编解码任务参数推断网络泵调度。
        ESP_LOG_LEVEL_LOCAL(trace_level, kTag,
                            "call codec cores encode=%d decode=%d poll=%d affinity=%d", encode_core,
                            decode_core, xPortGetCoreID(), xTaskGetCoreID(nullptr));
        ESP_LOG_LEVEL_LOCAL(
            trace_level, kTag,
            "call port read_n=%u read_avg_us=%llu submit_n=%u submit_avg_us=%llu send_n=%u "
            "send_avg_us=%llu",
            read_n, static_cast<unsigned long long>(read_n ? read_us / read_n : 0),
            playback_submits_,
            static_cast<unsigned long long>(
                playback_submits_ ? playback_submit_us_ / playback_submits_ : 0),
            audio_sends_,
            static_cast<unsigned long long>(audio_sends_ ? audio_send_us_ / audio_sends_ : 0));
        if (udp_mode_) {
            ESP_LOG_LEVEL_LOCAL(
                trace_level, kTag, "call pump n=%u avg_us=%llu max_us=%llu gap_max_us=%llu",
                udp_poll_calls_,
                static_cast<unsigned long long>(udp_poll_calls_ ? udp_poll_us_ / udp_poll_calls_
                                                                : 0),
                static_cast<unsigned long long>(udp_poll_max_us_),
                static_cast<unsigned long long>(udp_poll_gap_max_us_));
            udp_poll_calls_ = 0;
            udp_poll_us_ = udp_poll_max_us_ = udp_poll_gap_max_us_ = 0;
            ESP_LOG_LEVEL_LOCAL(
                trace_level, kTag,
                "call rx n=%u avg_us=%llu max_us=%llu empty_n=%u empty_avg_us=%llu "
                "decrypt_n=%u decrypt_avg_us=%llu",
                udp_receive_calls_,
                static_cast<unsigned long long>(
                    udp_receive_calls_ ? udp_receive_us_ / udp_receive_calls_ : 0),
                static_cast<unsigned long long>(udp_receive_max_us_), udp_empty_calls_,
                static_cast<unsigned long long>(udp_empty_calls_ ? udp_empty_us_ / udp_empty_calls_
                                                                 : 0),
                udp_decrypt_calls_,
                static_cast<unsigned long long>(
                    udp_decrypt_calls_ ? udp_decrypt_us_ / udp_decrypt_calls_ : 0));
            udp_receive_us_ = udp_receive_max_us_ = udp_empty_us_ = udp_decrypt_us_ = 0;
            udp_receive_calls_ = udp_empty_calls_ = udp_decrypt_calls_ = 0;
        }
        playback_submit_us_ = audio_send_us_ = 0;
        playback_submits_ = audio_sends_ = 0;
        audio_trace_at_ = now;
#if CONFIG_UR_SOFTWARE_AEC && !CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER
        unsigned waiting = 0, ready = 0;            // 队列快照只用于定位背压，不借此消费或清理帧。
        bool busy = false, capture_active = false;  // 编码侧已无待搬PCM队列，记录实际武装状态。
        {
            std::lock_guard<std::mutex> lock(call_encode_mutex_);
            capture_active = call_capture_active_;
            ready = call_encode_ready_.size();
            busy = call_encode_busy_;
        }
        ESP_LOG_LEVEL_LOCAL(
            trace_level, kTag, "call encode capture_active=%d ready=%u busy=%d stack_free=%u",
            capture_active, ready, busy,
            static_cast<unsigned>(call_encode_task_ ? uxTaskGetStackHighWaterMark(call_encode_task_)
                                                    : 0));
        {
            std::lock_guard<std::mutex> lock(call_decode_mutex_);
            waiting = call_decode_waiting_.size();
            ready = call_decode_ready_.size();
            busy = call_decode_busy_;
        }
        ESP_LOG_LEVEL_LOCAL(trace_level, kTag, "call decode wait=%u ready=%u busy=%d rejected=%u",
                            waiting, ready, busy, playback_rejected_);
#endif
    }

#if CONFIG_UR_SOFTWARE_AEC && !CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER
    // EnsureCallEncoder 懒建独立有界编码任务；算法不再占网络/PCM事件线程。
    // Core0麦克风输入实测繁忙，显式选算法核避免浮点首次执行时选核不确定；单核回0。
    bool EnsureCallEncoder() {
        if (call_encode_task_)
            return true;
        call_encode_stopped_ = xSemaphoreCreateBinary();
        if (!call_encode_stopped_)
            return false;
        call_event_task_ = xTaskGetCurrentTaskHandle();  // 创建者即唯一媒体I/O线程，借用至退出屏障。
        call_encode_shutdown_ = false;
        if (xTaskCreatePinnedToCoreWithCaps(
                [](void* context) {
                    auto* self = static_cast<PeerMedia*>(context);  // 完成屏障后不再借用本对象。
                    const auto stopped = self->call_encode_stopped_;
                    self->RunCallEncoder();
                    xSemaphoreGive(stopped);
                    vTaskDeleteWithCaps(nullptr);
                },
                "ur_call_encode", 24 * 1024, this, 4, &call_encode_task_,
                (CONFIG_FREERTOS_NUMBER_OF_CORES > 1 ? 1 : 0),
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
            vSemaphoreDelete(call_encode_stopped_);
            call_encode_stopped_ = nullptr;
            call_encode_task_ = nullptr;
            call_event_task_ = nullptr;
            return false;
        }
        return true;
    }
    // StopCallEncoder 等在途算法和队列访问完成，再允许销毁本腿编解码器。
    void StopCallEncoder() {
        if (!call_encode_task_)
            return;
        {
            std::lock_guard<std::mutex> lock(call_encode_mutex_);
            call_encode_shutdown_ = true;
            ++call_encode_generation_;
            call_capture_active_ = false;
            call_encode_ready_.clear();
        }
        call_encode_cv_.notify_all();
        xSemaphoreTake(call_encode_stopped_, portMAX_DELAY);
        vSemaphoreDelete(call_encode_stopped_);
        call_encode_stopped_ = nullptr;
        call_encode_task_ = nullptr;
        call_event_task_ = nullptr;
    }
    // RunCallEncoder 自行领取AFE样本并编码，不依赖网络/JPEG轮询推进采集。
    // 四帧结果背压及一帧在途保持；读帧/等待/DSP均在短锁外，代次隔离关闭前的迟到结果。
    void RunCallEncoder() {
        while (true) {
            uint64_t generation = 0;  // 仅借用当前采集代次，静音或关闭后不能提交旧结果。
            {
                std::unique_lock<std::mutex> lock(call_encode_mutex_);
                call_encode_cv_.wait(lock, [this] {
                    return call_encode_shutdown_ ||
                           (call_capture_active_ && call_encode_ready_.size() < 4);
                });
                if (call_encode_shutdown_)
                    return;
                generation = call_encode_generation_;
                call_encode_busy_ = true;
            }
            std::vector<int16_t> pcm;  // PCM桥串行采集起止，工作任务不读取事件线程的开麦状态。
            const auto result = ReadCapture(pcm);
            std::vector<uint8_t> opus;  // 编码后仍只交原媒体事件线程发送，套接字保持单线程。
            const bool frame =
                result == ur::device::PcmResult::Frame && pcm.size() == kIn16kSamples;
            const bool ok = frame && EncodeCall(pcm, opus);
            bool notify_result = false;  // 释放队列锁后才唤醒事件线程，避免抢占后争锁。
            bool wait_capture = false;  // 先耗尽PCM暂存尾帧，空时再睡眠等待生产者，避免忙等。
            {
                std::lock_guard<std::mutex> lock(call_encode_mutex_);
                call_encode_busy_ = false;
                if (!call_encode_shutdown_ && call_capture_active_ &&
                    generation == call_encode_generation_) {
                    if (ok && !opus.empty()) {
                        call_encode_ready_.push_back(std::move(opus));
                        notify_result = true;
                    }
                    wait_capture = !frame;
                }
            }
            if (notify_result && call_event_task_)
                xTaskNotifyGive(call_event_task_);
            if (wait_capture)
                owner_.pcm_->WaitCapture();
        }
    }
    // ResetCallEncoding 隔离取消/静音/重连前的在途帧，不重放旧麦克风内容。
    void ResetCallEncoding() {
        {
            std::lock_guard<std::mutex> lock(call_encode_mutex_);
            ++call_encode_generation_;
            call_capture_active_ = false;
            call_encode_ready_.clear();
        }
        call_encode_cv_.notify_all();
    }
    // QueueCallCapture 只武装独立采集任务；返回首次启动且结果仍有容量，不在网络线程搬帧。
    bool QueueCallCapture() {
        if (!owner_.pcm_ || !owner_.media_active_ || owner_.mic_paused_)
            return false;
        bool activated = false;  // 重复轮询不制造虚假采集任务或无界唤醒。
        {
            std::lock_guard<std::mutex> lock(call_encode_mutex_);
            activated = !call_capture_active_ && call_encode_ready_.size() < 4;
            call_capture_active_ = true;
        }
        call_encode_cv_.notify_one();
        return activated;
    }
    // PumpCallEncoded 使用四帧容量，逐包收网防六包邮箱溢出，批尾才唤醒算法任务。
    // 避免每包之间被编解码抢占；发送仍受10ms合作预算限制，余帧留待下次调度。
    // 静音即时作废待发结果；工作任务不触碰libpeer/UDP，网络始终由原事件线程推进。
    void PumpCallEncoded() {
        if (!owner_.media_active_)
            return;
        if (owner_.mic_paused_) {
            ResetCallEncoding();
            return;
        }
        const auto started = esp_timer_get_time();  // 收包与发送仍可能被其他高优先级任务抢占。
        bool released_capacity = false;  // 批内不唤醒等待满队列的编码器，避免每包一次算法折返。
        for (int i = 0; i < 4 && (i == 0 || esp_timer_get_time() - started < 10000); ++i) {
            std::vector<uint8_t> opus;  // 移出短锁后只在原媒体线程发网。
            {
                std::lock_guard<std::mutex> lock(call_encode_mutex_);
                if (call_encode_ready_.empty())
                    break;
                opus = std::move(call_encode_ready_.front());
                call_encode_ready_.pop_front();
            }
            released_capacity = true;
            SendEncodedCall(opus);
#if CONFIG_UR_AI_SIGNALING_WEBRTC_VIDEO
            if (udp_mode_) {
                ReceiveUDP(4);
            }
#endif
        }
        if (released_capacity) {
            call_encode_cv_.notify_one();
#if CONFIG_UR_AI_SIGNALING_WEBRTC_VIDEO
            if (udp_mode_)
                PumpPlayback(1);  // UDP 每轮只递交一帧，优先让信令任务继续排空视频分片。
#endif
        }
    }

    // EnsureCallDecoder 只为当前通话懒建任务，固定算法核并在失败时释放本腿。
    bool EnsureCallDecoder() {
        if (call_decode_task_)
            return true;
        call_decode_stopped_ = xSemaphoreCreateBinary();
        if (!call_decode_stopped_)
            return false;
        call_decode_shutdown_ = false;
        if (xTaskCreatePinnedToCoreWithCaps(
                [](void* context) {
                    auto* self = static_cast<PeerMedia*>(context);  // 完成屏障后不再借用本对象。
                    const auto stopped = self->call_decode_stopped_;
                    self->RunCallDecoder();
                    xSemaphoreGive(stopped);
                    vTaskDeleteWithCaps(nullptr);
                },
                "ur_call_decode", 24 * 1024, this, 4, &call_decode_task_,
                (CONFIG_FREERTOS_NUMBER_OF_CORES > 1 ? 1 : 0),
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
            vSemaphoreDelete(call_decode_stopped_);
            call_decode_stopped_ = nullptr;
            call_decode_task_ = nullptr;
            return false;
        }
        return true;
    }
    // StopCallDecoder 等当前有界算法和队列访问退出后，调用者才能销毁编解码器。
    void StopCallDecoder() {
        if (!call_decode_task_)
            return;
        {
            std::lock_guard<std::mutex> lock(call_decode_mutex_);
            call_decode_shutdown_ = true;
            ++call_decode_generation_;
            call_decode_waiting_.clear();
            call_decode_ready_.clear();
        }
        call_decode_cv_.notify_all();
        xSemaphoreTake(call_decode_stopped_, portMAX_DELAY);
        vSemaphoreDelete(call_decode_stopped_);
        call_decode_stopped_ = nullptr;
        call_decode_task_ = nullptr;
    }
    // RunCallDecoder 只执行Opus和有界队列；网络、PCM票据及状态机仍归媒体事件线程。
    void RunCallDecoder() {
        while (true) {
            CallDecodeRequest request;  // 移出短锁后执行算法，避免上行编码等待解码锁。
            {
                std::unique_lock<std::mutex> lock(call_decode_mutex_);
                call_decode_cv_.wait(lock, [this] {
                    return call_decode_shutdown_ ||
                           (!call_decode_waiting_.empty() && call_decode_ready_.size() < 4);
                });
                if (call_decode_shutdown_)
                    return;
                request = std::move(call_decode_waiting_.front());
                call_decode_waiting_.pop_front();
                call_decode_busy_ = true;
            }
            std::vector<int16_t> pcm;  // 最多一帧在途，结果必须仍属于当前播放代次。
            const bool ok = DecodeCall(request.opus, pcm);
            {
                std::lock_guard<std::mutex> lock(call_decode_mutex_);
                call_decode_busy_ = false;
                if (!call_decode_shutdown_ && request.generation == call_decode_generation_ && ok &&
                    !pcm.empty())
                    call_decode_ready_.push_back(std::move(pcm));
            }
        }
    }
    // PumpCallDecoded 有背压时保留结果，完成票据由原事件线程领取。
    void PumpCallDecoded() {
        owner_.pcm_->PumpPlayback();
        for (int i = 0; i < 4 && !owner_.pcm_->playback_backpressured(owner_.playback_epoch_);
             ++i) {
            std::vector<int16_t> pcm;
            {
                std::lock_guard<std::mutex> lock(call_decode_mutex_);
                if (call_decode_ready_.empty())
                    break;
                pcm = std::move(call_decode_ready_.front());
                call_decode_ready_.pop_front();
            }
            call_decode_cv_.notify_one();
            if (!WriteCallPCM(pcm))
                ++playback_rejected_;
        }
    }
#endif

    // ReceiveUDP 有界领取网络突发；每帧编码之间再次调用，避免六包邮箱等整批编码后才排空。
    void ReceiveUDP(int limit) {
#if CONFIG_UR_AI_SIGNALING_WEBRTC_VIDEO
        auto& packet = udp_receive_packet_;  // 事件线程独占复用数据报空间，不反复分配空轮询缓冲。
        std::vector<uint8_t> audio, image;  // 解密结果仍只在本次领取期间持有，生产队列保持有界。
        for (int i = 0; i < limit; ++i) {
            const auto receive_started = esp_timer_get_time();  // 包含实际socket调用及其调度等待。
            ur::device::ReceiveResult received;
            {
                std::lock_guard<std::mutex> lock(udp_port_mutex_);
                received = udp_port_.receive(packet);
            }
            const auto receive_us = esp_timer_get_time() - receive_started;
            ++udp_receive_calls_;
            udp_receive_us_ += receive_us;
            udp_receive_max_us_ = std::max<uint64_t>(udp_receive_max_us_, receive_us);
            if (received != ur::device::ReceiveResult::Received) {
                ++udp_empty_calls_;
                udp_empty_us_ += receive_us;
                break;
            }
            const auto decrypt_started = esp_timer_get_time();  // 格式校验、CTR及视频重组与收包分开。
            // 每包按领取时刻计时；批次内被抢占或处理音频时不能沿用过期时间。
            const auto now = static_cast<uint64_t>(decrypt_started / 1000);
#if CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER
            // 已交付完整画面后短暂跳过视频包，为音频接收和采集保留处理时间。
            if ((jpeg_output_ && jpeg_output_->WaitingForDma() &&
                 WatcherUdpVideoGate::IsVideoPacket(packet)) ||
                watcher_video_gate_.ShouldSkip(packet, now))
                continue;
#endif
            bool valid;
            {
                std::lock_guard<std::mutex> lock(udp_av_mutex_);
                valid = udp_channel_->receive(packet, now, audio, image);
            }
            ++udp_decrypt_calls_;
            udp_decrypt_us_ += esp_timer_get_time() - decrypt_started;
            if (!valid)
                continue;
            if (!audio.empty()) {
                state_ = PeerState::kConnected;
                if (owner_.media_active_) {
                    // UDP突发与播放驱动异步，先暂存原包，再由有背压的播放泵交付。
                    OnAudioTrack(audio.data(), audio.size(), this);
                }
            }
#if CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER
            if (!image.empty())
                watcher_video_gate_.OnFrame(now);
#endif
            if (!image.empty() && owner_.remote_video_selected_ && owner_.media_active_) {
                ++udp_jpeg_frames_;
                if (udp_jpeg_frames_ % 10 == 1) {
                    ESP_LOGI(kTag, "call udp jpeg frame #%u size=%u",
                             udp_jpeg_frames_, static_cast<unsigned>(image.size()));
                }
                if (!jpeg_output_) {
                    jpeg_output_ = std::make_unique<UrCallJpegOutput>();
                    if (!jpeg_output_->Start([this](const uint8_t* data, int w, int h) {
                            owner_.FireRemoteFrame(data, w, h);
                        }))
                        jpeg_output_.reset();
                }
                if (jpeg_output_) {
                    jpeg_output_->Submit(std::move(image));
                    if (jpeg_output_->Disabled())
                        owner_.remote_video_disabled_.store(true);
                }
            }
        }
#else
        (void)limit;
#endif
    }

    // PollUDP 有界排空媒体、按同等八帧预算泵音频并最多发送一帧JPEG；真正采集仍等Connected和平台Established。
    void PollUDP() {
#if CONFIG_UR_AI_SIGNALING_WEBRTC_VIDEO
        if (!udp_channel_)
            return;
        const auto started = esp_timer_get_time();  // 八轮数量上限之外，持续负载不能拖住控制事件。
        const auto now = static_cast<uint64_t>(started / 1000);
        std::vector<uint8_t> packet, audio;
        if ((!connected() || owner_.mic_paused_) &&
            (!udp_probe_at_ || now - udp_probe_at_ >= 200)) {
            udp_probe_at_ = now;
            bool sealed;
            {
                std::lock_guard<std::mutex> lock(udp_av_mutex_);
                sealed = udp_channel_->audio({0xf8, 0xff, 0xfe}, packet);
            }
            if (sealed) {
                std::lock_guard<std::mutex> lock(udp_port_mutex_);
                udp_port_.send(packet);
            }
        }
        // 先一次性排空较大的UDP突发：网页视频每帧由多片JPEG组成，不能让音频
        // 编解码把首片和尾片分散到多个轮询窗口，导致整帧在200ms重组期限内过期。
        ReceiveUDP(32);
        if (!owner_.media_active_ || !owner_.pcm_)
            return;
        owner_.pcm_->PumpPlayback();
        PumpPlayback(1);
#if CONFIG_UR_SOFTWARE_AEC && !CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER
        PumpCallEncoded();
#endif
        if (!owner_.mic_paused_) {
            std::vector<int16_t> pcm;
            // 音频帧每20ms产生一次；允许连续突发在90ms内被消费，避免正式配置下音频只跑到约28fps。
            // JPEG任务低于AFE后，延长窗口不会让视频解码抢占音频处理。
            for (int i = 0; i < 8 && (i == 0 || esp_timer_get_time() - started < 90000); ++i) {
#if CONFIG_UR_SOFTWARE_AEC && !CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER
                const auto received_before =
                    udp_decrypt_calls_;              // 是否仍有网络工作，不由采集武装结果推断。
                const auto sent_before = audio_up_;  // 只比较实际发包推进，空结果不强行忙等。
#endif
                ReceiveUDP(4);
#if CONFIG_UR_SOFTWARE_AEC && !CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER
                QueueCallCapture();
                PumpCallEncoded();
                PumpPlayback(1);
                if (received_before == udp_decrypt_calls_ && sent_before == audio_up_ &&
                    downlink_.empty())
                    break;
#else
                PumpPlayback(1);  // 同步UDP也消费暂存包，满时保留到驱动容量释放。
                if (ReadCapture(pcm) != ur::device::PcmResult::Frame)
                    break;
                if (EncodeCall(pcm, audio))
                    SendEncodedCall(audio);
#endif
            }
        }
        if (jpeg_output_ && owner_.remote_video_disabled_.load())
            jpeg_output_->ProtectAudio();
#endif
    }
    /* OpenPeer 创建 Peer（STUN 与 aiVision 复用同一可达配置）并生成 offer。 */
    bool OpenPeer() {
        static const char* kStunUrls[] = {"stun:stun.miwifi.com:3478"};
        PeerConfiguration config = {};
        for (std::size_t i = 0; i < sizeof(kStunUrls) / sizeof(kStunUrls[0]); ++i) {
            config.ice_servers[i].urls = kStunUrls[i];
        }
        config.audio_codec = CODEC_OPUS;
#if CONFIG_UR_AI_SIGNALING_WEBRTC_VIDEO
        /* 普通两轨：libpeer 默认 SDP 音频/视频均为 sendrecv（sdp.c sdp_append_h264）。 */
        if (!owner_.voice_call_)
            config.video_codec = CODEC_H264;
#endif
        config.datachannel = DATA_CHANNEL_NONE;
        config.onaudiotrack = &PeerMedia::OnAudioTrack;
#if CONFIG_UR_AI_SIGNALING_WEBRTC_VIDEO
        /* rtp_decode_h264 输出完整 Annex-B NALU，直接喂通话解码渲染器。 */
        config.onvideotrack = &PeerMedia::OnVideoTrack;
        /* 对端（浏览器）PLI/FIR 请求关键帧：libpeer 仅在注册本回调时转发，
         * 缺失则设备永不响应关键帧请求（真机 pliCount=247 仍 0 帧解码实证）。 */
        config.on_request_keyframe = &PeerMedia::OnRequestKeyframe;
#endif
        config.user_data = this;
        pc_ = peer_connection_create(&config);
        if (pc_ == nullptr) {
            ESP_LOGE(kTag, "call peer_connection_create failed");
            return false;
        }
        peer_connection_oniceconnectionstatechange(pc_, &PeerMedia::OnIceState);
        /* 上游 agent 候选同步收集，create_offer 返回完整 SDP（iceMode=complete 语义）。 */
        const char* offer = peer_connection_create_offer(pc_);
        if (offer == nullptr) {
            ESP_LOGE(kTag, "call create_offer failed");
            close();
            return false;
        }
        const std::size_t offer_len = std::strlen(offer);
        /* offer SDP 可达数 KB，拷入 PSRAM，避免内部 SRAM 峰值被 SDP 吃掉。 */
        if (!offer_sdp_.Assign(offer, offer_len)) {
            ESP_LOGE(kTag, "call offer psram alloc failed (%d bytes)", static_cast<int>(offer_len));
            close();
            return false;
        }
        ESP_LOGI(kTag, "call local offer ready (%d bytes)", static_cast<int>(offer_sdp_.size()));
        return true;
    }
    /* OnAudioTrack 缓存下行 opus 帧（libpeer loop 线程写入，Poll 消费）；
     * 帧拷贝落 PSRAM，UDP暂存四帧、WebRTC暂存一百帧，限制积压与内部SRAM峰值。 */
    static void OnAudioTrack(uint8_t* data, size_t size, void* userdata) {
        auto* self = static_cast<PeerMedia*>(userdata);
        if (data == nullptr || size == 0) {
            return;
        }
        PsramBuffer frame;
        if (!frame.Assign(data, size)) {
            return; /* PSRAM 不足直接丢帧保实时。 */
        }
        // UDP只暂存四个20ms包；持续拥塞时丢旧保实时，原WebRTC保持既有上限。
        const std::size_t limit = self->udp_mode_ ? 4 : kDownlinkLimit;
        if (self->downlink_.size() >= limit) {
            self->downlink_.pop_front();
        }
        self->downlink_.push_back(std::move(frame));
    }
#if CONFIG_UR_AI_SIGNALING_WEBRTC_VIDEO
    /* OnVideoTrack 把 Annex-B NALU 喂解码渲染器（渲染器内部有界入队，满则丢旧）。 */
    static void OnVideoTrack(uint8_t* data, size_t size, void* userdata) {
        auto* self = static_cast<PeerMedia*>(userdata);
        if (data == nullptr || size == 0 || !self->owner_.remote_video_selected_ ||
            self->owner_.remote_video_disabled_.load() || !self->owner_.renderer_started_) {
            return;
        }
        auto* renderer = ur::device::GetCallVideoRenderer();
        if (renderer != nullptr) {
            renderer->OnNalu(data, size);
        }
    }
    /* OnRequestKeyframe 为 libpeer PLI/FIR 回调（信令任务上下文）：转发到
     * 上行视频管线，下一帧强制 IDR（节流由管线内部保证）。 */
    static void OnRequestKeyframe(void* userdata) {
        auto* self = static_cast<PeerMedia*>(userdata);
        self->owner_.RequestVideoIDR();
    }
#endif
    /* OnIceState 镜像 libpeer 连接状态（loop 线程回调；仅本任务驱动的 libpeer
     * 单线程模型内触发，与 Poll 同任务无竞争）。 */
    static void OnIceState(PeerConnectionState state, void* userdata) {
        auto* self = static_cast<PeerMedia*>(userdata);
        ESP_LOGI(kTag, "call libpeer state -> %d", static_cast<int>(state));
        switch (state) {
            case PEER_CONNECTION_CONNECTED:
                self->state_ = PeerState::kConnected;
                break;
            case PEER_CONNECTION_DISCONNECTED:
                self->state_ = PeerState::kConnecting;
                break;
            case PEER_CONNECTION_FAILED:
            case PEER_CONNECTION_CLOSED:
                self->state_ = PeerState::kFailed;
                break;
            default:
                break;
        }
    }
    /* PumpCapture 领取AFE采集并发送Opus，RTP时钟仍48k。
     * Watcher与无回采构建在媒体任务内编码，其他回采板型通过独立有界任务交付。 */
    void PumpCapture() {
        if (!owner_.pcm_ || !call_codec_ || !owner_.media_active_ || owner_.mic_paused_)
            return;
#if CONFIG_UR_SOFTWARE_AEC && !CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER
        QueueCallCapture();
        PumpCallEncoded();
#else
        std::vector<std::int16_t> pcm;
        if (ReadCapture(pcm) != ur::device::PcmResult::Frame || pcm.size() != kIn16kSamples)
            return;
        std::vector<std::uint8_t> opus;
        if (EncodeCall(pcm, opus) && !opus.empty())
            SendEncodedCall(opus);
#endif
    }
    /* PumpPlayback 解码下行队列并写入共享 PCM 播放端口（解码输出固定 16k）。
     * media_active_ 未置位（连通瞬间的同一轮）先排队，待 begin_playback 后再写。
     * Watcher与无回采构建同步解码，先确认播放容量。其他回采板型使用四帧有界任务。 */
    void PumpPlayback(int limit = 1) {
        if (owner_.pcm_ == nullptr || !call_codec_ || !owner_.media_active_) {
            return;
        }
#if CONFIG_UR_SOFTWARE_AEC && !CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER
        PumpCallDecoded();
#else
        // 同步解码前先领取驱动完成票据，满时保留压缩包供下一轮播放。
        owner_.pcm_->PumpPlayback();
#endif
        for (int i = 0; i < limit && !downlink_.empty(); ++i) {
            auto& frame = downlink_.front();
#if CONFIG_UR_SOFTWARE_AEC && !CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER
            {
                std::lock_guard<std::mutex> lock(call_decode_mutex_);
                if (call_decode_waiting_.size() >= 4)
                    break;
                call_decode_waiting_.push_back(
                    {std::vector<uint8_t>(frame.data(), frame.data() + frame.size()),
                     call_decode_generation_});
            }
            downlink_.pop_front();
            call_decode_cv_.notify_one();
#else
            // 解码会推进Opus历史，必须在取包前确认播放容量，避免入队失败后丢失整帧。
            if (owner_.pcm_->playback_backpressured(owner_.playback_epoch_))
                break;
            decode_in_.assign(frame.data(), frame.data() + frame.size());
            downlink_.pop_front();
            std::vector<std::int16_t> pcm;
            if (!DecodeCall(decode_in_, pcm) || pcm.empty()) {
                continue;
            }
            WriteCallPCM(pcm);
#endif
        }
#if CONFIG_UR_SOFTWARE_AEC && !CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER
        PumpCallDecoded();
#endif
    }
#if CONFIG_UR_AI_SIGNALING_H264_UPLINK
    /* PumpVideo 把相机管线已编码 NALU 经本 Peer 上行；沿用 aiVision 实测的
     * 帧级限速（≥200ms）与大小上限（8KB），保护 STUN keepalive 不断链。 */
    void PumpVideo() {
        if (owner_.voice_call_ || owner_.video_ == nullptr || owner_.camera_paused_ ||
            state_ != PeerState::kConnected) {
            return;
        }
        /* 帧级限速：仅"距上次发送不足200ms"才跳过；首次(video_send_at_==0)必须放行，
         * 否则首帧永达不了发送路径、video_send_at_ 永不置位（真机视频0上行根因）。 */
        if (video_send_at_ && esp_timer_get_time() - video_send_at_ < 200 * 1000LL) {
            return;
        }
        std::vector<std::uint8_t> nalu;
        if (owner_.video_->TakeFrame(nalu)) {
            /* 上限 12KB：SPS/PPS 前置后 IDR 约 8.8-9KB，旧 8KB 上限把所有关键帧
             * 全部丢弃（真机 pli 风暴+0 帧解码实证）；700B 分片下 9KB 约 13 片
             * 无突发风险。 */
            if (nalu.size() > 12288) {
                return;
            }
            video_send_at_ = esp_timer_get_time();
            if (peer_connection_send_video(pc_, nalu.data(), nalu.size()) != 0) {
                return;
            }
            ++owner_.video_uplink_frames_;
            if (owner_.video_uplink_frames_ % 10 == 1) {
                ESP_LOGI(kTag, "call uplink video frame #%d size=%d", owner_.video_uplink_frames_,
                         static_cast<int>(nalu.size()));
            }
        }
    }
#endif

    bool udp_mode_ = false;                           // 本次严格选择的媒体协议。
    uint64_t udp_probe_at_ = 0;                       // 静音探测单调时钟。
    std::mutex udp_port_mutex_, udp_av_mutex_;        // 分别保护共享套接字和CTR媒体序号。
    std::atomic<bool> udp_video_stop_{true};          // 停止意图，供视频任务跨核读取。
    TaskHandle_t udp_video_task_ = nullptr;            // 本次视频腿独占的任务。
    SemaphoreHandle_t udp_video_stopped_ = nullptr;    // 相机和网络资源释放前的退出屏障。
    ur::device::UrSignalingUdpPort udp_port_;         // 已授权单一服务器地址。
    std::vector<uint8_t> udp_receive_packet_;  // 仅媒体事件线程使用，随通话腿释放。
    ur::device::UrSignalingCipher udp_cipher_;        // 生命周期随当前通话。
    std::unique_ptr<ur::device::UdpAV> udp_channel_;  // 独立方向与步长128协议。
    std::unique_ptr<ur::device::UrSignalingOpus>
        call_codec_;  // 本腿16k/48k输入独立实例，不改变共享AI编解码器。
#if CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER
    WatcherCallAudio call_audio_;  // 仅当前Watcher人际下行使用，随通话腿释放。
    WatcherCallCaptureAudio call_capture_audio_;  // 仅当前Watcher人际上行使用，与下行历史隔离。
#endif
#if CONFIG_UR_AI_SIGNALING_WEBRTC_VIDEO
    std::unique_ptr<UrCallJpegOutput> jpeg_output_;  // 最新帧解码器，慢帧永久停显到结束。
    unsigned udp_jpeg_frames_ = 0;                   // 已完成UDP JPEG重组的帧数。
#endif
    Impl& owner_;
    PeerConnection* pc_ = nullptr;
    PeerState state_ = PeerState::kIdle;
    bool engaged_ = false;
    bool offer_taken_ = false;
    /* offer_sdp_/answer_sdp_ 为 SDP 大缓冲（PSRAM），生命周期随本端口。 */
    PsramBuffer offer_sdp_;
    PsramBuffer answer_sdp_;
    /* downlink_ 缓存下行 opus 帧（回调写入 PSRAM，poll 消费）。 */
    std::deque<PsramBuffer> downlink_;
    /* decode_in_ 为解码输入的复用暂存（Opus 单帧上限 1275 字节）。 */
    std::vector<std::uint8_t> decode_in_;
    /* audio_up_/audio_up_fail_ 为上行音频发送成功/失败计数（信令任务内读写）。 */
    int audio_up_ = 0;
    int audio_up_fail_ = 0;
    int audio_trace_reported_failures_ = 0;  // 避免同一历史发送失败反复触发告警。
    std::uint64_t capture_read_us_ = 0, playback_submit_us_ = 0,
                  audio_send_us_ = 0;  // 媒体线程端口计时，非声学延迟。
    unsigned capture_reads_ = 0, playback_submits_ = 0,
             audio_sends_ = 0;       // 每个诊断窗口的实际调用次数。
    uint64_t udp_receive_us_ = 0, udp_receive_max_us_ = 0, udp_empty_us_ = 0,
             udp_decrypt_us_ = 0;  // socket等待、空读和协议校验分开，包含调度墙钟。
    unsigned udp_receive_calls_ = 0, udp_empty_calls_ = 0,
             udp_decrypt_calls_ = 0;  // 每秒窗口内调用事实，不充当播放帧数。
    uint64_t udp_poll_at_us_ = 0, udp_poll_us_ = 0, udp_poll_max_us_ = 0,
             udp_poll_gap_max_us_ = 0;  // 同窗媒体泵与外层进入间隔，包含抢占但不称为纯算法耗时。
    unsigned udp_poll_calls_ = 0;       // 五秒诊断窗口内实际UDP媒体泵次数。
#if CONFIG_UR_AI_SIGNALING_WEBRTC_VIDEO && CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER
    WatcherUdpVideoGate watcher_video_gate_;  // 本通话视频收包节拍，不限速音频。
#endif
    std::mutex decode_trace_mutex_;     // 保护两侧编解码计时，不跨越算法调用或队列操作。
    int last_encode_core_ = -1, last_decode_core_ = -1;  // 算法完成时读取的实际CPU核。
#if CONFIG_UR_SOFTWARE_AEC && !CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER
    std::mutex call_encode_mutex_;            // 保护采集代次、四帧结果及一帧在途，算法在锁外。
    std::condition_variable call_encode_cv_;  // 未武装或结果满时阻塞，空采集由AFE通知唤醒。
    bool call_capture_active_ = false;        // 事件线程武装/作废，工作线程仅在短锁内查询。
    std::deque<std::vector<uint8_t>> call_encode_ready_;  // 最多四帧待原事件线程发送。
    uint64_t call_encode_generation_ = 0;                 // 仅主媒体线程递增，工作线程核对。
    bool call_encode_shutdown_ = false, call_encode_busy_ = false;  // 结束意图和实际在途状态。
    TaskHandle_t call_encode_task_ = nullptr;
    TaskHandle_t call_event_task_ = nullptr;  // 借用创建者任务，StopCallEncoder退出屏障后才清空。          // 生命周期随本腿，AI不创建此任务。
    SemaphoreHandle_t call_encode_stopped_ = nullptr;  // 算法和队列访问全部结束屏障。
    // CallDecodeRequest 持有压缩帧和媒体播放代次，重连或关闭后迟到结果必须丢弃。
    struct CallDecodeRequest {
        std::vector<uint8_t> opus;
        std::uint64_t generation;
    };
    std::mutex call_decode_mutex_;                       // 同步两侧四帧队列及代次，算法在锁外执行。
    std::condition_variable call_decode_cv_;             // 无输入或结果满时阻塞，禁止忙等。
    std::deque<CallDecodeRequest> call_decode_waiting_;  // 最多四帧等待处理。
    std::deque<std::vector<int16_t>> call_decode_ready_;            // 最多四帧等待PCM端口。
    std::uint64_t call_decode_generation_ = 0;                      // 重连及关闭均作废在途帧。
    bool call_decode_shutdown_ = false, call_decode_busy_ = false;  // 生命周期和实际在途事实。
    TaskHandle_t call_decode_task_ = nullptr;          // 当前通话任务，仅媒体事件线程创建和销毁。
    SemaphoreHandle_t call_decode_stopped_ = nullptr;  // 算法退出及队列访问完成后的屏障。
    unsigned playback_rejected_ = 0;                   // PCM写入拒绝独立于网络和算法失败统计。
#endif
    std::uint64_t audio_trace_at_ = 0;  // 本腿诊断窗口起点，仅媒体事件线程访问。
    std::uint64_t encode_time_us_ = 0, decode_time_us_ = 0;  // 算法实际累计墙钟耗时。
    unsigned encode_attempts_ = 0, decode_attempts_ = 0;  // 当前窗口实际处理帧数。
    unsigned encode_failures_ = 0, decode_failures_ = 0;  // 算法失败和网络失败独立记录。
#if CONFIG_UR_AI_SIGNALING_WEBRTC_VIDEO
    /* video_send_at_ 上次视频帧发送时刻（微秒）：帧级限速保护 keepalive。 */
    int64_t video_send_at_ = 0;
#endif
};

/* ---- Impl 方法定义（PeerMedia 已完整）---- */

UrSignalingCall::Impl::Impl() = default;

UrSignalingCall::Impl::~Impl() {
    TeardownLeg();
    if (ui_queue_ != nullptr) {
        vQueueDeleteWithCaps(ui_queue_);
        ui_queue_ = nullptr;
    }
    if (hooks_mutex_ != nullptr) {
        vSemaphoreDelete(hooks_mutex_);
    }
}

void UrSignalingCall::Impl::Attach(ur::device::UrSignalingMqttPort* port,
                                   const std::string& identity) {
    if (port == nullptr || identity.empty()) {
        return;
    }
    /* 懒构造边界：这里只保存轻量指针/字符串，通话腿等重组件在首条邀请时建。 */
    if (!pending_tombstones_identity_.empty() && pending_tombstones_identity_ != identity)
        pending_tombstones_ = PendingCallTombstones{};
    pending_tombstones_identity_ = identity;
    port_ = port;
    identity_ = identity;
    dial_ready_.store(EnsureUiQueue());  // 冷启动即允许主动呼叫，队列仍不创建媒体或客户端。
}

// Detach 只在宿主信令线程退出时执行；向平台尽力结束原腿后同步停止本地媒体。
void UrSignalingCall::Impl::Detach() {
    dial_ready_.store(false);
    ++ui_generation_;  // 让解绑前已取代次、随后才入队的拨号也在新宿主上失效。
    pending_dial_.reset();
    dial_queued_.store(false);
#if CONFIG_UR_SPARKBOT_VOICE_MODEL_TEST
    StopVoiceAnswerTest(true);
#endif
    ++ui_generation_;  // 即使没有活动腿，也要作废宿主卸载前并发投递的命令。
    if (client_)
        client_->end(MonotonicMs());
    StopMedia();
    TeardownLeg();
    media_.reset();  // 部分装配未创建client时也必须等待编解码任务实际退出。
    if (pending_invite_) {
        ur::device::Envelope invite;
        if (port_ && ur::device::decode_envelope(pending_invite_->body, invite) ==
                         ur::device::DecodeResult::Ok)
            RejectInvite(invite, "busy");
        RetirePending(PendingCallTombstones::Decision::Busy);
    }
    pending_accept_ = false;
    if (ui_queue_) {
        QueuedUiCommand stale{};  // 不让旧宿主尚未消费的接听或挂断污染下一次装配。
        while (xQueueReceive(ui_queue_, &stale, 0) == pdTRUE) {}
    }
    port_ = nullptr;
    pcm_ = nullptr;
    codec_ = nullptr;
    host_busy_ = {};
    host_yield_ = {};
#if CONFIG_UR_AI_SIGNALING_WEBRTC_VIDEO
    video_ = nullptr;
    video_starter_ = {};
#endif
    identity_.clear();
    instance_.clear();
    FireState(UiState::Hidden);
    ui_state_ = UiState::Hidden;
    hidden_pending_ = false;
}

void UrSignalingCall::Impl::SetAudioBridge(ur::device::UrSignalingPcm* pcm,
                                           ur::device::UrSignalingOpus* codec) {
    pcm_ = pcm;
    codec_ = codec;
}

void UrSignalingCall::Impl::SetHostBusy(std::function<bool()> host_busy) {
    host_busy_ = std::move(host_busy);
}

void UrSignalingCall::Impl::SetHostYield(std::function<bool()> yield) {
    host_yield_ = std::move(yield);
}

#if CONFIG_UR_AI_SIGNALING_WEBRTC_VIDEO
void UrSignalingCall::Impl::SetVideoPipeline(ur::device::UrSignalingVideo* video,
                                             std::function<bool()> starter) {
    video_ = video;
    video_starter_ = std::move(starter);
}
#endif

void UrSignalingCall::Impl::SetHooks(UiHooks hooks) {
    /* 互斥锁懒创建：注册回调（板级 UI 初始化）通常先于任何通话发生。 */
    if (hooks_mutex_ == nullptr) {
        hooks_mutex_ = xSemaphoreCreateMutex();
        if (hooks_mutex_ == nullptr) {
            return;
        }
    }
    xSemaphoreTake(hooks_mutex_, portMAX_DELAY);
    hooks_ = std::move(hooks);
    xSemaphoreGive(hooks_mutex_);
}

bool UrSignalingCall::Impl::Handle(std::string_view topic, std::string_view body, std::uint64_t now,
                                   std::uint64_t wall) {
    if (port_ == nullptr || pcm_ == nullptr || codec_ == nullptr) {
        return false;
    }
    const std::string& leg = client_ ? client_->session_id() : kEmptySession;
    if (client_ == nullptr && body.find("humanCall") == std::string_view::npos &&
        !(pending_invite_ && body.find("sessionEnded") != std::string_view::npos)) {
        /* 无在途腿且正文不含 humanCall 用途：直接放行，避免对 AI 高频信令反复解析。 */
        return false;
    }
    ur::device::Envelope envelope;
    if (ur::device::decode_envelope(body, envelope) != ur::device::DecodeResult::Ok) {
        return false;
    }
    /* 分派规则：humanCall 邀请，或报文会话等于当前人际腿会话（ACK/事件均带会话）。 */
    const bool invite = envelope.method == "sessionInvite" && envelope.request &&
                        ur::device::human_call_invitation(body);
    const bool leg_match = !leg.empty() && envelope.session == leg;
    const bool create_match = client_ && client_->owns_shared_create_reply(envelope);
    pending_tombstones_.Advance(wall);
    if (!invite && pending_invite_ && client_ == nullptr &&
        topic == "$thing/down/signaling/" + identity_ &&
        ur::device::human_call_terminal_matches(envelope, pending_invite_->session, 1)) {
        // 仅接受本腿的合法结束事件，先保留终态再丢弃排队接听，避免 QoS 重投复活。
        RetirePending(PendingCallTombstones::Decision::Cancelled);
        pending_accept_ = false;
        return true;
    }
    if (invite && topic != "$thing/down/signaling/" + identity_) return false;
    if (invite) {
        const auto expires = ur::device::human_call_pending_deadline(envelope);
#if CONFIG_UR_AI_E2E_TEST
        // 仅测试包记录原始墙钟和截止，以区分时钟偏差与无效邀请；不记录会话身份。
        ESP_LOGI(kTag, "invite clock wall=%llu deadline=%llu valid=%d expired=%d",
                 static_cast<unsigned long long>(wall),
                 static_cast<unsigned long long>(expires.value_or(0)), expires.has_value(),
                 expires && pending_tombstones_.Expired(*expires));
#endif
        if (!expires || pending_tombstones_.Expired(*expires)) return true;
        const auto decision = pending_tombstones_.Find(envelope.token, envelope.session, envelope.payload);
        if (decision != PendingCallTombstones::Decision::None) {
            if (decision == PendingCallTombstones::Decision::Busy)
                RejectInvite(envelope, "busy");
            else if (decision == PendingCallTombstones::Decision::Declined)
                RejectInvite(envelope, "declined");
            return true;  // 已取消或身份冲突不得重新呈现。
        }
    }
    if (!invite && !leg_match && !create_match) {
        return false;
    }
    if (invite && client_ == nullptr && dial_queued_.load()) {
        RejectInvite(envelope, "busy");
        const auto expires = ur::device::human_call_pending_deadline(envelope);
        if (expires && !pending_tombstones_.Remember(std::move(envelope.token), std::move(envelope.session),
                std::move(envelope.payload), *expires, PendingCallTombstones::Decision::Busy))
            pending_tombstones_.Fence(*expires);
        return true;
    }
    if (client_ != nullptr) {
        const auto state = client_->state();
        if (state == ur::device::HumanCallState::Failed ||
            state == ur::device::HumanCallState::Idle ||
            state == ur::device::HumanCallState::Available) {
            /* 上一腿终态残留（含正常收尾后的 Available）：先销毁再按新邀请重建，
             * 防止残留腿对新邀请直接回 busy、看门狗计时跨腿误触发。 */
            TeardownLeg();
        }
    }
    if (invite && client_ == nullptr && pending_invite_) {
        // 让位中的邀请仍占用设备：重投保持幂等，其他来电不能覆盖首个来电。
        if (pending_invite_->token == envelope.token ||
            pending_invite_->session == envelope.session) {
            // 同一 token 或会话的重投必须保留完整原身份，冲突不能拒绝原腿。
            ur::device::Envelope original;
            if (pending_invite_->token != envelope.token ||
                pending_invite_->session != envelope.session ||
                ur::device::decode_envelope(pending_invite_->body, original) !=
                    ur::device::DecodeResult::Ok ||
                original.payload != envelope.payload ||
                original.event_sequence != envelope.event_sequence ||
                original.code != envelope.code) {
                ESP_LOGW(kTag, "conflicting pending invite ignored");
            }
        } else {
            const auto expires = ur::device::human_call_pending_deadline(envelope);
            if (expires && !pending_tombstones_.Expired(*expires)) {
                RejectInvite(envelope, "busy");
                // 首个等待邀请必须留有终态槽；溢出时用截止屏障阻止拒接复活。
                const auto bytes = envelope.token.size() + envelope.session.size() +
                                   envelope.payload.size();
                if (!pending_tombstones_.CanRecordOther(bytes, pending_invite_->retained_bytes) ||
                    !pending_tombstones_.Remember(std::move(envelope.token),
                                                  std::move(envelope.session),
                                                  std::move(envelope.payload), *expires,
                                                  PendingCallTombstones::Decision::Busy))
                    pending_tombstones_.Fence(*expires);
            }
        }
        return true;
    }
    const auto invite_bytes = envelope.token.size() + envelope.session.size() +
                              envelope.payload.size();
    if (invite && client_ == nullptr && !pending_tombstones_.CanReserve(invite_bytes)) {
        const auto expires = ur::device::human_call_pending_deadline(envelope);
        if (expires && !pending_tombstones_.Expired(*expires)) {
            RejectInvite(envelope, "busy");
            pending_tombstones_.Fence(*expires);
        }
        return true;
    }
    if (invite && client_ == nullptr && HostBusy()) {
        /* 让位（interrupt）语义：AI 会话占用时不立即回 busy，先请求宿主结束
         * AI 会话并保留邀请快照；Poll 里周期检查让位进度，超时才回 busy。 */
        const auto expires = ur::device::human_call_pending_deadline(envelope);
        if (!expires || (wall != 0 && *expires <= wall)) {
            ESP_LOGW(kTag, "invalid or expired invite during host yield");
            return true;
        }
        StartYieldWait(envelope, body, now, *expires);
        return true;
    }
    if (invite && client_ == nullptr) {
        /* 宿主空闲：直接建腿交付邀请。 */
        DeliverInvite(body, now, wall);
        return true;
    }
    if (client_ == nullptr && !EnsureLeg(now)) {
        /* 内存不足无法建腿：明确记日志并立即回 busy，让平台释放邀请。 */
        ESP_LOGE(kTag, "human leg construct failed (heap low) free=%u internal=%u",
                 static_cast<unsigned>(esp_get_free_heap_size()),
                 static_cast<unsigned>(esp_get_free_internal_heap_size()));
        if (invite) {
            RejectInvite(envelope, "busy");
        }
        return true;
    }
    /* 腿报文入口日志（每腿仅数条）：method + 处理前状态，配合 human state 变化
     * 日志可完整还原腿对下行（含 sessionEnded）的消费轨迹。 */
    ESP_LOGI(kTag, "human leg rx %s (state=%d)", envelope.method.c_str(),
             static_cast<int>(client_->state()));
    if (!client_->receive_shared(topic, body, now, wall)) {
        /* 借用腿对畸形/时钟回退会自我失败；仅记首因，销毁在 Poll 收尾。 */
        const auto facts = client_->failure_facts();
        ESP_LOGW(kTag, "human leg receive rejected failure=%d", static_cast<int>(facts.failure));
    }
    return true;
}
void UrSignalingCall::Impl::Poll(std::uint64_t now, std::uint64_t wall) {
    now_ms_ = now; /* 快照供 StartMedia 等无参方法登记错峰时刻。 */
    pending_tombstones_.Advance(wall);
    DrainUiCommands(now);
    AdvanceDial(now);
#if CONFIG_UR_SPARKBOT_VOICE_MODEL_TEST
    AdvanceVoiceAnswerGuard();
    AdvanceVoiceAnswerAccept(now);
#endif
    if (client_ == nullptr) {
        AdvanceYieldWait(now, wall);
        return;
    }
    /* 状态变化即打印：与 AI voice state 日志同口径，定位腿滞留的准确状态。 */
    const auto current_state = client_->state();
    if (current_state != last_client_state_) {
        last_client_state_ = current_state;
        ESP_LOGI(kTag, "human state -> %d", static_cast<int>(current_state));
    }
    client_->poll(now, wall);
#if CONFIG_UR_AI_SIGNALING_WEBRTC_VIDEO
    /* 下行解码器延后启动：给相机上行首批 SPI DMA 分配留出窗口。 */
    if (renderer_start_at_ != 0 && now >= renderer_start_at_) {
        renderer_start_at_ = 0;
        StartRenderer();
    }
    /* 相机上行错峰启动：到达登记时刻且媒体仍连通时拉起（一次）。 */
    if (video_uplink_pending_at_ != 0 && now >= video_uplink_pending_at_) {
        video_uplink_pending_at_ = 0;
        if (media_active_ && media_ != nullptr && media_->connected() && !camera_paused_) {
            StartVideoUplink();
        }
    }
#endif
    if (media_ != nullptr) {
        // RTC 沿用已验证的低优先级，避免媒体处理挤占空闲任务；UDP 还要排空
        // 视频分片，沿用其独立调度上限。退出后恢复 AI 宿主调度。
        const auto original_priority = uxTaskPriorityGet(nullptr);
        const auto media_priority = media_->IsUDP() ? 5U : 2U;
        const bool reduce_priority = media_active_ && original_priority > media_priority;
        if (reduce_priority)
            vTaskPrioritySet(nullptr, media_priority);
        media_->Poll();
        if (reduce_priority)
            vTaskPrioritySet(nullptr, original_priority);
    }
#if CONFIG_UR_AI_SIGNALING_WEBRTC_VIDEO
    if (media_active_ && remote_video_selected_ && remote_video_disabled_.load() &&
        !remote_video_pause_sent_ && client_->control_track("video-in", true, now))
        remote_video_pause_sent_ = true;
#endif
#if CONFIG_UR_AI_SIGNALING_WEBRTC_VIDEO
    // UDP AV 的 JPEG 解码器自带独立慢帧门；音频采集窗口只约束共享CPU的WebRTC渲染器，
    // 避免UDP音频启动爬坡把本地屏幕误停显。
    if (media_active_ && media_ && !media_->IsUDP()) {
        const auto audio_frames = media_->AudioUpFrames();
        if (audio_capture_gate_.overloaded(esp_timer_get_time() / 1000, audio_frames,
                                           mic_paused_) &&
            !remote_video_disabled_.exchange(true))
            ESP_LOGW(kTag, "remote video disabled audio gate frames=%u", audio_frames);
    }
#if CONFIG_UR_AI_SIGNALING_H264_UPLINK
    if (media_active_ && remote_video_disabled_.load() && media_ && !media_->IsUDP() && video_)
        video_->PrioritizeAudio();
#endif
    if (media_active_ && renderer_started_) {
        const auto media_now = static_cast<std::uint64_t>(esp_timer_get_time() / 1000);
        if ((!mic_paused_ && media_now > media_started_at_ + 3000 &&
             media_now > audio_capture_at_ + 120) ||
            heap_caps_get_largest_free_block(MALLOC_CAP_DMA) < 8192)
            remote_video_disabled_.store(true);  // 音频先行，收画面不能阻塞麦克风采集。
        auto last_frame = remote_frame_at_.load();
        if (remote_video_disabled_.load()) {
            StopRenderer();  // 只在信令线程停解码任务，禁止在帧回调里等待自己退出。
            remote_frame_at_.store(0);
            FireRemoteFrame(nullptr, 0, 0);
            ESP_LOGW(kTag, "remote video hidden for audio/decoding budget");
        } else if (last_frame && media_now >= last_frame + 500 &&
                   remote_frame_at_.compare_exchange_strong(last_frame, 0)) {
            FireRemoteFrame(nullptr, 0, 0);
        }
    }
#endif
    DriveNegotiation(now);
    WatchIceDrop(now);
    WatchEstablish(now);
    ProjectState();
    RecoverIfFailed(now);
}

bool UrSignalingCall::Impl::Busy() const {
    if (pending_dial_) return true;
    if (client_ == nullptr) {
        return false;
    }
    const auto state = client_->state();
    return state != ur::device::HumanCallState::Idle &&
           state != ur::device::HumanCallState::Available &&
           state != ur::device::HumanCallState::Failed;
}

bool UrSignalingCall::Impl::EnsureUiQueue() {
    if (ui_queue_ == nullptr) {
        ui_queue_ = xQueueCreateWithCaps(kUiQueueLength, sizeof(QueuedUiCommand),
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    return ui_queue_ != nullptr;
}

void UrSignalingCall::Impl::PostUi(int command) {
    PostUiForGeneration(command, ui_generation_.load());
}

void UrSignalingCall::Impl::PostUiForGeneration(int command, std::uint32_t generation) {
    if (ui_queue_ != nullptr) {
        const QueuedUiCommand queued{command, generation};
        xQueueSend(ui_queue_, &queued, 0);
    }
}

bool UrSignalingCall::Impl::EnsureLeg(std::uint64_t now, bool outgoing_voice) {
    /* 首次直接建腿和 AI 让位路径共用队列，避免首次让位时丢失按键。 */
    if (!EnsureUiQueue()) {
        return false;
    }
    // 每条新腿使用独立请求前缀，避免客户端序号归零后命中上一通创建请求的幂等墓碑。
    instance_ = RandomCallInstance();
    /* nothrow 构造：内存不足返回 false 而不是 abort，调用方回 busy。 */
    media_.reset(new (std::nothrow) PeerMedia(*this));
    if (media_ == nullptr) {
        return false;
    }
    ur::device::HumanCallConfig config;
    config.camera_id = "front";
    config.borrowed_connection = true;
    config.mode = outgoing_voice ? ur::device::HumanCallMode::Voice
                                 : ur::device::HumanCallMode::Video;
    config.voice_and_video = true;
    // 纯语音不预约接收画面；视频的实际接收仍须本次邀请轨授权。
#if CONFIG_UR_HUMAN_CALL_UDP_AV
    config.receive_video = !outgoing_voice && media_->video_receive_supported();
#else
    config.receive_video = !outgoing_voice && media_->video_receive_supported();
#endif
#if CONFIG_UR_HUMAN_CALL_UDP_AV
    config.udp_av = true;
    config.udp_only = true;
#endif
    client_.reset(new (std::nothrow)
                      ur::device::HumanCallClient(*port_, *media_, identity_, instance_, config));
    if (client_ == nullptr) {
        media_.reset();
        return false;
    }
    if (!client_->start_shared(now)) {
        client_.reset();
        media_.reset();
        return false;
    }
    /* 堆水位日志①：腿构造完成，量化通话栈实际占用（仅每次建腿打一条）。 */
    ESP_LOGI(kTag, "human leg constructed free_heap=%u internal_free=%u",
             static_cast<unsigned>(esp_get_free_heap_size()),
             static_cast<unsigned>(esp_get_free_internal_heap_size()));
    return true;
}

void UrSignalingCall::Impl::TeardownLeg() {
    if (client_ == nullptr) {
        return;
    }
#if CONFIG_UR_SPARKBOT_VOICE_MODEL_TEST
    StopVoiceAnswerTest(true);
#endif
    ++ui_generation_;  // 销毁通话腿时让仍在投递或排队的旧操作失效。
    const auto facts = client_->failure_facts();
    if (facts.failure != ur::device::HumanCallFailure::None) {
        ESP_LOGW(kTag, "human leg ended failure=%d capabilityCode=%d",
                 static_cast<int>(facts.failure), facts.capability_ack_code);
    }
    client_->close();
    client_.reset();
    media_.reset();
    dial_queued_.store(false);
    offer_sent_ = connected_reported_ = media_active_ = false;
    mic_paused_ = camera_paused_ = false;
    ice_down_since_ = 0;
    leg_started_ms_ = 0;
    last_client_state_ = ur::device::HumanCallState::Idle;
    /* 异常并存时也须保留等待邀请的终态，不能让旧邀请因销毁腿而复活。 */
    RetirePending(PendingCallTombstones::Decision::Busy);
    pending_accept_ = false;
    if (ui_queue_ != nullptr) {
        /* 清掉残留按键（如结束页误触挂断），避免污染下一条腿。 */
        QueuedUiCommand stale{};
        while (xQueueReceive(ui_queue_, &stale, 0) == pdTRUE) {
        }
    }
    /* 堆水位日志②：腿销毁释放后，验证空闲内部 SRAM 已回到通话前水平。 */
    ESP_LOGI(kTag, "human leg released free_heap=%u internal_free=%u",
             static_cast<unsigned>(esp_get_free_heap_size()),
             static_cast<unsigned>(esp_get_free_internal_heap_size()));
}

void UrSignalingCall::Impl::RejectInvite(const ur::device::Envelope& invite, const char* reason) {
    ur::device::Envelope reject;
    reject.token = invite.token;
    reject.method = "sessionReject";
    reject.session = invite.session;
    reject.payload = std::string(R"({"reason":"")") + reason + "\"}";
    std::string body;
    if (ur::device::encode_envelope(reject, body) != ur::device::DecodeResult::Ok) {
        return;
    }
    port_->publish("$thing/up/signaling/" + identity_, body);
    ESP_LOGW(kTag, "human invite rejected reason=%s", reason);
}

void UrSignalingCall::Impl::RetirePending(PendingCallTombstones::Decision decision) {
    if (!pending_invite_) return;
    ++ui_generation_;  // 取消、拒绝或超时后不得把旧操作应用于下一通来电。
    const auto expires = pending_invite_->expires;
    ur::device::Envelope original;
    if (ur::device::decode_envelope(pending_invite_->body, original) !=
            ur::device::DecodeResult::Ok ||
        !pending_tombstones_.Remember(std::move(pending_invite_->token),
                                      std::move(pending_invite_->session),
                                      std::move(original.payload), expires, decision)) {
        // 意外容量或解析失败时不能让旧邀请变成新许可。
        ESP_LOGW(kTag, "pending invitation terminal fenced");
        pending_tombstones_.Fence(expires);
    }
    pending_invite_.reset();
}

void UrSignalingCall::Impl::StartYieldWait(const ur::device::Envelope& invite,
                                           std::string_view body, std::uint64_t now,
                                           std::uint64_t expires) {
    PendingInvite snapshot;
    snapshot.token = invite.token;
    snapshot.session = invite.session;
    snapshot.body.assign(body);
    snapshot.expires = expires;
    snapshot.retained_bytes = invite.token.size() + invite.session.size() + invite.payload.size();
    snapshot.started = now;
    snapshot.last_check = now;
    pending_invite_ = std::move(snapshot);
    pending_accept_ = false;
    if (!EnsureUiQueue()) {
        // 内存不足时立即拒绝并保留终态，避免出现无法接听或拒绝的来电。
        RejectInvite(invite, "busy");
        RetirePending(PendingCallTombstones::Decision::Busy);
        return;
    }
    /* 首次让位请求：宿主在信令任务内同步结束 AI 会话（非阻塞）。 */
    if (RequestHostYield()) {
        ESP_LOGI(kTag, "human invite waiting for host yield (token=%.8s..)",
                 pending_invite_->token.c_str());
    } else {
        /* 宿主不受理让位：立即回 busy，不让平台空等。 */
        ESP_LOGW(kTag, "host yield not accepted; rejecting invite");
        RejectInvite(invite, "busy");
        RetirePending(PendingCallTombstones::Decision::Busy);
    }
}

void UrSignalingCall::Impl::AdvanceYieldWait(std::uint64_t now, std::uint64_t wall) {
    if (!pending_invite_.has_value()) return;
    if (client_ != nullptr) {
        // 异常并存时保持防重投事实，不能静默丢弃等待快照。
        RetirePending(PendingCallTombstones::Decision::Busy);
        pending_accept_ = false;
        return;
    }
    if (now - pending_invite_->started >= kYieldTimeoutMs) {
        ESP_LOGI(kTag, "host yield timeout after %llu ms; rejecting invite",
                 static_cast<unsigned long long>(kYieldTimeoutMs));
        ur::device::Envelope invite;
        if (ur::device::decode_envelope(pending_invite_->body, invite) ==
            ur::device::DecodeResult::Ok) {
            RejectInvite(invite, "busy");
        }
        RetirePending(PendingCallTombstones::Decision::Busy);
        pending_accept_ = false;
        return;
    }
    if (now - pending_invite_->last_check < kYieldCheckMs) {
        return;
    }
    pending_invite_->last_check = now;
    if (HostBusy()) {
        /* AI 会话仍在（应用层可能在让位后重开）：再次请求让位，直到超时。 */
        RequestHostYield();
        return;
    }
    /* 让位完成：建腿并把保留的原始邀请原样交给状态机，进入 Ringing。 */
    ESP_LOGI(kTag, "host yielded; delivering pending invite");
    const std::string body = std::move(pending_invite_->body);
    pending_invite_.reset();
    DeliverInvite(body, now, wall);
}

bool UrSignalingCall::Impl::DeliverInvite(std::string_view body, std::uint64_t now,
                                          std::uint64_t wall) {
    /* 新邀请交付即取代任何等待快照（保留 pending_accept_ 供补执行）。 */
    pending_invite_.reset();
    if (!EnsureLeg(now)) {
        ESP_LOGE(kTag, "human leg construct failed free=%u internal=%u",
                 static_cast<unsigned>(esp_get_free_heap_size()),
                 static_cast<unsigned>(esp_get_free_internal_heap_size()));
        ur::device::Envelope invite;
        if (ur::device::decode_envelope(body, invite) == ur::device::DecodeResult::Ok) {
            RejectInvite(invite, "busy");
        }
        pending_accept_ = false;
        return false;
    }
    const std::string topic = "$thing/down/signaling/" + identity_;
    if (!client_->receive_shared(topic, body, now, wall)) {
        const auto facts = client_->failure_facts();
        ESP_LOGW(kTag, "invite deliver rejected failure=%d", static_cast<int>(facts.failure));
    }
    leg_started_ms_ = now;
    if (pending_accept_ && client_->state() == ur::device::HumanCallState::Ringing) {
        /* 让位等待期用户已按接听：建腿后立即补执行。 */
        pending_accept_ = false;
#if CONFIG_UR_SPARKBOT_VOICE_MODEL_TEST
        // 上一通试验模型可能仍在释放，补执行也必须经过同一接听屏障。
        ApplyUiCommand(kUiAccept, now);
#else
        client_->accept(now);
#endif
        ESP_LOGI(kTag, "pending accept applied after delivery");
    }
    return true;
}

// PostDial 校验队列载荷边界；真正身份、联系人权限和忙线仍由平台独立核验。
bool UrSignalingCall::Impl::PostDial(const std::string& target, bool voice_only) {
    const auto generation = ui_generation_.load();  // 在就绪检查前固定宿主代次，解绑重绑不能接管旧意图。
    const auto separator = target.find(':');  // 产品与设备名必须都非空，不能把名称当成任意Topic。
    if (!dial_ready_.load() || !ui_queue_ || target.size() > 128 || separator == std::string::npos ||
        separator == 0 || separator + 1 == target.size() ||
        target.find_first_of("/+#") != std::string::npos ||
        std::any_of(target.begin(), target.end(), [](unsigned char ch) { return ch < 32 || ch == 127; }))
        return false;
    bool expected = false;  // 原子预留仅保护排队意图，任意任务都不读取客户端裸指针。
    if (!dial_queued_.compare_exchange_strong(expected, true)) return false;
    if (!dial_ready_.load() || generation != ui_generation_.load()) {
        dial_queued_.store(false);
        return false;
    }
    QueuedUiCommand command{kUiDial, generation};  // 队列按值复制目标，调用方无需保活。
    std::memcpy(command.target, target.c_str(), target.size() + 1);
    command.voice_only = voice_only;
    if (xQueueSend(ui_queue_, &command, 0) != pdTRUE) {
        dial_queued_.store(false);
        return false;
    }
    return true;
}

// AdvanceDial 等待AI会话实际释放后拨号；失败和超时清理原意图，避免迟到拨打另一个联系人。
void UrSignalingCall::Impl::AdvanceDial(std::uint64_t now) {
    if (!pending_dial_) return;
    if (HostBusy() && now - dial_requested_at_ < kYieldTimeoutMs) return;
    const auto command = *pending_dial_;  // 删除等待快照后才可将媒体所有权交给正式客户端。
    pending_dial_.reset();
    if (client_) TeardownLeg();  // 上一腿Available也要归还HAL，不能直接覆盖仍持有资源的客户端。
    if (HostBusy() || !EnsureLeg(now, command.voice_only) || !client_->dial(command.target, now)) {
        TeardownLeg();
        dial_queued_.store(false);
        ui_state_ = UiState::Ended;
        FireState(ui_state_);
        ESP_LOGW(kTag, "outgoing human call unavailable");
        return;
    }
    leg_started_ms_ = now;
    dial_queued_.store(true);
    ESP_LOGI(kTag, "outgoing human call requested voice=%d", command.voice_only);
}

void UrSignalingCall::Impl::DrainUiCommands(std::uint64_t now) {
    if (ui_queue_ == nullptr) {
        return;
    }
    QueuedUiCommand queued{};
    while (xQueueReceive(ui_queue_, &queued, 0) == pdTRUE) {
        if (queued.generation == ui_generation_.load()) {
            if (queued.command == kUiDial) {
                if (Busy() || pending_invite_) {
                    dial_queued_.store(false);
                    continue;
                }
                pending_dial_ = queued;
                dial_requested_at_ = now;
                voice_call_ = queued.voice_only;
                ui_state_ = UiState::Connecting;
                FireState(ui_state_);
                if (HostBusy() && !RequestHostYield()) {
                    pending_dial_.reset();
                    dial_queued_.store(false);
                    ui_state_ = UiState::Ended;
                    FireState(ui_state_);
                }
            } else {
                ApplyUiCommand(queued.command, now);
            }
        } else if (queued.command == kUiDial) {
            dial_queued_.store(false);
        }
    }
}

void UrSignalingCall::Impl::ApplyUiCommand(int command, std::uint64_t now) {
    if (client_ == nullptr) {
        if (pending_dial_ && command == kUiHangUp) {
            pending_dial_.reset();
            dial_queued_.store(false);
            ++ui_generation_;
            ui_state_ = UiState::Ended;
            FireState(ui_state_);
        }
        /* 让位等待期（腿未建）：接听排队建腿后自动执行；拒接直接了结邀请。 */
        if (pending_invite_.has_value()) {
            if (command == kUiAccept) {
                pending_accept_ = true;
            } else if (command == kUiReject) {
                ur::device::Envelope invite;
                if (ur::device::decode_envelope(pending_invite_->body, invite) ==
                    ur::device::DecodeResult::Ok) {
                    RejectInvite(invite, "declined");
                }
                RetirePending(PendingCallTombstones::Decision::Declined);
                pending_accept_ = false;
            }
        }
        return;
    }
    const auto state = client_->state();
    switch (command) {
        case kUiAccept:
#if CONFIG_UR_SPARKBOT_VOICE_MODEL_TEST
            if (state == ur::device::HumanCallState::Ringing && !voice_accept_pending_) {
                voice_accept_pending_ = true;
                voice_accept_generation_ = ui_generation_.load();
                StopVoiceAnswerTest(false);
            }
#else
            if (state == ur::device::HumanCallState::Ringing && !client_->accept(now)) {
                ESP_LOGW(kTag, "ui accept rejected");
            }
#endif
            break;
        case kUiReject:
#if CONFIG_UR_SPARKBOT_VOICE_MODEL_TEST
            voice_accept_pending_ = false;
            StopVoiceAnswerTest(false);
#endif
            if (state == ur::device::HumanCallState::Ringing && !client_->reject(now)) {
                ESP_LOGW(kTag, "ui reject rejected");
            }
            break;
        case kUiHangUp:
#if CONFIG_UR_SPARKBOT_VOICE_MODEL_TEST
            voice_accept_pending_ = false;
            StopVoiceAnswerTest(false);
#endif
            if (Busy() && state != ur::device::HumanCallState::Ending) {
                ESP_LOGI(kTag, "ui hangup accepted state=%d", static_cast<int>(state));
                /* Ringing 下 end() 内部转拒接；其余活动腿本地立即停流。 */
                if (!client_->end(now)) {
                    ESP_LOGW(kTag, "ui hangup rejected");
                }
            } else {
                /* 无活动腿（让位等待/已收尾）或正在结束：明确记 no-op 便于排查。 */
                ESP_LOGI(kTag, "ui hangup no-op state=%d", static_cast<int>(state));
            }
            break;
        case kUiToggleMic:
            if (state == ur::device::HumanCallState::Connected &&
                !client_->track_control_pending()) {
                if (mic_paused_ && (!media_ || !media_->RestartCapture())) {
                    ESP_LOGW(kTag, "mic resume capture restart failed; keeping muted");
                    break;
                }
                mic_paused_ = !mic_paused_;
                client_->control_track(voice_call_ ? "voice" : "mic-1", mic_paused_, now);
                NotifyTrackState();
            }
            break;
        case kUiToggleCamera:
            if (!voice_call_ && state == ur::device::HumanCallState::Connected &&
                !client_->track_control_pending()) {
                const bool pause = !camera_paused_;  // 当前按钮请求的本地相机状态。
#if CONFIG_UR_AI_SIGNALING_WEBRTC_VIDEO
                const bool udp = media_ && media_->IsUDP();  // 仅操作当前已选媒体的相机驱动。
                if (udp) {
                    auto* camera = Board::GetInstance().GetCamera();
                    if (pause) {
                        media_->StopUdpVideo();
                        if (camera && udp_camera_owned_) {
                            // 释放相机前恢复默认节拍，避免影响后续使用者。
                            camera->SetStreamFrameInterval(0);
                            camera->StopStream();
                        }
                        udp_camera_owned_ = false;
                    } else if (!camera || !camera->StartCompressedStream()) {
                        // 实际恢复失败保持暂停，不向平台宣告已经恢复视频。
                        ESP_LOGW(kTag, "UDP camera resume failed; remaining paused");
                        break;
                    } else if (!media_->StartUdpVideo()) {
                        camera->StopStream();
                        ESP_LOGW(kTag, "UDP video worker failed; remaining paused");
                        break;
                    } else {
                        // 仅在通话接管相机及视频任务均成功后设置采样节拍。
                        camera->SetStreamFrameInterval(40);
                        udp_camera_owned_ = true;
                    }
                }
#endif
                camera_paused_ = pause;
                client_->control_track("video-1", camera_paused_, now);
#if CONFIG_UR_AI_SIGNALING_WEBRTC_VIDEO
                if (!udp) {
                    if (camera_paused_) {
                        StopVideoUplink();
                    } else {
                        StartVideoUplink();
                    }
                }
#endif
                NotifyTrackState();
            }
            break;
        default:
            break;
    }
}

void UrSignalingCall::Impl::DriveNegotiation(std::uint64_t now) {
    if (client_ == nullptr || media_ == nullptr) {
        return;
    }
    const auto state = client_->state();
    if (state == ur::device::HumanCallState::Negotiating && !offer_sent_) {
        const auto offer = media_->TakeOffer();
        if (offer.has_value()) {
            offer_sent_ = true;
#if CONFIG_UR_AI_SIGNALING_WEBRTC_VIDEO
            /* 视频通话才要求视频m-line；纯语音SDP和UDP固定格式由人际核心按邀请类型校验。 */
            if (!voice_call_ && !media_->IsUDP() &&
                offer->find("m=video ") == std::string::npos) {
                ESP_LOGE(kTag, "call offer lacks video m-line; ending leg");
                client_->end(now);
                return;
            }
#endif
            if (!client_->offer_ready(*offer, now)) {
                ESP_LOGW(kTag, "offer_ready rejected");
            }
        }
    }
    if (state == ur::device::HumanCallState::ConnectingMedia && media_->connected() &&
        !connected_reported_) {
        connected_reported_ = true;
        StartMedia();
        if (!client_->media_connected(now)) {
            ESP_LOGW(kTag, "media_connected rejected");
        }
        return;
    }
    if (connected_reported_ && media_->connected() && !media_active_) {
        /* ICE 短暂断开后再次连通：恢复本地采集与渲染（看门狗未触发挂断时）。 */
        StartMedia();
    }
}

void UrSignalingCall::Impl::WatchIceDrop(std::uint64_t now) {
    if (client_ == nullptr || media_ == nullptr) {
        return;
    }
    if (client_->state() == ur::device::HumanCallState::Connected) {
        if (media_->connected()) {
            ice_down_since_ = 0;
        } else {
            if (ice_down_since_ == 0) {
                ice_down_since_ = now;
                StopMedia();
            } else if (now - ice_down_since_ >= kIceDropHangupMs) {
                ESP_LOGW(kTag, "ice dropped over %llu ms; hanging up",
                         static_cast<unsigned long long>(kIceDropHangupMs));
                ice_down_since_ = 0;
                client_->end(now);
            }
        }
    } else {
        ice_down_since_ = 0;
    }
}

void UrSignalingCall::Impl::WatchEstablish(std::uint64_t now) {
    if (!Busy() || media_active_ || connected_reported_) {
        // 曾连通过的腿由正常结束确认、核心阶段期限及断线检测收尾，不能重判建立超时。
        return;
    }
    if (now - leg_started_ms_ < kEstablishWatchdogMs) {
        return;
    }
    /* 兜底强制收尾：先尽力通知平台（Ringing 转拒接、活动腿发 sessionEnd），
     * 再无条件销毁本地腿，恢复对下一通来电的接听能力。 */
    ESP_LOGW(kTag, "human leg watchdog: no media in %llu ms state=%d; force teardown",
             static_cast<unsigned long long>(kEstablishWatchdogMs),
             static_cast<int>(client_->state()));
    client_->end(now);
    // 强制销毁后不再有状态可供ProjectState投影，须先撤掉通话控制层。
    FireState(UiState::Hidden);
    ui_state_ = UiState::Hidden;
    hidden_pending_ = false;
    TeardownLeg();
}

void UrSignalingCall::Impl::ProjectState() {
    if (client_ == nullptr) {
        return;
    }
    const auto target = MapState(client_->state());
    // Ended与Hidden不同，必须先消费待隐藏标记，避免不断重发Ended而永不收尾。
    if (hidden_pending_ && ui_state_ == UiState::Ended && target == UiState::Hidden) {
        FireState(UiState::Hidden);
        ui_state_ = UiState::Hidden;
        hidden_pending_ = false;
    } else if (target != ui_state_) {
        if (target == UiState::Hidden && ui_state_ != UiState::Hidden) {
            /* 活动腿归位：先报 Ended 让页面收尾，下一轮再归 Hidden。 */
            FireState(UiState::Ended);
            ui_state_ = UiState::Ended;
            hidden_pending_ = true;
        } else {
            FireState(target);
            ui_state_ = target;
            if (target != UiState::Hidden) {
                hidden_pending_ = false;
            }
        }
    }
    if (ui_state_ == UiState::Hidden && !hidden_pending_) {
        /* 腿已归位：复位单轮旗标并销毁通话腿，空闲期不驻留任何重组件。 */
        offer_sent_ = false;
        connected_reported_ = false;
        mic_paused_ = camera_paused_ = false;
        TeardownLeg();
    }
}

void UrSignalingCall::Impl::RecoverIfFailed(std::uint64_t now) {
    (void)now;
    /* 失败腿先归 Idle（媒体/请求账本已关），让 ProjectState 走 Ended→Hidden
     * 收尾后统一 TeardownLeg 释放；无需在途恢复——下一条邀请会整体重建。 */
    if (client_ != nullptr && client_->state() == ur::device::HumanCallState::Failed) {
        client_->close();
    }
}

// StartMedia 接管本次通话的采集和播放，并按需启动视频；重复启动或缺少PCM桥时直接返回。
void UrSignalingCall::Impl::StartMedia() {
    if (media_active_ || pcm_ == nullptr) {
        return;
    }
    media_active_ = true;
    mic_paused_ = camera_paused_ = false;
    udp_camera_owned_ = false;
    if (media_)
        media_->ResetPlayback();
    playback_epoch_ = ++epoch_counter_;
    pcm_->begin_playback("human-call", playback_epoch_);
    pcm_->begin_capture("mic-1");
#if CONFIG_UR_AI_SIGNALING_WEBRTC_VIDEO
    // 先完成相机DMA分配，错峰启动对端画面；慢帧和音频缺帧由Poll停显。
    media_started_at_ = audio_capture_at_ = esp_timer_get_time() / 1000;
    remote_video_disabled_.store(false);
    audio_capture_gate_ = {};
    remote_video_pause_sent_ = false;
    remote_frame_at_.store(0);
    if (!voice_call_ && media_ && media_->IsUDP()) {
        auto* camera = Board::GetInstance().GetCamera();
        const bool camera_started = camera && camera->StartCompressedStream();
        if (!camera_started || !media_->StartUdpVideo()) {
            if (camera_started)
                camera->StopStream();
            camera_paused_ = true;
            ESP_LOGW(kTag, "UDP camera or video worker failed");
        } else {
            camera->SetStreamFrameInterval(40);
            udp_camera_owned_ = true;
        }
    } else if (!voice_call_)
        StartVideoUplink();
    renderer_start_at_ = !voice_call_ && !(media_ && media_->IsUDP()) && remote_video_selected_
                             ? media_started_at_ + 2000
                             : 0;
    video_uplink_pending_at_ = 0;
#endif
    NotifyTrackState();
    // 音频控制统一经应用任务：人际通话无需持续识别唤醒词，关闭时归还原状态。
#if CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER && CONFIG_UR_HUMAN_CALL_UDP_AV && \
    CONFIG_UR_SOFTWARE_AEC && !CONFIG_UR_AI_SIGNALING_DEVICE_AEC
    // 同步硬件参考不经过播放FIFO，视频也启用AEC；无硬件参考时保留纯语音策略。
    // codec 通过板级接口查询真实回采能力，避免用构建名称猜测硬件。
    auto* codec = Board::GetInstance().GetAudioCodec();
    const bool use_aec = voice_call_ || (codec && codec->input_reference());
#else
    // 已显式声明设备AEC或其他板型的原模式继续由配置决定。
    const bool use_aec = true;
#endif
    Application::GetInstance().Schedule([use_aec] { SetHumanCallAudioProcessing(true, use_aec); });
    ESP_LOGI(kTag, "call media started");
}

void UrSignalingCall::Impl::StopMedia() {
    if (!media_active_) {
        return;
    }
    // 空闲归位恢复接管前设置；已转入AI时，音频控制由应用状态机接续。
    Application::GetInstance().Schedule([] { SetHumanCallAudioProcessing(false); });
    media_active_ = false;
    if (media_)
        media_->ResetPlayback();
    if (pcm_ != nullptr) {
        pcm_->end_capture();
        if (playback_epoch_ != 0) {
            pcm_->end_playback(playback_epoch_);
        }
        /* 复位共享 PCM 的播放代次，避免 AI 会话重建后 epoch 回绕被拒。 */
        pcm_->reset_playback_epochs();
    }
#if CONFIG_UR_AI_SIGNALING_WEBRTC_VIDEO
    renderer_start_at_ = 0;
    if (media_ && media_->IsUDP()) {
        media_->StopUdpVideo();
        // 未成功接管或已暂停时不能停止其他功能持有的相机。
        if (udp_camera_owned_) {
            if (auto* camera = Board::GetInstance().GetCamera()) {
                camera->SetStreamFrameInterval(0);
                camera->StopStream();
            }
        }
        udp_camera_owned_ = false;
    } else
        StopVideoUplink();
    StopRenderer();
    remote_frame_at_.store(0);
    FireRemoteFrame(nullptr, 0, 0);
#endif
    ESP_LOGI(kTag, "call media stopped");
}

#if CONFIG_UR_AI_SIGNALING_WEBRTC_VIDEO
void UrSignalingCall::Impl::StartRenderer() {
    auto* renderer = ur::device::GetCallVideoRenderer();
    if (renderer == nullptr) {
        return;
    }
    remote_video_gate_ = {};
    renderer_started_ =
        renderer->Start([this, renderer](const std::uint8_t* rgb565, int width, int height) {
            if (remote_video_disabled_.load())
                return;
            const auto started = esp_timer_get_time();
            FireRemoteFrame(rgb565, width, height);
            const auto elapsed_ms =
                (esp_timer_get_time() - started + renderer->LastFrameProcessingUs()) / 1000;
            remote_frame_at_.store(esp_timer_get_time() / 1000);
            if (!remote_video_gate_.completed(esp_timer_get_time() / 1000, elapsed_ms))
                remote_video_disabled_.store(true);
        });
}

void UrSignalingCall::Impl::StopRenderer() {
    if (!renderer_started_) {
        return;
    }
    renderer_started_ = false;
    if (auto* renderer = ur::device::GetCallVideoRenderer()) {
        renderer->Stop();
    }
}

void UrSignalingCall::Impl::StartVideoUplink() {
    if (video_starter_ && video_starter_()) {
        video_uplink_frames_ = 0;
    }
}

void UrSignalingCall::Impl::RequestVideoIDR() {
#if CONFIG_UR_AI_SIGNALING_H264_UPLINK
    if (video_ != nullptr) {
        video_->RequestIDR();
    }
#endif
}

void UrSignalingCall::Impl::StopVideoUplink() {
    video_uplink_pending_at_ = 0;  // 取消在途的错峰启动。
#if CONFIG_UR_AI_SIGNALING_H264_UPLINK
    if (video_ != nullptr) {
        video_->Stop();
    }
#endif
}
#endif

#if CONFIG_UR_SPARKBOT_VOICE_MODEL_TEST
void UrSignalingCall::Impl::StartVoiceAnswerTest() {
    const auto generation = ui_generation_.load();
    voice_guard_.store(true);
    voice_guard_end_pending_ = false;
    voice_allowed_.store(true);
    voice_app_jobs_.fetch_add(1);
    Application::GetInstance().Schedule([this, generation] {
        if (voice_allowed_.load() && ui_generation_.load() == generation &&
            Application::GetInstance().GetDeviceState() == kDeviceStateIdle) {
            auto& audio = Application::GetInstance().GetAudioService();
            audio.BeginCallVoiceCapture(generation, [this](std::uint32_t token,
                                                          CallVoiceWorker::Outcome outcome) {
                // 回调发生时模型已经释放；旧代次仍由信令队列再次校验。
                if (outcome == CallVoiceWorker::Outcome::Recognized)
                    PostUiForGeneration(kUiAccept, token);
                voice_app_jobs_.fetch_add(1);
                Application::GetInstance().Schedule([this, token] {
                    if (ui_generation_.load() == token)
                        Application::GetInstance().GetAudioService().StopCallVoiceCapture();
                    voice_app_jobs_.fetch_sub(1);
                });
            });
        }
        voice_app_jobs_.fetch_sub(1);
    });
}

void UrSignalingCall::Impl::StopVoiceAnswerTest(bool end_guard) {
    voice_allowed_.store(false);
    if (end_guard) {
        voice_guard_end_pending_ = true;
        voice_accept_pending_ = false;
    }
    voice_app_jobs_.fetch_add(1);
    Application::GetInstance().Schedule([this] {
        Application::GetInstance().GetAudioService().StopCallVoiceCapture();
        voice_app_jobs_.fetch_sub(1);
    });
}

void UrSignalingCall::Impl::AdvanceVoiceAnswerGuard() {
    if (voice_guard_end_pending_ && voice_app_jobs_.load() == 0 &&
        !Application::GetInstance().GetAudioService().IsCallVoiceCaptureBusy()) {
        voice_guard_end_pending_ = false;
        voice_guard_.store(false);
    }
}

void UrSignalingCall::Impl::AdvanceVoiceAnswerAccept(std::uint64_t now) {
    if (!voice_accept_pending_) return;
    if (client_ == nullptr || ui_generation_.load() != voice_accept_generation_ ||
        client_->state() != ur::device::HumanCallState::Ringing) {
        voice_accept_pending_ = false;
        return;
    }
    if (voice_app_jobs_.load() != 0 ||
        Application::GetInstance().GetAudioService().IsCallVoiceCaptureBusy()) return;
    voice_accept_pending_ = false;
    if (!client_->accept(now)) ESP_LOGW(kTag, "ui accept rejected");
}
#endif

void UrSignalingCall::Impl::FireState(UiState state) {
#if CONFIG_UR_SPARKBOT_VOICE_MODEL_TEST
    if (state == UiState::Ringing) StartVoiceAnswerTest();
    else if (state == UiState::Ended || state == UiState::Hidden) StopVoiceAnswerTest(true);
#endif
    if (hooks_mutex_ == nullptr) {
        return;
    }
    if (xSemaphoreTake(hooks_mutex_, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }
    if (hooks_.on_call_kind)
        hooks_.on_call_kind(client_ && client_->mode() == ur::device::HumanCallMode::Voice);
    if (hooks_.on_state) {
        hooks_.on_state(state);
    }
    if (hooks_.on_state_generation) {
        hooks_.on_state_generation(state, ui_generation_.load());
    }
    xSemaphoreGive(hooks_mutex_);
}

void UrSignalingCall::Impl::FireRemoteFrame(const std::uint8_t* rgb565, int width, int height) {
    if (hooks_mutex_ == nullptr) {
        return;
    }
    if (xSemaphoreTake(hooks_mutex_, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }
    if (hooks_.on_remote_frame) {
        hooks_.on_remote_frame(rgb565, width, height);
    }
    xSemaphoreGive(hooks_mutex_);
}

void UrSignalingCall::Impl::NotifyTrackState() {
    if (hooks_mutex_ == nullptr) {
        return;
    }
    if (xSemaphoreTake(hooks_mutex_, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }
    if (hooks_.on_track_state) {
        hooks_.on_track_state(mic_paused_, camera_paused_);
    }
    xSemaphoreGive(hooks_mutex_);
}

UrSignalingCall::UiState UrSignalingCall::Impl::MapState(ur::device::HumanCallState state) {
    switch (state) {
        case ur::device::HumanCallState::Ringing:
            return UiState::Ringing;
        case ur::device::HumanCallState::Accepting:
        case ur::device::HumanCallState::Inviting:
        case ur::device::HumanCallState::Negotiating:
        case ur::device::HumanCallState::ConnectingMedia:
        case ur::device::HumanCallState::WaitingEstablished:
            return UiState::Connecting;
        case ur::device::HumanCallState::Connected:
            return UiState::Active;
        case ur::device::HumanCallState::Ending:
        case ur::device::HumanCallState::Failed:
            return UiState::Ended;
        default:
            return UiState::Hidden;
    }
}

/* ---- UrSignalingCall 公共入口：薄包装到 Impl ---- */

UrSignalingCall& UrSignalingCall::Instance() {
    static UrSignalingCall instance;
    return instance;
}

UrSignalingCall::UrSignalingCall() : impl_(new Impl()) {}
UrSignalingCall::~UrSignalingCall() { delete impl_; }

void UrSignalingCall::Attach(ur::device::UrSignalingMqttPort* port, const std::string& identity) {
    impl_->Attach(port, identity);
}

void UrSignalingCall::Detach() { impl_->Detach(); }

void UrSignalingCall::SetAudioBridge(ur::device::UrSignalingPcm* pcm,
                                     ur::device::UrSignalingOpus* codec) {
    impl_->SetAudioBridge(pcm, codec);
}

void UrSignalingCall::SetHostBusy(std::function<bool()> host_busy) {
    impl_->SetHostBusy(std::move(host_busy));
}

void UrSignalingCall::SetHostYield(std::function<bool()> yield) {
    impl_->SetHostYield(std::move(yield));
}

#if CONFIG_UR_AI_SIGNALING_WEBRTC_VIDEO
void UrSignalingCall::SetVideoPipeline(ur::device::UrSignalingVideo* video,
                                       std::function<bool()> starter) {
    impl_->SetVideoPipeline(video, std::move(starter));
}
#endif

void UrSignalingCall::SetUiHooks(UiHooks hooks) { impl_->SetHooks(std::move(hooks)); }

bool UrSignalingCall::HandleDownlink(const std::string& topic, const std::string& body,
                                     std::uint64_t now_ms, std::uint64_t wall_ms) {
    return impl_->Handle(topic, body, now_ms, wall_ms);
}

bool UrSignalingCall::HandleDownlink(std::string_view topic, std::string_view body,
                                     std::uint64_t now_ms, std::uint64_t wall_ms) {
    return impl_->Handle(topic, body, now_ms, wall_ms);
}

void UrSignalingCall::Poll(std::uint64_t now_ms, std::uint64_t wall_ms) {
    impl_->Poll(now_ms, wall_ms);
}

bool UrSignalingCall::Busy() const { return impl_->Busy(); }
#if CONFIG_UR_SPARKBOT_VOICE_MODEL_TEST
bool UrSignalingCall::VoiceAnswerGuardActive() const { return impl_->VoiceAnswerGuardActive(); }
#endif

void UrSignalingCall::UiAccept() { impl_->PostUi(Impl::kUiAccept); }
void UrSignalingCall::UiReject() { impl_->PostUi(Impl::kUiReject); }
void UrSignalingCall::UiHangUp() { impl_->PostUi(Impl::kUiHangUp); }
void UrSignalingCall::UiAccept(std::uint32_t generation) {
    impl_->PostUiForGeneration(Impl::kUiAccept, generation);
}
void UrSignalingCall::UiReject(std::uint32_t generation) {
    impl_->PostUiForGeneration(Impl::kUiReject, generation);
}
void UrSignalingCall::UiHangUp(std::uint32_t generation) {
    impl_->PostUiForGeneration(Impl::kUiHangUp, generation);
}
void UrSignalingCall::UiToggleMic() { impl_->PostUi(Impl::kUiToggleMic); }
void UrSignalingCall::UiToggleCamera() { impl_->PostUi(Impl::kUiToggleCamera); }

bool UrSignalingCall::Dial(const std::string& target, bool voice_only) {
    return impl_->PostDial(target, voice_only);
}

#endif /* CONFIG_UR_AI_SIGNALING_WEBRTC */
