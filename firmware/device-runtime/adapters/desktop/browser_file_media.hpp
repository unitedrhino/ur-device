// 桌面文件播放器驱动；绑定由内容服务授权的单实例源，本地浏览器仅执行媒体而不持有设备身份。
#pragma once
#include <functional>
#include "ur/device/output_group_gate.hpp"
#include "ur/device/playback_engine.hpp"

namespace ur::device {
// BrowserFileBinding 来自可信内容适配，不接受模型或MQTT自由URL；只绑定一个子会话/文件实例。
struct BrowserFileBinding {
    std::string session;      // 独立mediaPlayback子会话。
    std::string playback_id;  // 单个播放器实例。
    std::string content_ref;  // 云端授权opaque引用，load时必须完全匹配。
    std::string url;          // 短期授权HTTPS下载地址，或测试桥固定媒体资产路径；不得记录日志。
    std::string origin;       // 可信HTTPS源域；空仅允许本地固定媒体资产，不推断任意同源路径。
    std::string mime;         // audio/wav、audio/mpeg、video/mp4或video/webm，仍需实际解码验证。
    bool audio = true;        // 视频是否含获准输出的音轨；false时必须静音，不能隐含获得speaker。
    std::uint64_t expires_at = 0;        // 授权内容单调时钟截止，不能由浏览器延长。
    std::uint32_t max_bytes = 16777216;  // 实际下载上限，1至64MiB；不是仅信任Content-Length。
};
// valid_browser_file_binding 统一校验可信绑定的身份、固定来源及资源边界，不分配浏览器资源。
bool valid_browser_file_binding(const BrowserFileBinding& binding);
// BrowserFilePermission 检查当前session及完整speaker/display组租约；必须由宿主焦点管理器实现。
// 回调在设备事件线程执行，参数为固定实例是否使用audio/video及当前单调时间。
using BrowserFilePermission = std::function<bool(bool audio, bool video, std::uint64_t now)>;
// BrowserFileMedia 由设备事件线程访问；浏览器事实严格关联session/实例/代次，不直接改变核心。
class BrowserFileMedia final : public PlaybackDriver, public OutputGroupGatePort {
public:
    // binding和permission均来自可信宿主；空回调、非法源或资源声明拒绝创建工作命令。
    BrowserFileMedia(BrowserFileBinding binding, BrowserFilePermission permission);
    ~BrowserFileMedia();
    // tick 必须先于submit/poll；到期、时钟倒退或不可恢复队列故障时关闭本播放器。
    bool tick(std::uint64_t now);
    bool begin(const PlaybackCommand& command, std::uint64_t generation) override;
    std::optional<PlaybackFact> take_fact() override;
    bool output_permitted() const override;
    // preparation_permitted 只检查原内容/整组资源许可，不把浏览器未就绪等价为许可被拒。
    bool preparation_permitted() const;
    // stop_all 只请求本播放器关闭；直到浏览器实际释放回执才返回true，不假报同步停止。
    bool stop_all() override;
    // receive 只接本地已鉴权桥的严格事实；false须令宿主关闭本实例，不输出原文。
    bool receive(std::string_view body);
    // take_command 取得一条有界本地命令；JSON可能含授权URL，不可写模型、UI文本或日志。
    std::optional<std::string> take_command();
    bool ready() const { return ready_ && !closing_ && !failed_; }  // 实际桌面媒体初始化完成。
    bool quiescent() const { return released_; }  // 已收到实际释放，不能从断连或命令排队推断。
    // binding 供可信宿主核对原实例/完整输出及截止，不直接暴露给网页或日志。
    const BrowserFileBinding& binding() const { return binding_; }
    // 独立门控只限制本文件路径的声音/画面，不能取得业务许可或停止AI采集。
    std::optional<std::uint64_t> request_output_gate(bool enabled, std::uint64_t now) override;
    bool output_gate_confirmed(std::uint64_t ticket, bool enabled) const override;

private:
    friend class BrowserFileStaticFaultTest;  // 仅测试缺失时钟前置，不改变生产接口。
    bool enqueue(std::string body);  // 命令编码或背压失败时不可继续接纳媒体工作。
    std::string command(const char* kind, std::uint64_t generation,
                        const PlaybackCommand* playback = nullptr) const;  // 不转发自由JSON。
    BrowserFileBinding binding_;           // 固定授权源，生命周期不跨实例复用。
    BrowserFilePermission permission_;     // 明确资源组许可，不默认true。
    FixedQueue<std::string, 8> commands_;  // 控制队列；关闭可取消尚未交付的播放命令。
    FixedQueue<PlaybackFact, 8> facts_;    // 实际完成队列，不能无限积压浏览器事实。
    std::optional<std::uint64_t> clock_;   // 同一宿主单调时钟。
    std::uint64_t generation_ = 0;         // 已接纳的内部媒体代次，本地桥以十进制字符串表示。
    std::string operation_;                // 当前实际等待完成的操作。
    bool video_ = false;                   // 来自固定MIME，不从响应正文提升能力。
    bool ready_ = false;                   // 浏览器实际初始化完成。
    bool loaded_ = false;                  // 当前固定内容实际加载过，不凭命令发出推断。
    bool pending_ = false;                 // 唯一已接纳驱动操作。
    bool completion_queued_ = false;       // 原操作完成已排队，重复事实不能在消费前重复完成。
    bool closing_ = false;                 // 不接新操作，仍等待实际关闭回执。
    bool released_ = false;                // 浏览器实际关闭并清理资源的事实。
    bool failed_ = false;                  // 配置、时间或队列失效，不可在原实例复活。
    std::uint64_t gate_ticket_ = 0, gate_confirmed_ = 0;  // 独立输出票据，与操作代次分开。
    std::uint64_t gate_deadline_ = 0;                     // 原请求固定两秒，重复/旧回执不能续期。
    bool gate_enabled_ = true;                       // 兼容既有路径，只增加限制，不替代原业务许可。
    bool gate_pending_ = false, gate_sent_ = false;  // 排队与实际交付分开，未交付不能确认。
};
}  // namespace ur::device
