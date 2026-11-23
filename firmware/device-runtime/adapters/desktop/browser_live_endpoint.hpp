// 文件说明：同一设备连接的单节目接收宿主；只持一个运行核心，退役保存紧凑原事实，不重建旧媒体。
#pragma once
#include "browser_live_focus.hpp"
#include "browser_live_media.hpp"
#include "ur/device/live_runtime_port.hpp"
namespace ur::device {
// BrowserLiveEndpoint 原邀请、控制和媒体事件同线程交接；最多32个原会话/每会话32控制及128KiB预算。
// 字符串capacity与vector元数据计入预算，媒体端口/第三方Peer独立预算，不声称覆盖浏览器堆。
class BrowserLiveEndpoint final : public LiveRuntimePort {
public:
    // factory/readiness来自宿主真实初始化；deadline为独立有限授权，不从邀请或网页续期。
    BrowserLiveEndpoint(PublishPort& mqtt, BrowserLiveFactory& factory,
                        BrowserLiveReadiness readiness, std::string up, std::string down,
                        std::uint64_t deadline);
    ~BrowserLiveEndpoint() override;
    bool matches(PublishPort& mqtt, std::string_view up, std::string_view down) const override;
    bool begin_connection(std::uint64_t now) override;
    bool owns(std::string_view session) const override;
    bool receive(std::string_view topic, std::string_view body, std::uint64_t now,
                 std::uint64_t wall) override;
    void tick(std::uint64_t now, std::uint64_t wall) override;
    void close(std::uint64_t now) override;
    // begin_parent_close
    // 停原媒体/新准入，但等待平台childEnded及原actual事实，不毁同连接cleanup资格。
    void begin_parent_close(std::uint64_t now) override;
    bool released() const override;
    bool settled() const override;
    // receive_local 核当前原instance的驱动事实；网页不能自报平台会话或授予输出。
    bool receive_local(std::string_view body);
    // active_media 仅供宿主有界泵本地命令；对象生命周期由本端点所有。
    BrowserLiveMediaPort* active_media();
    // scope 提供原核心会话/真实授权/Established交集，不从历史actual或网页字段取得。
    std::optional<BrowserLiveFocusScope> scope() const;
    // release_scope 精确查同连接、历史原父、平台已终态且实际Released的原会话；不返回播放许可。
    std::optional<BrowserLiveFocusScope> release_scope(std::string_view session) const;
    // rearm_lane 返回待唯一输出仲裁器重新确认的原组，不允许调用方擅自绕过Path票据。
    std::optional<std::string_view> rearm_lane() const;
    // rearmed 仅在唯一仲裁器重新收录该原组actualfalse后调用，不能由网页或构造推定。
    bool rearmed(std::string_view lane);

private:
    // Control 保存紧凑原规范化控制及确定回执，不保存驱动或可运行授权。
    struct Control {
        std::string token, original, reply;  // 原token/全字段字节/原实际或未知结果。
        bool delivered = false;              // 本地QoS发布受理，不等于后端持久收录。
    };
    // Record 保存原会话事实，Ended后仅重放原trackState，不重发accept或重建Peer。
    struct Record {
        std::string session, token, invitation, decision;  // 原五键邀请与准入请求事实。
        std::vector<Control> controls;                     // 最多32，无live引擎/重驱动依赖。
        bool decision_delivered = false;                   // 原发布背压必须保留，不虚称已交付。
        std::string parent{};  // 原C++不可变父绑定，不能由同主人新AI会话替代。
        std::uint64_t connection = 0, retain_until = 0;          // 原连接序与固定十分钟关闭资格。
        bool audio = false, video = false, established = false;  // 原完整轨/曾建立事实。
        bool platform_ended = false, actual_released = false;    // localclose不设平台终态。
    };
    // CapturePublisher 仍只转交原唯一网络驱动，缓存已形成原回执而不是将publish当实际成功。
    class CapturePublisher final : public PublishPort {
    public:
        explicit CapturePublisher(BrowserLiveEndpoint& owner) : owner_(owner) {}
        bool publish(std::string_view topic, std::string_view body) override;

    private:
        BrowserLiveEndpoint& owner_;  // 生命周期长于唯一原Client。
    };
    bool captured(std::string_view topic,
                  std::string_view body);  // 原结果关联/有界缓存并转交发布。
    bool observe(std::uint64_t now);       // 宿主高水位/有限期限，倒退不清历史。
    bool replay(const Record& record, const Envelope& message, std::string_view canonical);
    std::size_t memory() const;  // 核字符串容量和有界容器元数据，不包含第三方媒体堆。
    void retire();               // 原终态/actual退出/原事实已交付才紧凑退役。
    std::optional<BrowserLiveFocusScope> cleanup(const Record& record, bool actual) const;
    // cleanup 原紧凑终态关闭证明，不把源授权失效或历史actual提升成新输出许可。
    PublishPort& mqtt_;
    BrowserLiveFactory& factory_;     // 原连接和唯一预约工厂，寿命更长。
    BrowserLiveReadiness readiness_;  // 原真实能力快照，不因新Invite扩展。
    std::string up_, down_;
    std::uint64_t deadline_;                // 原鉴权路由及宿主有限截止。
    CapturePublisher publisher_;            // 生命周期长于借用它的Client。
    std::unique_ptr<LiveMediaPort> media_;  // 先构造，后于Client销毁，必须actualStopped才复用。
    std::unique_ptr<LiveClient> client_;    // 最多一个运行核心，不按历史复制重型依赖。
    std::optional<Record> current_;
    std::vector<Record> retired_;         // 最多32会话，满额安全拒绝。
    std::optional<std::uint64_t> clock_;  // 跨本宿主连接保持单调，不允许换token重置。
    std::optional<std::string> rearm_;    // 新基础node actualfalse尚未重新确认的原完整组。
    std::uint64_t connection_ = 0;  // 原MQTT连接生命周期序，旧连接墓碑不能越过新连接释放权限。
    bool parent_closing_ = false;   // 同连接只收尾原终态/关闭证明，新节目须合法新连接后才重新准入。
    bool accepting_ = false, terminal_ = false,
         connected_reported_ = false;  // 逻辑与actual分别维护。
};
}  // namespace ur::device
