// 文件说明：联犀语音信令会话、原生16k编解码装配及应用状态回调。
#include "esp_heap_caps.h"

#include "peer.h" /* libpeer：开机一次性完成 srtp/sctp 初始化，规避碎片期失败 */

#include "ur_signaling_protocol.h"
#include "ur_signaling_contacts.h"
#if CONFIG_UR_AI_SIGNALING_WEBRTC_VIDEO
#if CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER
#include "ur_av_probe.h"
#endif
#include "ur_signaling_call_video.h"
#endif

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>

#include <sys/time.h>
#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "application.h"
#include "board.h"
#include "display.h"
#include "ur/device/envelope.hpp"
#include "ur/device/voice_wire.hpp"
#include "ur_device.h"

namespace {
constexpr const char* kTag = "ur_sig";
/* 会话建立全程（连接、订阅、能力、创建、探测）在冷启动可能耗时数秒；
 * 上限与旧协议会话等待一致，避免唤醒路径长时间阻塞主任务。 */
constexpr TickType_t kEstablishedTimeout = pdMS_TO_TICKS(12000);
/* 命令与事件轮询周期；20ms 帧率下 10ms 轮询足够平滑。 */
constexpr TickType_t kTaskPollPeriod = pdMS_TO_TICKS(10);
/* 当前连接的能力缓存一小时过期，半小时前续报原声明，不依赖重新唤醒或重连。 */
constexpr std::uint64_t kCapabilityRefreshMs = 30 * 60 * 1000;
/* 队列深度覆盖并发交互入口；命令体很小，放内部 RAM。 */
constexpr UBaseType_t kCommandQueueLength = 8;
/* 采集自愈扫描节拍：低频检查"监听态但采集未开"，兼顾真机断言与时序开销。 */
constexpr std::uint64_t kCaptureHealScanMs = 1000;
/* 采集自愈最小补发间隔：重复触发安全，并让刚发出的 kStartInput 先落地
 * （start_input 在 want_capture_ 为真时返回 Busy，不会重复发布 audioStart）。 */
constexpr std::uint64_t kCaptureHealIntervalMs = 2000;
/* 等待平台回复的最长时间：超过即视为本轮回复已丢失（含媒体中断未重放），
 * 释放自愈闸门，避免设备长期保持"在听但不采集"的哑麦状态。 */
constexpr std::uint64_t kReplyWaitTimeoutMs = 10000;

std::uint64_t MonotonicMs() { return static_cast<std::uint64_t>(esp_timer_get_time() / 1000); }

/* 固件形态标记：连接分段表据此区分 WebRTC（含 offer/ICE/DTLS）与 UDP（无这些阶段）。 */
#if CONFIG_UR_AI_SIGNALING_WEBRTC
constexpr const char* kConnectFlavor = "webrtc";
#else
constexpr const char* kConnectFlavor = "udp";
#endif

std::uint64_t WallMs() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    if (tv.tv_sec <= 0) {
        /* 墙钟不可用时返回 0；仅焦点/私有上下文需要墙钟，语音会话不依赖。 */
        return 0;
    }
    return static_cast<std::uint64_t>(tv.tv_sec) * 1000ULL +
           static_cast<std::uint64_t>(tv.tv_usec) / 1000ULL;
}

std::string RandomInstanceId() {
    std::string instance;
    instance.reserve(32);
    while (instance.size() < 32) {
        uint32_t random = esp_random();
        char buffer[9] = {0};
        snprintf(buffer, sizeof(buffer), "%08" PRIx32, random);
        instance.append(buffer);
    }
    return instance;
}
}  // namespace

SignalingProtocol* SignalingProtocol::instance_ = nullptr;

SignalingProtocol::SignalingProtocol() {
    server_sample_rate_ = 16000;
    server_frame_duration_ = 20;
    commands_ = xQueueCreate(kCommandQueueLength, sizeof(CommandItem));
    events_ = xEventGroupCreate();
    instance_ = this;
}

