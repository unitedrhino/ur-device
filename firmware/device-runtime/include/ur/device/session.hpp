// 设备语音状态核心：输入与播放独立；由单一事件线程调用，无平台头文件。
#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace ur::device {
// Mode 为交互方式；Pipeline 为云端处理方式，两者不能互相替代。
enum class Mode { HalfDuplex, FullDuplex };
enum class Pipeline { Cascade, Realtime };
// Result 区分实际应用、幂等、陈旧事件及驱动故障，供信令层映射错误。
enum class Result { Applied, Duplicate, Invalid, Unsupported, Stale, Busy, DriverFailed };
// Capabilities 是适配器实际能力，不因平台选择就自动获得 AEC。
struct Capabilities {
    bool half_duplex = true;   // 是否支持逐轮收发。
    bool full_duplex = false;  // 是否允许采集与播放并发。
    bool device_aec = false;   // 当前采集路径是否提供有效 AEC。
};
// AudioPort
// 是同步接受操作的边界，驱动必须非阻塞；异步失败转交事件线程关闭会话。 stop_*
// 必须幂等且清理已排队资源；驱动不得同步重入 Session。
class AudioPort {
public:
    virtual ~AudioPort() = default;
    // start_capture 接受指定输入段的采集，false 表示尚未启动。
    virtual bool start_capture(std::string_view input_id) = 0;
    // stop_capture 停止采集与上行，不能停止播放。
    virtual void stop_capture() = 0;
    // start_playback 接受回复；驱动按 epoch 隔离迟到的本地播放回调。
    virtual bool start_playback(std::string_view response_id, std::uint64_t epoch) = 0;
    // stop_playback 丢弃指定代次的排队音频，不停止采集。
    virtual void stop_playback(std::uint64_t epoch) = 0;
};
// Session 是一个已协商语音会话的控制器；线协议解析及认证由调用层负责。
class Session {
public:
    // port 必须比 Session 活得更久；析构时停止本对象拥有的媒体。
    explicit Session(AudioPort& port) : port_(port) {}
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    // open 只接受平台已选择且设备支持的组合；失败不改变现有状态。
    Result open(std::string session_id, Mode mode, Pipeline pipeline, Capabilities capabilities);
    // start_input/stop_input 只操作输入；重复 start 不重复调用驱动。
    Result start_input(std::string_view session_id, std::string input_id);
    Result stop_input(std::string_view session_id, std::string_view input_id);
    // suspend_media只供已鉴权会话的媒体恢复流程调用；立即停两向驱动，保留原输入及回复墓碑。
    // 网络暂停不等于业务audioStop，也不能在恢复时重播旧回复。
    Result suspend_media(std::string_view session_id);
    // resume_media仅在新Peer实际连通且平台connected ACK后调用；只恢复未被主动暂停的原输入段。
    // 驱动拒绝时保持暂停状态，由宿主失败关闭；不创建新输入或重置回复代次。
    Result resume_media(std::string_view session_id);
    // begin_response 接收经路由校验的 respAudioStart，sequence 必须是正事件序号。
    Result begin_response(std::string_view session_id, std::string response_id,
                          std::uint64_t sequence);
    // network_done 只标记云端发送结束，不代表本地播放完成。
    Result network_done(std::string_view session_id, std::string_view response_id);
    // playback_drained 由真实播放队列排空回调触发，epoch 不符则丢弃。
    Result playback_drained(std::uint64_t epoch);
    // cancel_response 精确定向取消，保留全双工输入；不撤销工具动作。
    Result cancel_response(std::string_view session_id, std::string_view response_id);
    // close 同时停止两向资源，断网和驱动失败使用同一路径；重复关闭无副作用。
    void close();
    // 以下查询供适配器和测试观察实际本地状态，不代表云端 ACK。
    bool active() const { return active_; }
    bool capturing() const { return capturing_; }
    bool playing() const { return playing_; }
    std::uint64_t playback_epoch() const { return epoch_; }

private:
    // matches 校验会话隔离，结束后旧事件不能重新激活媒体。
    bool matches(std::string_view id) const;
    AudioPort& port_;               // 借用平台驱动，不拥有其生命周期。
    std::string session_id_;        // 平台确认的当前会话。
    std::string input_id_;          // 最近输入段，暂停后禁止同 ID 复活。
    std::string response_id_;       // 最近回复，取消后保留用于去重。
    Mode mode_ = Mode::HalfDuplex;  // 当前协商模式。
    bool active_ = false;           // 是否持有有效会话。
    bool capturing_ = false;        // 本地采集实际已接受启动。
    bool playing_ = false;          // 本地播放队列尚未排空或取消。
    // OutputRecoveryFlags共享原网络结束标记的字节，不增大嵌入式Session及其持有者。
    struct OutputRecoveryFlags {
        bool network_done : 1;     // 网络结束与设备播放结束分别建模。
        bool media_suspended : 1;  // 媒体屏障不是业务输入暂停或会话结束。
        bool resume_capture : 1;   // 主动audioStop撤销断流前采集意图，ACK不得恢复它。
    } output_{};
    std::uint64_t sequence_ = 0;    // 最近已处理的回复开始序号，阻止旧回复复活。
    std::uint64_t epoch_ = 0;       // 跨会话递增的本地播放代次，不随 close 清零。
};
}  // namespace ur::device
