// 文件说明：实时节目浏览器接收适配；C++持原业务绑定，网页仅提供回环网络/解码/输出实际事实。
#pragma once
#include <functional>
#include "browser_live_lane.hpp"
#include "ur/device/live_client.hpp"

namespace ur::device {
// BrowserLiveScope 查询原C++已建立AI父业务；空值或换父会话不能继续原节目授权。
using BrowserLiveScope = std::function<std::optional<std::string>()>;
// BrowserLiveReadiness 必须来自宿主实际格式/驱动预检，不按库存在、URL或模型参数广告能力。
struct BrowserLiveReadiness {
    bool audio = false;  // 实际Opus/48k/mono/20ms解码及默认关闭末端准备。
    bool video = false;  // 实际H264新IDR接收解码及可见呈现驱动准备。
};
// BrowserLiveMediaPort 固定一个原节目/完整组；不持设备密钥，不采集，不接自由源地址。
class BrowserLiveMediaPort final : public LiveMediaPort {
public:
    // instance是宿主本地唯一ID，不是云端session/身份；lease为独立有限授权期限。
    BrowserLiveMediaPort(LiveInvite invite, BrowserLiveLane& lane, BrowserLiveReadiness readiness,
                         BrowserLiveScope parent, std::string parent_id, std::uint64_t lease,
                         std::uint64_t instance, std::uint64_t now,
                         std::shared_ptr<bool> reservation = {});
    ~BrowserLiveMediaPort() override;
    LivePreparation prepare(const LiveInvite& invite, std::uint64_t deadline) override;
    bool begin_peer() override;
    bool apply_answer(std::string_view sdp) override;
    bool connected() const override;
    bool authorization_valid() const override;
    bool begin_track(const LiveTrackControl& command, std::uint64_t generation) override;
    std::optional<LiveMediaFact> take_fact() override;
    bool presentation_permitted(std::string_view track_id) const override;
    bool stop_all() override;
    // poll 原单调时钟由唯一宿主推进；回退/到期不重基线化或恢复旧Peer。
    bool poll(std::uint64_t now);
    // receive 严格核本地instance/m/原操作；网页无权改变平台会话或焦点租约。
    bool receive(std::string_view body);
    // take_command 交付本地固定命令，不发送MQTT，也不允许其他媒体handler消费它。
    std::optional<std::string> take_command();
    // take_offer 返回当前真实完整ICE offer，仅能消费一次，不能拿旧SDP建新Peer。
    std::optional<std::string> take_offer();
    // binding/lane 仅供宿主核原C++业务与完整资源组，不是网页自报授权。
    const LiveInvite& binding() const { return binding_; }
    // parent_session 只读原C++建立父绑定；不交网页，后续关闭证明不得借另一父业务。
    const std::string& parent_session() const { return parent_id_; }
    BrowserLiveLane& lane() const { return lane_; }

private:
    // enqueue 只接有限内部JSON；容量满拒绝，关闭命令使用独立保留槽。
    bool enqueue(std::string body);
    LiveInvite binding_;                                // 已严格解析的原邀请，整个实例不可改变。
    BrowserLiveLane& lane_;                             // 原完整输出组，寿命长于所有实际接收器。
    BrowserLiveReadiness readiness_;                    // 原实际预检，不读取网络自报能力。
    BrowserLiveScope parent_;                           // 原C++父业务查询，不信浏览器session字段。
    std::string parent_id_, instance_;                  // 原父业务与本地不含设备身份的唯一实例。
    std::uint64_t lease_, now_;                         // 原有限期限与单调高水位，不被控制续期。
    FixedQueue<std::string, 8> commands_;               // 当前有限本地意图，不无限累积SDP。
    FixedQueue<LiveMediaFact, 4> facts_;                // 原实际回执最多四条，不覆盖旧结果。
    std::optional<std::string> offer_, close_command_;  // 原完整offer与独立关闭保留槽。
    std::shared_ptr<bool> reservation_;                 // 仅实际Stopped后释放工厂唯一接收器预约。
    // Operation 固定原实际true票据，晚交历史完成不能借恢复后的新票据重开旧意图。
    struct Operation {
        LiveTrackControl command;                  // 原不可变控制，不接网页自行指定的新轨。
        std::uint64_t generation;                  // 原本地操作唯一水位。
        std::optional<std::uint64_t> gate_ticket;  // resume受理时已actual true的原票据。
    };
    std::array<std::optional<Operation>, 2> pending_{};  // 两轨独立在途，不相互暗停。
    bool prepared_ = false, peer_ = false, answer_ = false, connected_ = false;
    bool offer_received_ = false;  // 领取offer后仍隔离第二份旧SDP或重新协商意图。
    bool failed_ = false, closing_ = false, stopped_ = false;  // 意图、故障与实际退出分开。
};
// BrowserLiveFactory 只在原实际输出基线和预检成立后创建独立接收器，不创建第二设备身份。
class BrowserLiveFactory final : public LiveMediaFactory {
public:
    // lanes按audio/video/audioVideo固定三组，parent必须读原核心，不得由页面填身份。
    BrowserLiveFactory(std::array<BrowserLiveLane*, 3> lanes, BrowserLiveReadiness readiness,
                       BrowserLiveScope parent, std::uint64_t deadline);
    // create 固定当前原父业务/期限，端口仍需原sessionAccept/协商才能分配Peer。
    std::unique_ptr<LiveMediaPort> create(const LiveInvite& invite) override;
    // clock 由唯一宿主同步，不获取系统时钟，不允许用邀请重置高水位。
    bool clock(std::uint64_t now);

private:
    std::array<BrowserLiveLane*, 3> lanes_;  // 原可信物理组，禁止自由路径。
    BrowserLiveReadiness readiness_;         // 真实预检固定快照，不按invite扩能力。
    BrowserLiveScope parent_;                // 原当前AI父业务。
    std::uint64_t deadline_, serial_ = 0;    // 原授权截止和有界本地实例水位。
    std::optional<std::uint64_t> now_;       // 回退永久拒绝新的接收器。
    bool failed_ = false;                    // 无配置/回退不能通过新的邀请恢复。
    std::shared_ptr<bool> reservation_ = std::make_shared<bool>(false);  // 原事件线程唯一预约。
};
}  // namespace ur::device