SignalingProtocol::~SignalingProtocol() {
    if (task_ != nullptr) {
        // 队列满时重试关闭请求；停止屏障不能用两秒超时冒充任务已退出。
        while (task_running_.load() && !PostCommand(Command::kShutdown))
            vTaskDelay(pdMS_TO_TICKS(20));
        while (task_running_.load())
            vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (commands_ != nullptr) {
        vQueueDelete(commands_);
    }
    if (events_ != nullptr) {
        vEventGroupDelete(events_);
    }
    if (instance_ == this) {
        instance_ = nullptr;
    }
}

bool SignalingProtocol::Start() { return EnsureTaskRunning(); }

bool SignalingProtocol::EnsureTaskRunning() {
    if (task_running_.load()) {
        return true;
    }
    /* MQTT 设备身份由连接任务稍后填充（ur_setup_connect_params），本函数在应用
     * 启动期即被调用，存在冷启动竞态：identity 拿不到时若继续装配会在失败后
     * 重建资源。这里前置检查，未就绪即静默返回，由应用层 tick 幂等重试。 */
    char identity_probe[96] = {0};
    if (!ur_signaling_identity(identity_probe, sizeof(identity_probe))) {
        return false;
    }
    if (pcm_ == nullptr) {
        /* 本地编码输入采用AFE原生16k/20ms，避免持续收音额外升采样。
         * WebRTC的SDP/RTP时钟仍为48k；Opus载荷允许16k语音输入（RFC7587）。 */
        ur::device::AudioFormat format;
        format.sample_rate = 16000;
        format.channels = 1;
        format.frame_ms = 20;
        /* ESP32 上第二个 Opus 解码器与旧协议解码器并存会耗尽内部内存：
         * 信令会话期间挂起旧解码器，会话结束由 TaskLoop/析构恢复。 */
        Application::GetInstance().GetAudioService().SuspendDecoder();
        if (!codec_.configure(format)) {
            ESP_LOGE(kTag, "signaling codec configure failed");
            Application::GetInstance().GetAudioService().ResumeDecoder();
            return false;
        }
        /* PCM桥与本地编码器同为16k/20ms，不把RTP时钟误作输入采样率。 */
        ur::device::AudioFormat pcm_format;
        pcm_format.sample_rate = 16000;
        pcm_format.channels = 1;
        pcm_format.frame_ms = 20;
        pcm_ = std::make_unique<ur::device::UrSignalingPcm>(
            Application::GetInstance().GetAudioService(), pcm_format);
        if (!pcm_->valid()) {
            ESP_LOGE(kTag, "signaling pcm buffer invalid");
            pcm_.reset();
            return false;
        }
#if CONFIG_UR_AI_SIGNALING_WEBRTC
        /* 开机一次性完成 libpeer 初始化（srtp + sctp）：首次建 Peer 时堆已碎片化，
         * srtp_init 会 rc=2 alloc fail；peer_init 内部有一次性门，后续建链不再
         * 重跑加密自检（真机“接通中”且媒体不通的根因）。 */
        static bool s_peer_preinit_done = false;
        if (!s_peer_preinit_done) {
            s_peer_preinit_done = true;
            const int rc = peer_init();
            if (rc != 0) {
                ESP_LOGE(kTag, "libpeer preinit failed rc=%d free=%u", rc,
                         static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
            } else {
                ESP_LOGI(kTag, "libpeer preinit ok");
            }
        }
        /* WebRTC 媒体端口复用同一 16k/20ms PCM 桥；48k 重采样在适配层内部。 */
        ESP_LOGI(kTag, "[connect-ms] stage=media_assembly_begin flavor=%s t=%llu", kConnectFlavor,
                 static_cast<unsigned long long>(MonotonicMs()));
        media_ = std::make_unique<ur::device::WebRtcVoiceMedia>(*pcm_, codec_);
#if CONFIG_UR_HUMAN_CALL_UDP_AV
        // 仅登记可用 JPEG 相机，AI 视觉仍须单独装配 H264 管线。
        auto* call_camera = Board::GetInstance().GetCamera();
        static_cast<ur::device::WebRtcVoiceMedia*>(media_.get())->SetCallVideoAvailable(
            call_camera && call_camera->SupportsCompressedStream());
#endif
#if CONFIG_UR_AI_SIGNALING_H264_UPLINK
        /* 视频形态：注入摄像头管线后才对核心暴露 aiVision 能力。
         * 本板 GetCamera 实际返回 SscmaCamera*，构建无 RTTI 用 static_cast。 */
        Camera* camera = Board::GetInstance().GetCamera();
        auto* sscma = static_cast<SscmaCamera*>(camera);
        if (sscma != nullptr) {
            video_ = std::make_unique<ur::device::UrSignalingVideo>();
            auto* video_ptr = video_.get();
            auto* media_ptr = static_cast<ur::device::WebRtcVoiceMedia*>(media_.get());
            /* 启动闭包同时交给 AI 媒体与人际通话外观（后者经 SetVideoPipeline 注入）。 */
            video_starter_ = [sscma, video_ptr]() {
                return video_ptr->Start(sscma, [sscma] { return sscma->preview_frame(); });
            };
            media_ptr->SetVideoPipeline(video_ptr, video_starter_);
            ESP_LOGI(kTag, "video pipeline attached (camera ready)");
        } else {
            ESP_LOGW(kTag, "video flavor without SSCMA camera; aiVision disabled");
        }
#endif
#else
        media_ = std::make_unique<ur::device::UdpVoiceMedia>(*pcm_, codec_, udp_port_, cipher_);
#endif
        ESP_LOGI(kTag, "[connect-ms] stage=media_assembly_done flavor=%s t=%llu", kConnectFlavor,
                 static_cast<unsigned long long>(MonotonicMs()));
    }
    {
        char identity_buffer[96] = {0};
        if (!ur_signaling_identity(identity_buffer, sizeof(identity_buffer))) {
            ESP_LOGE(kTag, "device identity unavailable");
            return false;
        }
        identity_ = identity_buffer;
        capability_ = BuildCapability();
        runtime_config_.client_id = identity_;
        /* 共享连接的地址与凭据由联犀设备任务持有；核心只见适配层。 */
        runtime_config_.port = 1883;
    }
    task_running_.store(true);
    /* 信令状态机下行 JSON 解析+UDP 媒体装配调用链深，8KB/16KB 栈均实测溢出，
     * 给足 32KB PSRAM 栈。 */
    // UDP软件AEC双核构建的事件泵实测固定在繁忙的Core0，显式放到算法侧Core1。
    // AI复用此线程，须随人际通话一起回归；任务优先级、栈及失败回收保持原合同。
#if CONFIG_UR_HUMAN_CALL_UDP_AV && CONFIG_UR_SOFTWARE_AEC && CONFIG_FREERTOS_NUMBER_OF_CORES > 1
    // 信令任务必须先排空音视频UDP突发；编解码已在独立低一级任务运行，提升一档不会把DSP搬回网络线程。
    if (xTaskCreatePinnedToCoreWithCaps(TaskEntry, "ur_signal", 32768, this, 6, &task_, 1,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
#else
    if (xTaskCreateWithCaps(TaskEntry, "ur_signal", 32768, this, 6, &task_,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
#endif
        ESP_LOGE(kTag, "failed to create signaling task");
        task_running_.store(false);
        task_ = nullptr;
        return false;
    }
    return true;
}

ur::device::VoiceCapability SignalingProtocol::BuildCapability() const {
    ur::device::VoiceCapability capability;
    capability.modes.half_duplex = true;
    /* 协议校验要求 fullDuplex 声明必须带 device AEC；无 AEC 构建只声明半双工，
     * 否则能力上报被判 Invalid、会话永远无法建立（真机会话建立超时根因）。 */
#if CONFIG_UR_AI_SIGNALING_DEVICE_AEC
    capability.modes.full_duplex = true;
    capability.modes.device_aec = true;
#else
    capability.modes.full_duplex = false;
    capability.modes.device_aec = false;
#endif
    capability.audio.sample_rate = 16000;
    capability.audio.channels = 1;
    capability.audio.frame_ms = 20;
    capability.revision = 1;
    capability.transport = ur::device::VoiceTransport::Udp;
#if CONFIG_UR_AI_SIGNALING_WEBRTC
    /* WebRTC 形态在默认声明之后覆盖：协议校验要求 transport=webrtc 且
     * 音频 48k/20ms；能力上报被硬编码 UDP/16k 覆盖会导致平台按 UDP
     * 口径下发媒体（真机 media rate=16000 被拒的根因）。 */
    capability.transport = ur::device::VoiceTransport::WebRTC;
    capability.audio.sample_rate = 48000;
#if CONFIG_UR_AI_SIGNALING_WEBRTC_VIDEO
    // AI 视觉与人际通话分别检查管线；空闲和运行时使用同一相机判据。
    auto* camera = Board::GetInstance().GetCamera();
#if CONFIG_UR_AI_SIGNALING_H264_UPLINK
    if (camera) {
        capability.ai_vision = true;
    }
#endif
#if CONFIG_UR_HUMAN_CALL_UDP_AV
    if (camera && camera->SupportsCompressedStream()) {
#else
    if (camera) {
#endif
        capability.human_call = true;
        capability.human_voice_call = true;
#if CONFIG_UR_HUMAN_CALL_UDP_AV
        capability.human_udp_av = true;
#endif
        /* UDP AV 同样具备 JPEG 解码与屏幕渲染路径；声明接收能力后，平台会在
         * 网页视频通话邀请中加入 video-in，设备再按本次授权接收网页画面。 */
        capability.human_video_receive = ur::device::GetCallVideoRenderer() != nullptr;
    }
#endif
#endif
    return capability;
}

bool SignalingProtocol::ReportIdleCapability() {
#if CONFIG_UR_AI_SIGNALING_WEBRTC
    if (identity_.empty()) {
        return false;
    }
    const auto capability = BuildCapability();
    const ur::device::VoiceVisionProfile* vision = nullptr;
#if CONFIG_UR_AI_SIGNALING_WEBRTC_VIDEO
    if (capability.ai_vision || capability.human_call) {
        vision = ur::device::WebRtcVoiceMedia::DefaultVisionProfile();
    }
#endif
    ur::device::Envelope envelope;
    char token[32];
    snprintf(token, sizeof(token), "idle-cap-%08x", static_cast<unsigned>(esp_random()));
    if (ur::device::capability_command(token, capability, envelope, vision) !=
        ur::device::WireResult::Ok) {
        ESP_LOGW(kTag, "idle capability encode rejected");
        return false;
    }
    std::string body;
    if (ur::device::encode_envelope(envelope, body) != ur::device::DecodeResult::Ok) {
        ESP_LOGW(kTag, "idle capability envelope encode failed");
        return false;
    }
    const std::string topic = "$thing/up/signaling/" + identity_;
    if (!mqtt_port_.publish(topic, body)) {
        ESP_LOGW(kTag, "idle capability publish not accepted");
        return false;
    }
    ESP_LOGI(kTag, "idle capability reported bytes=%u aiVision=%d humanCall=%d",
             static_cast<unsigned>(body.size()), capability.ai_vision ? 1 : 0,
             capability.human_call ? 1 : 0);
    return true;
#else
    return false;
#endif
}

void SignalingProtocol::MaintainCapability(std::uint64_t now) {
#if CONFIG_UR_AI_SIGNALING_WEBRTC
    if (!idle_down_subscribed_.load() || now < idle_capability_retry_at_) {
        return;
    }
    // 只重发当前装配的原能力，不改变修订、邀请、媒体或既有通话状态。
    idle_capability_retry_at_ = now + (ReportIdleCapability() ? kCapabilityRefreshMs : 5000);
#else
    (void)now;
#endif
}

void SignalingProtocol::EnsureCallAppearance() {
#if CONFIG_UR_AI_SIGNALING_WEBRTC
    if (call_appearance_ready_ || pcm_ == nullptr || identity_.empty()) {
        return;
    }
    /* 人际通话外观绑定共享端口、媒体桥、占用查询与让位请求；runtime 重建后
     * 重复绑定安全。无 AI 运行时的冷启动也必须提前绑定，否则来电分派与接听
     * 状态机不会推进（真机来电停在队列根因）。 */
    ESP_LOGI(kTag, "heap probe pre-attach internal=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    UrSignalingCall::Instance().SetAudioBridge(pcm_.get(), &codec_);
    UrSignalingCall::Instance().SetHostBusy([this] {
        const auto voice_state =
            runtime_ ? runtime_->voice().state() : ur::device::VoiceState::Idle;
        return voice_state != ur::device::VoiceState::Idle &&
               voice_state != ur::device::VoiceState::Failed;
    });
    /* 让位回调在信令任务内同步执行（Handle/Poll 上下文）：直接走 runtime end
     * 等价 kEndSession 的任务内路径，不 PostCommand 也不等待事件——命令队列
     * 由本任务消费，阻塞等待会死锁。会话结束回调由 TaskLoop 的 Idle 边缘统一
     * 触发（InvalidateSession/kSessionFinishedEvent），与应用层关闭路径一致。 */
    UrSignalingCall::Instance().SetHostYield([this] {
        if (runtime_ == nullptr || !runtime_started_.load()) {
            return true; /* 无 AI 运行时：视为已让位 */
        }
        const auto voice_state = runtime_->voice().state();
        if (voice_state == ur::device::VoiceState::Idle ||
            voice_state == ur::device::VoiceState::Failed) {
            return true; /* 无存活 AI 会话：已让位 */
        }
        runtime_->end(MonotonicMs());
        ESP_LOGI(kTag, "human call yield: ending AI voice session");
        return true;
    });
#if CONFIG_UR_AI_SIGNALING_H264_UPLINK
    if (video_) {
        auto* camera = static_cast<SscmaCamera*>(Board::GetInstance().GetCamera());
        auto* video = video_.get();
        // 人际通话单独选低负载档；AI媒体闭包仍保持原规格。
        UrSignalingCall::Instance().SetVideoPipeline(video, [camera, video] {
            return video->Start(
                camera, [camera] { return camera->preview_frame(); },
                ur::device::UrSignalingVideo::Profile::HumanCall);
        });
    }
#endif
    UrSignalingCall::Instance().Attach(&mqtt_port_, identity_);
    UrSignalingContacts::Instance().Attach(&mqtt_port_, identity_, RandomInstanceId());
    call_appearance_ready_ = true;
    ESP_LOGI(kTag, "heap probe post-attach internal=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
#endif
}

bool SignalingProtocol::EnsureRuntimeCreated() {
    if (runtime_started_.load() && runtime_ != nullptr) {
        return true;
    }
    ESP_LOGI(kTag, "[connect-ms] stage=runtime_create_begin flavor=%s t=%llu", kConnectFlavor,
             static_cast<unsigned long long>(MonotonicMs()));
    if (pcm_ == nullptr || media_ == nullptr) {
        return false;
    }
    if (runtime_ != nullptr) {
        /* 上一次连接级失败后的残留对象；先按关闭流程释放再重建。 */
        runtime_->close(MonotonicMs());
        runtime_.reset();
    }
    runtime_ = std::make_unique<ur::device::VoiceRuntime>(mqtt_port_, *media_, identity_,
                                                          capability_, RandomInstanceId());
    /* 会话级视觉请求在连接前传入核心；能力上报不受影响，只决定本次三轨与否。 */
    runtime_->request_vision_session(vision_session_requested_.load());
    /* 调试期开启首因诊断：Failed 时打印 runtime 失败枚举定位断点。 */
    runtime_->enable_failure_diagnostics();
    ESP_LOGI(kTag, "runtime created; starting voice session");
    if (runtime_->start(runtime_config_, MonotonicMs()) != ur::device::Result::Applied) {
        ESP_LOGE(kTag, "runtime start rejected");
        runtime_.reset();
        return false;
    }
    runtime_started_.store(true);
    ESP_LOGI(kTag, "[connect-ms] stage=runtime_started flavor=%s t=%llu", kConnectFlavor,
             static_cast<unsigned long long>(MonotonicMs()));
#if CONFIG_UR_AI_SIGNALING_WEBRTC
    EnsureCallAppearance();
    /* 下行分派钩子注册到核心：humanCall 用途与人际腿报文在 AI 分支前被消费。 */
    runtime_->set_human_dispatch(
        [](std::string_view topic, std::string_view body, std::uint64_t now, std::uint64_t wall) {
            return UrSignalingContacts::Instance().Handle(topic, body) ||
                   UrSignalingCall::Instance().HandleDownlink(topic, body, now, wall);
        });
#endif
    return true;
}

bool SignalingProtocol::PostCommand(Command type) {
    if (commands_ == nullptr) {
        return false;
    }
    CommandItem item{type};
    return xQueueSend(commands_, &item, 0) == pdTRUE;
}

// WaitForNextPoll 仅在本人际腿占用时使用编码通知；不占用AI等待或改变其节拍。
void SignalingProtocol::WaitForNextPoll() {
#if CONFIG_UR_AI_SIGNALING_WEBRTC
    if (call_appearance_ready_ && UrSignalingCall::Instance().Busy()) {
        if (ulTaskNotifyTake(pdTRUE, kTaskPollPeriod) > 0)
            vTaskDelay(1);  // 有结果也让出至少一tick，禁止持续通知变成不让出的忙循环。
        return;
    }
#endif
    vTaskDelay(kTaskPollPeriod);
}

void SignalingProtocol::TaskEntry(void* context) {
    auto* protocol = static_cast<SignalingProtocol*>(context);
    protocol->TaskLoop();
    protocol->task_running_.store(false);
    vTaskDeleteWithCaps(nullptr);
}

void SignalingProtocol::TaskLoop() {
    bool running = true;
    while (running) {
        CommandItem item;
        while (xQueueReceive(commands_, &item, 0) == pdTRUE) {
            if (item.type == Command::kShutdown) {
                running = false;
                break;
            }
            /* 一次触发可能排队多个 kCreateSession（应用重试+串口命令叠加）；
             * 连续重建 runtime 会互相排空订阅确认事件，合并为一次重建。 */
            if (item.type == Command::kCreateSession) {
                CommandItem next;
                while (xQueuePeek(commands_, &next, 0) == pdTRUE &&
                       next.type == Command::kCreateSession) {
                    xQueueReceive(commands_, &next, 0);
                }
            }
            HandleCommand(item.type);
        }
        if (!running) {
            break;
        }

#if CONFIG_UR_AI_SIGNALING_WEBRTC
        UrSignalingContacts::Instance().Poll(MonotonicMs());
#endif
        if (runtime_ == nullptr) {
            /* TaskLoop 线程先于首个 kCreateSession 运行；runtime 未创建前只等命令，
             * 解引用空 runtime 会 LoadProhibited panic（OTA 分区回滚的根因）。 */
#if CONFIG_UR_AI_SIGNALING_WEBRTC
            /* 空闲期常驻订阅信令下行：humanCall 来电由平台主动投递，此前订阅只随
             * AI 会话建立，冷启动后无人订阅导致来电投递被 Broker 拒（真机
             * nativeDeviceUnavailable 实证）。runtime 建立后 VoiceClient 会重复
             * 订阅同一主题，Broker 端幂等。 */
            if (!idle_down_subscribed_.load() && !identity_.empty()) {
                const int mid = mqtt_port_.subscribe("$thing/down/signaling/" + identity_);
                if (mid > 0) {
                    idle_down_subscribed_.store(true);
                    ESP_LOGI(kTag, "idle signaling down subscribed (mid=%d)", mid);
                }
            }
            /* 冷启动与长期空闲都续报同一能力，发布失败五秒后再试。 */
            MaintainCapability(MonotonicMs());
            /* 无 AI 运行时也必须推进人际通话：先绑定通话外观，再排空共享端口
             * 事件交给它，并驱动状态机发送接听/媒体应答。此前只有 VoiceRuntime
             * 存在时才分派，冷启动来电虽已订阅但永远停在队列（真机实证）。 */
            EnsureCallAppearance();
            if (call_appearance_ready_) {
                const std::uint64_t idle_now = MonotonicMs();
                for (unsigned count = 0; count < 32; ++count) {
                    const auto event = mqtt_port_.take_event();
                    if (!event.has_value()) {
                        break;
                    }
                    if (event->kind == ur::device::MqttEvent::Kind::Message) {
                        if (!UrSignalingContacts::Instance().Handle(event->topic, event->body))
                            UrSignalingCall::Instance().HandleDownlink(event->topic, event->body,
                                                                   idle_now, WallMs());
                    }
                }
                UrSignalingCall::Instance().Poll(idle_now, WallMs());
            }
#endif
            WaitForNextPoll();
            continue;
        }
        const std::uint64_t now = MonotonicMs();
#if CONFIG_UR_AI_SIGNALING_WEBRTC
        // 人际长通话或AI运行时也续期，不能等回到无runtime分支才刷新。
        MaintainCapability(now);
        /* 事件消费接管：AI 运行时 Idle/Failed（人际来电让位结束 AI 会话后即如此）
         * 时 poll 不再消费共享 MQTT 事件队列，人际腿的下行（邀请/ACK/mediaAnswer/
         * sessionEstablished）会滞留队列——真机 sessionAccept 应答断链根因。
         * 由本循环直接排空事件并按用途交给通话外观；AI 侧陈旧报文在其会话重建
         * 时由 mqtt connect 排空，不受影响。Running/Draining 仍由 runtime 消费。 */
        const auto runtime_state = runtime_->state();
        if (runtime_state == ur::device::RuntimeState::Idle ||
            runtime_state == ur::device::RuntimeState::Failed) {
            for (unsigned count = 0; count < 32; ++count) {
                const auto event = mqtt_port_.take_event();
                if (!event.has_value()) {
                    break;
                }
                if (event->kind == ur::device::MqttEvent::Kind::Message) {
                    if (!UrSignalingContacts::Instance().Handle(event->topic, event->body))
                        UrSignalingCall::Instance().HandleDownlink(event->topic, event->body, now,
                                                               WallMs());
                }
            }
        }
#endif
        /* 运行时 poll 内部泵 MQTT 事件、媒体双向与播放排空；不得外层重复取事件。 */
        runtime_->poll(now, WallMs());
        pcm_->PumpPlayback();
#if CONFIG_UR_AI_SIGNALING_WEBRTC
        /* 人际通话状态机与媒体在同一信令任务推进；未通话时为空操作。 */
        UrSignalingCall::Instance().Poll(now, WallMs());
#endif
        HandlePlaybackEdges();

        while (auto content = runtime_->take_content()) {
            HandleContent(*content);
        }

        const auto state = runtime_->voice().state();
        /* 状态变化即打印：定位半双工轮次中 voice 意外回 Idle 的准确时刻。 */
        if (state != last_voice_state_) {
            last_voice_state_ = state;
            ESP_LOGI(kTag, "voice state -> %d (runtime=%d)", static_cast<int>(state),
                     static_cast<int>(runtime_ ? runtime_->state() : ur::device::RuntimeState::Idle));
            if (state == ur::device::VoiceState::Recovering) {
                /* 媒体重协商作废平台本轮回复（旧 Peer 已停流，回复不会续播）：
                 * 清等待闸门，恢复链闭环后自愈才能把采集重新打开。 */
                awaiting_reply_.store(false);
                ESP_LOGI(kTag, "capture self-heal: reply wait cleared by media recovery");
            }
        }
        if (state == ur::device::VoiceState::Established) {
            if (!session_active_.exchange(true)) {
                full_duplex_.store(runtime_->voice().mode() == ur::device::Mode::FullDuplex);
                {
                    std::lock_guard<std::mutex> lock(vad_mutex_);
                    vad_endpoint_.Begin(static_cast<int64_t>(esp_timer_get_time()));
                }
                ESP_LOGI(kTag, "session established mode=%s full_duplex=%d",
                         full_duplex_.load() ? "fullDuplex" : "halfDuplex", full_duplex_.load());
                ESP_LOGI(kTag, "[connect-ms] stage=voice_established mode=%s t=%llu",
                         full_duplex_.load() ? "fullDuplex" : "halfDuplex",
                         static_cast<unsigned long long>(MonotonicMs()));
            }
            xEventGroupSetBits(events_, kSessionEstablishedEvent);
            /* 视觉提问可能在会话建立前已排队（sig_ask 早于 established）：就绪即补发；
             * attach 完成前平台暂拒时由下方循环周期重试直到受理。 */
            if (!pending_text_.empty()) {
                SendPendingText("established");
            }
        } else if (state == ur::device::VoiceState::Failed ||
                   runtime_->state() == ur::device::RuntimeState::Failed) {
            if (session_active_.exchange(false)) {
                /* 调试期领取 runtime 首因枚举，定位 voice Failed 的实际触发分支。 */
                if (auto failure = runtime_->take_failure_diagnostic()) {
                    ESP_LOGW(kTag, "runtime failure: %s",
                             std::string(ur::device::runtime_failure_name(*failure)).c_str());
                }
                InvalidateSession("session failed");
            } else {
                /* 诊断：未激活会话的失败也要暴露首因，定位建立期回落。 */
                if (auto failure = runtime_->take_failure_diagnostic()) {
                    ESP_LOGW(kTag, "runtime failure (inactive): %s",
                             std::string(ur::device::runtime_failure_name(*failure)).c_str());
                }
            }
            xEventGroupSetBits(events_, kSessionFinishedEvent);
            if (runtime_->state() == ur::device::RuntimeState::Failed) {
                /* 连接级失败后核心不再复用；复位标志允许下一次完全重启。 */
                runtime_started_.store(false);
            }
        } else if (state == ur::device::VoiceState::Idle && session_active_.load()) {
            session_active_.store(false);
            full_duplex_.store(false);
            InvalidateSession("session ended");
            xEventGroupSetBits(events_, kSessionFinishedEvent);
        }
        /* 视觉提问重试：CLI 可能早于 attach 排队，平台在 attach 前暂拒；
         * Established 期间每约 2 秒重试直到 Applied（成功后清空）。 */
        static std::uint64_t last_text_retry_ms = 0;
        const std::uint64_t now_ms = MonotonicMs();
        if (!pending_text_.empty() && session_active_.load() &&
            runtime_->voice().state() == ur::device::VoiceState::Established &&
            now_ms - last_text_retry_ms >= 2000) {
            last_text_retry_ms = now_ms;
            SendPendingText("retry");
        }
        /* 采集自愈：媒体恢复链闭环后应用预期"在听且能说话"，但半双工播放期关麦与
         * 延迟开麦都可能落空，此处按设备事实低频请求应用重放开麦（见实现处注释）。 */
        if (now_ms - capture_heal_scan_ms_ >= kCaptureHealScanMs) {
            capture_heal_scan_ms_ = now_ms;
            HealListeningCapture(now_ms);
        }
        CheckVadEndpoint();
        WaitForNextPoll();
    }

#if CONFIG_UR_AI_SIGNALING_WEBRTC
    if (call_appearance_ready_) {
        UrSignalingContacts::Instance().Detach();
        UrSignalingCall::Instance().Detach();  // 编码退出屏障先于PCM宿主及当前任务销毁。
        call_appearance_ready_ = false;
    }
#endif
    if (runtime_ != nullptr) {
        runtime_->close(MonotonicMs());
        runtime_.reset();
    }
    Application::GetInstance().GetAudioService().ResumeDecoder();
    session_active_.store(false);
    full_duplex_.store(false);
    runtime_started_.store(false);
    xEventGroupSetBits(events_, kSessionFinishedEvent);
}

void SignalingProtocol::HandleCommand(Command command) {
    switch (command) {
        case Command::kEnsureStarted:
            EnsureRuntimeCreated();
            break;
        case Command::kCreateSession:
            // 同一事件任务先退休旧通知再接管本次请求，旧失败不能唤醒本轮建连等待。
            xEventGroupClearBits(
                events_, kSessionEstablishedEvent | kSessionFinishedEvent | kSessionBusyEvent);
            xEventGroupSetBits(events_, kSessionCreateStartedEvent);
#if CONFIG_UR_AI_SIGNALING_WEBRTC
            if (UrSignalingCall::Instance().Busy()) {
                /* 通话占用媒体：拒绝本次 AI 会话重建，通话结束后由应用重试。 */
                ESP_LOGW(kTag, "session create deferred: human call active");
                // 应用线程正在等待本次建连；及时唤醒，避免误判超时并重连 MQTT。
                xEventGroupSetBits(events_, kSessionBusyEvent);
                break;
            }
#endif
            if (!EnsureRuntimeCreated()) {
                xEventGroupSetBits(events_, kSessionFinishedEvent);
                break;
            }
            {
                /* 除已建立外任何状态（含卡在协商/结束的残留状态）都整体重建：
                 * 按契约重新能力上报+新会话，共享 MQTT 连接不受影响。 */
                const auto state = runtime_->voice().state();
                if (state != ur::device::VoiceState::Established) {
                    runtime_->close(MonotonicMs());
                    runtime_.reset();
                    runtime_started_.store(false);
                    if (!EnsureRuntimeCreated()) {
                        ESP_LOGW(kTag, "session restart failed");
                    }
                }
            }
            break;
        case Command::kStartInput:
            if (runtime_ != nullptr) {
                const auto result = runtime_->start_input(MonotonicMs());
                /* 非 Applied 分支必须可见：自愈补发在采集已开/播放中会被拒为
                 * Busy/Duplicate（这就是幂等保证），真机断言需要区分"被拒"与"未发出"。 */
                ESP_LOGI(kTag, "audioStart requested result=%d", static_cast<int>(result));
                ESP_LOGI(kTag, "[connect-ms] stage=audio_start_send result=%d t=%llu",
                         static_cast<int>(result), static_cast<unsigned long long>(MonotonicMs()));
            }
            /* 命令已处理（无论 Applied/Busy）：解除"在途"闸门，后续按实际采集状态自愈。 */
            start_command_pending_.store(false);
            break;
        case Command::kStopInput:
            if (runtime_ != nullptr) {
                runtime_->stop_input(MonotonicMs());
                ESP_LOGI(kTag, "audioStop requested");
            }
            break;
        case Command::kCancelResponse:
            if (runtime_ != nullptr &&
                runtime_->cancel_response(MonotonicMs()) == ur::device::Result::Applied) {
                ESP_LOGI(kTag, "respCancel requested");
            }
            break;
        case Command::kSendText:
            if (runtime_ != nullptr && !pending_text_.empty()) {
                if (runtime_->voice().state() != ur::device::VoiceState::Established) {
                    /* 会话未就绪：保留正文，established 边缘重试（见 TaskLoop）。 */
                    break;
                }
                SendPendingText("command");
            }
            break;
        case Command::kEndSession:
            if (runtime_ != nullptr) {
                runtime_->end(MonotonicMs());
            }
            break;
        default:
            break;
    }
}

bool SignalingProtocol::WaitForEstablished(TickType_t timeout, bool* call_busy, bool* finished) {
    const TickType_t started = xTaskGetTickCount();  // 无论旧通知出现几次，都共用本次建连预算。
    TickType_t remaining = timeout;                  // 无符号tick差兼容计数器回绕。
    if (await_session_create_) {
        const EventBits_t accepted =
            xEventGroupWaitBits(events_, kSessionCreateStartedEvent, pdTRUE, pdFALSE, timeout);
        await_session_create_ = false;
        if ((accepted & kSessionCreateStartedEvent) == 0) {
            *call_busy = false;
            *finished = false;
            return false;
        }
        const TickType_t elapsed = xTaskGetTickCount() - started;  // 两阶段共享同一截止时间。
        remaining = elapsed < timeout ? timeout - elapsed : 0;
    }
    const EventBits_t bits = xEventGroupWaitBits(
        events_, kSessionEstablishedEvent | kSessionFinishedEvent | kSessionBusyEvent, pdTRUE,
        pdFALSE, remaining);
    *call_busy = (bits & kSessionBusyEvent) != 0;
    *finished = (bits & kSessionFinishedEvent) != 0;
    // 占用或结束与就绪同时到达时，不能把已结束的会话当成本次成功。
    return !*call_busy && !*finished && (bits & kSessionEstablishedEvent) != 0;
}

bool SignalingProtocol::OpenAudioChannel() {
#if CONFIG_UR_AI_SIGNALING_WEBRTC_VIDEO && CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER
    if (UrAVProbeActive()) return false;
#endif
    ESP_LOGI(kTag, "[connect-ms] stage=open_audio_channel_begin t=%llu",
             static_cast<unsigned long long>(MonotonicMs()));
    if (!EnsureTaskRunning()) {
        SetError("服务器连接失败");
        return false;
    }
    const auto state = runtime_ ? runtime_->voice().state() : ur::device::VoiceState::Idle;
    if (state != ur::device::VoiceState::Established || !runtime_started_.load()) {
        /* 非已建立的任何状态都走重建命令；事件任务内兜底关闭卡死残留。 */
        await_session_create_ = true;
        xEventGroupClearBits(events_, kSessionEstablishedEvent | kSessionFinishedEvent |
                                          kSessionBusyEvent | kSessionCreateStartedEvent);
        if (!PostCommand(Command::kCreateSession)) {
            await_session_create_ = false;
            SetError("会话请求繁忙，请重试");
            return false;
        }
    }
    bool call_busy = false;
    bool finished = false;
    if (!WaitForEstablished(kEstablishedTimeout, &call_busy, &finished)) {
        if (call_busy) {
            ESP_LOGI(kTag, "AI session unavailable while human call is active");
            return false;
        }
        // 让位结束旧 AI 腿也会唤醒等待者；只有实际等待超时才触发网络自愈。
        if (finished) {
            ESP_LOGW(kTag, "AI session ended before establishment");
            return false;
        }
        const auto timeout_state =
            runtime_ ? runtime_->voice().state() : ur::device::VoiceState::Idle;
        ESP_LOGE(kTag, "session establishment timeout state=%d",
                 static_cast<int>(timeout_state));
        /* 建会话前段（能力上报/建会话）超时且从未收到任何下行应答：正常平台对
         * capabilityReport 必回 200/409，零应答说明平台侧连接绑定失效（平台重启
         * 后 TCP 不断但上行被静默丢弃）。强制 MQTT 重连自愈，下一次会话重建即恢复。 */
        if (timeout_state == ur::device::VoiceState::Idle ||
            timeout_state == ur::device::VoiceState::Reporting ||
            timeout_state == ur::device::VoiceState::Creating) {
            ur_device_force_mqtt_reconnect();
        }
        SetError("会话建立超时");
        return false;
    }
    ESP_LOGI(kTag, "[connect-ms] stage=open_audio_channel_ok t=%llu",
             static_cast<unsigned long long>(MonotonicMs()));
    session_active_.store(true);
    last_incoming_time_ = std::chrono::steady_clock::now();
    error_occurred_ = false;
    if (on_audio_channel_opened_ != nullptr) {
        on_audio_channel_opened_();
    }
    return true;
}

bool SignalingProtocol::OpenVisionChannel() {
    /* aiVision 三轨视觉会话：仅本次连接生效，失败立即复位为默认语音形态。 */
    vision_session_requested_.store(true);
    if (!OpenAudioChannel()) {
        vision_session_requested_.store(false);
        return false;
    }
    return true;
}

void SignalingProtocol::InvalidateSession(const char* reason) {
    ESP_LOGW(kTag, "session invalidated: %s", reason);
    full_duplex_.store(false);
    awaiting_reply_.store(false); /* 会话终结后等待窗口作废，下一次会话重新判定。 */
    vision_session_requested_.store(false); /* 会话终结后下一次连接回默认语音形态。 */
    if (on_audio_channel_closed_ != nullptr) {
        on_audio_channel_closed_();
    }
}

void SignalingProtocol::CloseAudioChannel(bool send_goodbye) {
    if (!task_running_.load() || !session_active_.load()) {
        return;
    }
    /* 结束会话立即停采集；等待确认不是用户可感知路径，直接返回。 */
    PostCommand(Command::kEndSession);
    session_active_.store(false);
    full_duplex_.store(false);
    awaiting_reply_.store(false); /* 主动挂断同样作废等待窗口：自愈不得补开麦。 */
    vision_session_requested_.store(false); /* 主动关闭同样回默认语音形态。 */
    if (on_audio_channel_closed_ != nullptr) {
        on_audio_channel_closed_();
    }
}

bool SignalingProtocol::IsAudioChannelOpened() const {
    /* 跨线程只读原子镜像，避免与信令任务的状态写入竞争。 */
    return task_running_.load() && session_active_.load() && !error_occurred_;
}

bool SignalingProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> packet) {
    /* 信令链路的麦克风数据经 PCM 分流直采；旧 60ms 编码包不再上行。 */
    (void)packet;
    return true;
}

void SignalingProtocol::SendWakeWordDetected(const std::string& wake_word) {
    (void)wake_word;
    /* 信令上行只发送 audioStart 后的实时 Opus，不上传唤醒词。 */
}

void SignalingProtocol::SendStartListening(ListeningMode mode) {
    /* 新的监听轮开始：不再等旧回复，等待闸门清位。 */
    {
        std::lock_guard<std::mutex> lock(vad_mutex_);
        vad_endpoint_.Begin(esp_timer_get_time());
        vad_auto_stop_ = mode != kListeningModeManualStop;
        awaiting_reply_.store(false);
    }
    /* aiVision 会话生命周期由用户应用端 attach/detach 控制：自动开麦会在静音后
     * 触发 VAD 收尾结束会话（真机 consent 排查定位），视觉提问期保持不开麦。
     * 设备本地语音会话（aiConversation 两轨）正常开麦：按会话实际形态判定，
     * 不再由摄像头能力一刀切禁麦（真机点击设备说话无响应根因）。 */
#if CONFIG_UR_AI_SIGNALING_WEBRTC_VIDEO
    if (session_active_.load() && runtime_ != nullptr &&
        runtime_->voice().vision_session()) {
        ESP_LOGI(kTag, "capture start skipped: aiVision session keeps mic closed");
        return;
    }
#endif
    ESP_LOGI(kTag, "capture start requested mode=%d established=%d", static_cast<int>(mode),
             session_active_.load());
    ESP_LOGI(kTag, "[connect-ms] stage=capture_request mode=%d t=%llu", static_cast<int>(mode),
             static_cast<unsigned long long>(MonotonicMs()));
    /* 标记命令在途：信令任务处理前，采集自愈不得再补发一条 kStartInput。 */
    start_command_pending_.store(true);
    if (!PostCommand(Command::kStartInput)) {
        start_command_pending_.store(false);
    }
}

void SignalingProtocol::SendStopListening() {
    /* 应用显式停麦（挂断/退出监听）：不再期待平台回复，自愈不得补发开麦。 */
    {
        std::lock_guard<std::mutex> lock(vad_mutex_);
        vad_endpoint_.Begin(esp_timer_get_time());
        vad_auto_stop_ = false;
        awaiting_reply_.store(false);
    }
    PostCommand(Command::kStopInput);
}

void SignalingProtocol::OnLocalVadState(bool speaking) {
    /* 全双工持续上行，静音判句尾由服务端负责；半双工沿用本地 VAD 句尾。
     * 本回调只记录边沿，静音持续时间由信令任务周期检查。 */
    if (full_duplex_.load()) {
        return;
    }
    std::lock_guard<std::mutex> lock(vad_mutex_);
    vad_endpoint_.Update(speaking, esp_timer_get_time());
}

// CheckVadEndpoint 由信令任务周期调用，停麦未受理时保留本轮截止以便重试。
void SignalingProtocol::CheckVadEndpoint() {
    std::lock_guard<std::mutex> lock(vad_mutex_);
    if (vad_auto_stop_ && !full_duplex_.load() && session_active_.load() && !awaiting_reply_.load() &&
        runtime_ != nullptr && runtime_->voice().capturing() &&
        vad_endpoint_.ShouldStop(esp_timer_get_time())) {
        if (runtime_->stop_input(MonotonicMs()) != ur::device::Result::Applied) {
            return;
        }
        /* 句尾停麦后进入等待平台回复窗口：该窗口内"监听态但采集未开"是正常交接，
         * 自愈补发会打断本轮回复，故置等待闸门（起播/新轮/媒体重协商/超时清位）。 */
        awaiting_reply_.store(true);
        awaiting_reply_at_ms_.store(MonotonicMs());
        ESP_LOGI(kTag, "capture stop for reply wait (vad endpoint)");
    }
}

void SignalingProtocol::PrepareForOta() { PostCommand(Command::kEndSession); }

void SignalingProtocol::SendAbortSpeaking(AbortReason reason) {
    (void)reason;
    PostCommand(Command::kCancelResponse);
}

void SignalingProtocol::SendMcpMessage(const std::string& message) {
    (void)message;
    /* 信令模式下设备工具走服务端 MCP/action；不转发公共小智 MCP。 */
}

bool SignalingProtocol::SendTextQuestion(const std::string& text) { return SendText(text); }

/* SendPendingText 送出排队的视觉文字提问：半双工采集会阻塞文字输入，
 * 先停麦让位（平台 vision 流程同轮取帧后语音回复）。 */
void SignalingProtocol::SendPendingText(const char* trigger) {
#if CONFIG_UR_AI_SIGNALING_WEBRTC_VIDEO
    /* 语音会话无 text 输入模态：排队问题（早于会话建立）在此丢弃，
     * 避免established后每2秒Unsupported重试形成死循环。 */
    if (runtime_ != nullptr && !runtime_->voice().vision_session()) {
        if (!pending_text_.empty()) {
            ESP_LOGW(kTag, "pending text dropped, not a vision session (%s)", trigger);
            pending_text_.clear();
        }
        return;
    }
#endif
    runtime_->stop_input(MonotonicMs());
    const auto result = runtime_->send_text(pending_text_, false, MonotonicMs());
    ESP_LOGI(kTag, "text send via %s (%d bytes) result=%d", trigger,
             static_cast<int>(pending_text_.size()), static_cast<int>(result));
    if (result == ur::device::Result::Applied) {
        pending_text_.clear();
    }
}

bool SignalingProtocol::SendText(const std::string& text) {
    /* aiVision 会话 negotiated 模态含 text；文字问题触发平台同轮取摄像头帧
     * 交视觉模型。允许会话建立前排队（CLI 在会话期可能被主任务推迟），
     * established 边缘自动补发。 */
    if (text.empty()) {
        return false;
    }
#if CONFIG_UR_AI_SIGNALING_WEBRTC_VIDEO
    /* 视觉提问只属于 aiVision 三轨会话；语音会话无 text 输入模态，排队会在
     * established 后每 2 秒Unsupported重试形成死循环，入口直接拒绝。 */
    if (session_active_.load() && runtime_ != nullptr &&
        !runtime_->voice().vision_session()) {
        ESP_LOGW(kTag, "text question requires vision session (sig_vision first)");
        return false;
    }
#endif
    pending_text_ = text;
    return PostCommand(Command::kSendText);
}

void SignalingProtocol::HandlePlaybackEdges() {
    const bool playing = runtime_->voice().playing();
    if (playing && !playback_playing_) {
        playback_playing_ = true;
        /* 平台回复已起播：本轮"等待回复"窗口结束，释放自愈闸门（此后回复若非用户
         * 主动打断而是被媒体断连强停，下一轮沿 tts stop 会把应用置回监听态）。 */
        awaiting_reply_.store(false);
        EmitLegacyJson("tts", "start", "");
    } else if (!playing && playback_playing_) {
        playback_playing_ = false;
        ESP_LOGI(kTag, "reply playback stopped; capture self-heal re-armed");
        EmitLegacyJson("tts", "stop", "");
    }
}

/* HealListeningCapture 幂等自愈媒体恢复链后的采集缺口：判定"监听态且采集未开"用三个
 * 同时成立的事实——会话已建立（session_active_ + voice Established）、应用处于监听态
 * （Application::GetDeviceState 为原子状态，跨任务只读安全）、设备侧采集确实未开
 * （voice().capturing() 读 Session 实际驱动状态，不走意图标志）。协议层没有监听意图
 * 标志（SendStartListening 的模式参数曾被丢弃、VAD 端点只判句尾时刻），据此以应用状态
 * 为准最可靠且不误判。
 * 两道防误伤闸门：awaiting_reply_（本地 VAD 句尾停麦到平台回复起播的正常交接窗口，
 * 此时补发会打断本轮回复；等待超时视为回复丢失后放行）与播放未排空/正在播放
 * （半双工播放期不开麦，也避免截断回复尾音；与应用的延迟开麦同款闸门）。
 * 应用"表情保持"（ur_emotion_hold_）造成的延迟开麦不在闸门内：那只是展示观感延时，
 * 同样会因单次边沿丢失而永久卡住（正是本缺陷形态），提前开麦不影响应用随后的兑现。
 * 动作固定为 Application::EnsureListeningAudio()：应用主任务里重放自己的开麦路径
 * （audioStart + 语音处理）。只补 audioStart 不够——AfeAudioEngine 仅在语音处理开启时
 * 回调输出帧，信令采集队列否则恒为空，麦克风实际不出声。
 * 幂等安全：EnsureListeningAudio 内部在主任务复核监听态，重复触发不会重复上行；
 * 即便重复到达，VoiceClient::start_input 在已有采集意图/播放中时返回 Busy，不发布
 * 报文、不改状态机；2 秒最小间隔节流保证重复触发安全。全双工无需自愈（采集持续开），
 * 视觉会话由 SendStartListening 内部守卫拦截。 */
void SignalingProtocol::HealListeningCapture(std::uint64_t now) {
    if (!session_active_.load() || runtime_ == nullptr) {
        return;
    }
    if (awaiting_reply_.load()) {
        if (now - awaiting_reply_at_ms_.load() < kReplyWaitTimeoutMs) {
            return;
        }
        awaiting_reply_.store(false);
        ESP_LOGW(kTag, "capture self-heal: reply wait timeout, gate released");
    }
    if (runtime_->voice().state() != ur::device::VoiceState::Established) {
        return; /* 建立/重协商/结束中不补发：媒体未就绪时开麦必然失败或落到旧代次。 */
    }
    if (start_command_pending_.load()) {
        return; /* 开麦命令已排队但信令任务尚未处理：等它落地后再按实际采集状态判定，
                 * 否则会在 audioStart 受理前再补发一条重复命令（真机 result=0→5）。 */
    }
    auto& app = Application::GetInstance();
    if (app.GetDeviceState() != kDeviceStateListening) {
        return; /* 空闲/播报/配网等非监听态：用户本意不需要开麦（挂断后不复活）。 */
    }
    auto& audio = app.GetAudioService();
    if (runtime_->voice().capturing() || runtime_->voice().playing() || !audio.IsPlaybackIdle()) {
        return; /* 已在采集、正在播放或回复尾音未排空：不是缺口，补发会截断/抢麦。 */
    }
    if (now - capture_heal_at_ms_ < kCaptureHealIntervalMs) {
        return; /* 节流：留给上一次补发落地（audioStart 往返 + 采集开启）。 */
    }
    capture_heal_at_ms_ = now;
    ESP_LOGW(kTag, "capture self-heal: listening but capture off, resume app listening");
    /* 统一走应用的开麦路径（主任务执行）：媒体采集与语音处理（AFE 输出）一起恢复。
     * 只在此处补 audioStart 会让麦克风仍不出声——AfeAudioEngine 仅在语音处理开启时
     * 回调输出帧，信令采集队列因此始终为空。 */
    app.EnsureListeningAudio();
}

void SignalingProtocol::EmitLegacyJson(const char* type, const char* state,
                                       const std::string& value) {
    if (on_incoming_json_ == nullptr) {
        return;
    }
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", type);
    if (state != nullptr) {
        cJSON_AddStringToObject(root, "state", state);
    }
    if (strcmp(type, "stt") == 0 || (state != nullptr && strcmp(state, "sentence_start") == 0)) {
        cJSON_AddStringToObject(root, "text", value.c_str());
    } else if (strcmp(type, "llm") == 0) {
        cJSON_AddStringToObject(root, "emotion", value.c_str());
    }
    on_incoming_json_(root);
    cJSON_Delete(root);
}

void SignalingProtocol::HandleContent(const ur::device::ContentEvent& content) {
    const std::string& method = content.envelope.method;
    if (method == "respSttDone") {
        EmitLegacyJson("stt", nullptr, content.text);
    } else if (method == "respTextDone") {
        if (!content.text.empty()) {
            EmitLegacyJson("tts", "sentence_start", content.text);
        }
    } else if (method == "respEmotion") {
        EmitLegacyJson("llm", nullptr, content.emoji);
    }
    if (runtime_ != nullptr) {
        runtime_->observe_content_display(content, true, true);
    }
}
