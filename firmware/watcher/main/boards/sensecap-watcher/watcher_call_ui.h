/* Watcher 设备端语音/视频通话界面：微信式来电/通话页面。
 * 本文件只描述页面与线程契约：信令/解码任务回调仅投递快照，全部 LVGL 操作
 * 收敛到主任务（Application::Schedule + 显示锁）或 LVGL 任务（页面定时器）。
 * 屏幕与控件随首次来电惰性创建，进程生命周期内复用。 */
#pragma once

#include "sdkconfig.h"

#if CONFIG_UR_AI_SIGNALING_WEBRTC

#include <lvgl.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>

#include "display/display.h"
#include "lvgl_image.h"
#include "ur_signaling_call.h"

namespace watcher {

/* WatcherCallUi 圆屏 412x412 通话页面：来电（接听/拒接）、接通中、通话中
 * （全屏对方视频 + 底部挂断/麦克风/摄像头控制钮）与通话结束短暂提示。 */
class WatcherCallUi {
public:
    /* Instance 返回进程级单例；未安装显示前各入口均为安全空操作。 */
    static WatcherCallUi& Instance();

    /* Install 绑定显示出口并注册 UrSignalingCall UiHooks：
     * - keep_awake 在显示任务上下文回调，用于通话期间阻止息屏（内部自行转主任务）；
     * - set_active 由板级显示实现，true 冻结菜单渲染并让出屏幕，false 归位主页。
     * 须在显示初始化完成后、信令任务启动前调用一次。 */
    static void Install(Display* display, std::function<void()> keep_awake,
                        std::function<void(bool)> set_active);

    /* OnKnobClick 旋轮单击联动：来电时接听、其余通话状态挂断；
     * 仅在显示任务上下文调用。 */
    void OnKnobClick();

    // TraceState 仅供本地sig_perf读取可见性和屏幕归属，不改变控件或采集媒体内容。
    static void TraceState();

    // RefreshTheme 在已有显示锁内更新惰性通话页，换肤不重建控件或媒体对象。
    static void RefreshTheme();

private:
    WatcherCallUi() = default;
    ~WatcherCallUi() = default;
    WatcherCallUi(const WatcherCallUi&) = delete;
    WatcherCallUi& operator=(const WatcherCallUi&) = delete;

    /* HandleState/HandleTrackState/HandleRemoteFrame 由信令或解码任务调用，
     * 只做拷贝与转主任务，绝不直接触碰 LVGL。 */
    // HandleCallKind 在状态之前把已校验的邀请类型转交显示任务。
    void HandleCallKind(bool voice);
    void HandleState(UrSignalingCall::UiState state);
    void HandleTrackState(bool mic_paused, bool camera_paused);
    void HandleRemoteFrame(const std::uint8_t* rgb565, int width, int height);

    /* ApplyState/ApplyTrackState/DrainFrame 在主任务执行并持有显示锁。 */
    // ApplyCallKind 按语音/视频设置来电文案和实际可用控制钮，不启动媒体。
    void ApplyCallKind(bool voice);
    void ApplyState(UrSignalingCall::UiState state);
    void ApplyTrackState(bool mic_paused, bool camera_paused);
    void DrainFrame();

    /* BuildLocked 惰性创建通话屏幕；主题字体缺失时返回 false 由下次再试。 */
    bool BuildLocked();

    /* ShowLocked 加载通话屏幕并通知板级冻结菜单；HideLocked 收回通话屏幕、
     * 归位主页并释放视频帧；两者均要求显示任务上下文或已持有显示锁。 */
    void ShowLocked();
    void HideLocked();

    /* OnEndedTimeout 通话结束后 1.5 秒自动收回页面；LVGL 定时器上下文。 */
    static void OnEndedTimeout(lv_timer_t* timer);

    /* TouchKeepAwake 限频唤醒息屏计时，避免每帧都转主任务。 */
    void TouchKeepAwake();

