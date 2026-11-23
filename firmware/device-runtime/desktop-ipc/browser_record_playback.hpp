// 文件说明：独立桌面档案源端口；可信固定目录归C++，网页只执行本地档案解码和指定Peer。
#pragma once
#include <functional>
#include <map>
#include "ur/device/record_playback_client.hpp"

namespace ur::device {
// BrowserRecordAsset 是部署者固定清单中的单份档案，不从平台邀请解释任意路径或URL。
struct BrowserRecordAsset {
    std::string camera_id, recording_id, recording_version, content_ref;  // 同一不可变档案四元组。
    std::string route;              // 本地桥固定白名单/record-assets/路径，不是MQTT来源地址。
    std::uint64_t duration_ms = 0;  // 清单实际整份时长，load仍核验真实解码时长。
    std::uint64_t max_bytes = 0;    // 流式读取上限1～64MiB，不由邀请扩大。
    bool audio = false;             // 明确配置MP4内Opus音轨，不授设备麦克风或扬声器。
};
// BrowserRecordEvent 只交当前原Peer的真实offer/ICE失败事实，不能作为平台Established。
struct BrowserRecordEvent {
    enum class Kind { Offer, Connected, Failed };  // 不把SDP与连通／失败混用。
    Kind kind = Kind::Failed;                      // 默认不代表媒体成功。
    std::uint64_t media_revision = 0;              // 原Peer身份，不能以新的回调替代旧m。
    std::string sdp;                               // 仅Offer有完整SDP，不记录到公开日志。
};
// BrowserRecordPermission 是宿主原档案本地有限许可；默认缺失时禁止准备和发布。
using BrowserRecordPermission = std::function<bool(std::uint64_t)>;
// BrowserRecordPlaybackPort 每实例最多当前＋一个退出Peer，命令/事实队列有界且不复用实时采集。
class BrowserRecordPlaybackPort final : public RecordPlaybackPort {
public:
    BrowserRecordPlaybackPort(BrowserRecordAsset asset, BrowserRecordPermission permission);
    ~BrowserRecordPlaybackPort() override;
    // tick 交付单调时钟；过期／倒退切断源，不能由页面时钟续期。
    void tick(std::uint64_t now);
    // ready 只有已配置清单和真实页面格式探针通过才允许宿主声明历史能力。
    bool ready() const;
    RecordPreparation prepare(const RecordPlaybackInvite& binding, std::uint64_t deadline) override;
    bool begin(const PlaybackCommand& command, std::uint64_t generation,
               std::uint64_t next_revision) override;
    std::optional<RecordPlaybackFact> take_fact() override;
    bool begin_peer(std::uint64_t media_revision) override;
    bool apply_answer(std::uint64_t media_revision, std::string_view sdp) override;
    bool connected(std::uint64_t media_revision) const override;
    void close_peer(std::uint64_t media_revision) override;
    bool peer_stopped(std::uint64_t media_revision) const override;
    bool restore_intent(std::uint64_t media_revision, std::string_view state,
                        std::uint64_t generation) override;
    bool publishing_permitted(std::uint64_t media_revision) const override;
    bool stop_all() override;
    // receive 仅处理严格本地桥白名单，失败不输出原body、来源URL或浏览器异常。
    bool receive(std::string_view body);
    // take_command/take_event 由同一宿主交接；排队接纳不是浏览器执行或平台许可。
    std::optional<std::string> take_command();
    std::optional<BrowserRecordEvent> take_event();
    bool failed() const { return failed_; }  // 队列/协议故障后安全关闭，不降级采集。

private:
    // Peer 保留实际Stopped直到旧代次退役；当前与退出Peer不共享身份。
    struct Peer {
        bool answered = false, connected = false;  // 完整answer受理与真实连通分开。
        bool closing = false, stopped = false;     // 关闭请求与真实全回调退出分开。
    };
    bool enqueue(std::string body);  // 上限32，溢出关闭，不能丢配置仍执行play。
    std::string command(const char* kind, std::uint64_t media_revision = 0,
                        const PlaybackCommand* control = nullptr,
                        std::string_view value = {}) const;
    bool valid() const;                   // 检查本地时钟/原许可/独立授权期限，不接页面自报权限。
    BrowserRecordAsset asset_;            // 固定可信目录元数据，无外部可写来源。
    BrowserRecordPermission permission_;  // 宿主有限授权检查，不由浏览器代劳。
    std::optional<RecordPlaybackInvite> binding_;  // 只绑定一次原历史会话。
    std::string operation_;  // 仅原待处理操作；stop可中断seek，但不能替代其他未交事实。
    std::string last_fact_;  // 最近原实际事实规范字节，重复网页回调不再次推进核心水位。
    FixedQueue<std::string, 32> commands_;      // 有界本地执行意图。
    FixedQueue<RecordPlaybackFact, 4> facts_;   // 原操作实际事实不可无限积压。
    FixedQueue<BrowserRecordEvent, 4> events_;  // 完整SDP及真实连通事实单独交接。
    std::map<std::uint64_t, Peer> peers_;       // 至多当前与退出对象，旧实际停止后移除。
    std::optional<std::uint64_t> clock_;        // 宿主单调时钟，不读取JS wall clock。
    std::uint64_t deadline_ = 0, generation_ = 0, current_revision_ = 1;  // 分开的有限关联。
    bool configured_ = false, format_ready_ = false, failed_ = false;  // 清单和实际格式均须成立。
    bool loaded_ = false, pending_ = false, fact_queued_ = false;      // 实际load和原操作队列阶段。
    bool bound_sent_ = false;  // 原Bind已交宿主后才可能存在网页资源，排队不等于交付。
    bool closing_ = false, released_ = false;  // 全组关闭与真实退出分开。
};
}  // namespace ur::device