    Display* display_ = nullptr;            // 显示出口，仅用于取主题与显示锁。
    std::function<void()> keep_awake_;      // 通话期间阻止息屏的板级回调。
    std::function<void(bool)> set_active_;  // 板级菜单让出/归位门禁。
    lv_obj_t* screen_ = nullptr;            // 通话页根屏幕，惰性创建后长期复用。
    lv_obj_t* remote_image_ = nullptr;      // 全屏对方视频，等比缩放铺满圆屏。
    lv_obj_t* status_label_ = nullptr;      // 接通中/等待视频/通话已结束提示。
    lv_obj_t* ring_title_ = nullptr;        // 来电类型标题，首个邀请前按已投递快照建立。
    lv_obj_t* ring_box_ = nullptr;          // 来电容器：标题＋接听/拒接钮与文字。
    lv_obj_t* accept_btn_ = nullptr;        // 来电绿色接听圆钮。
    lv_obj_t* accept_hint_ = nullptr;      // 来电操作说明随皮肤保持可读。
    lv_obj_t* reject_hint_ = nullptr;      // 拒接说明与主题字体同步。
    lv_obj_t* reject_btn_ = nullptr;        // 来电红色拒接圆钮。
    lv_obj_t* accept_icon_ = nullptr;       // 接听钮电话图标。
    lv_obj_t* reject_icon_ = nullptr;       // 拒接钮电话图标（旋转 135 度示挂断）。
    lv_obj_t* control_box_ = nullptr;       // 通话控制容器：挂断/麦克风/摄像头。
    lv_obj_t* hangup_btn_ = nullptr;        // 通话中红色挂断圆钮。
    lv_obj_t* hangup_icon_ = nullptr;       // 挂断钮电话图标（旋转 135 度）。
    lv_obj_t* mic_btn_ = nullptr;           // 麦克风开关圆钮。
    lv_obj_t* mic_icon_ = nullptr;          // 麦克风图标（MIC/MIC_OFF）。
    lv_obj_t* camera_btn_ = nullptr;        // 摄像头开关圆钮。
    lv_obj_t* camera_icon_ = nullptr;       // 摄像头图标（PHOTO_CAMERA）。
    lv_timer_t* ended_timer_ = nullptr;     // 通话结束 1.5 秒自动收回定时器。
    bool voice_call_ = false;               // 显示任务持有的邀请类型，语音不展示摄像头控制。
    bool shown_ = false;                    // 通话屏幕当前是否占用显示。
    bool mic_paused_ = false;               // 最近一次轨开关快照（显示任务持有）。
    bool camera_paused_ = false;            // 同上。
    int frame_width_ = 0;                   // 当前视频帧分辨率，变化时重算缩放。
    int frame_height_ = 0;
    uint32_t last_awake_ms_ = 0;                         // 上次唤醒息屏计时的毫秒时刻。
    std::unique_ptr<LvglAllocatedImage> current_frame_;  // 正在显示的帧，替换时回收到备用槽。
    std::mutex frame_mutex_;                             // 只保护待显示帧槽的交换。
    std::unique_ptr<LvglAllocatedImage> pending_frame_;  // 最新待上屏帧，旧帧被覆盖释放。
    std::unique_ptr<LvglAllocatedImage> spare_frame_;    // 复用的第三个缓冲，避免每帧申请PSRAM。
    std::uint64_t frame_generation_ = 0;                 // 停流或结束后作废在途拷贝；由帧锁保护。
    std::uint64_t pending_frame_generation_ = 0;         // 待显示帧所属代次；由帧锁保护。
    std::uint64_t displayed_frame_generation_ = 0;       // 当前上屏帧代次；由显示锁保护。
    bool frame_accepting_ = false;                       // 信令状态允许接收帧；由帧锁保护。
    std::atomic<bool> frame_scheduled_{false};           // 是否已有一次上屏转主任务在途。
    std::uint32_t displayed_frames_ = 0;                 // 当前通话窗口已交给LVGL的帧数。
    std::uint32_t dropped_frames_ = 0;                   // 待上屏槽已有最新帧时丢弃的重复拷贝数。
    std::int64_t display_rate_started_us_ = 0;           // 当前显示帧率统计窗口起点。
    std::atomic<int> ui_state_{static_cast<int>(UrSignalingCall::UiState::Hidden)};  // 页面粗状态。
};

}  // namespace watcher

#endif /* CONFIG_UR_AI_SIGNALING_WEBRTC */
