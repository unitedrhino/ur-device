// 新信令回复边界的数据报适配：16字节包头不变，使用已授权的下行 nonce 区分回复。
#pragma once
#include <array>
#include <optional>
#include <limits>
#include "ur/device/datagram_port.hpp"

namespace ur::device {
namespace detail {
// presignal_increment对仅本机观察计数作饱和递增；饱和返回false且保持值，不改变媒体业务。
inline bool presignal_increment(std::uint32_t& value) {
    if (value == std::numeric_limits<std::uint32_t>::max())
        return false;
    ++value;
    return true;
}
}  // namespace detail
// ReplyPreSignalDiagnostic只记录所选回复在选择前的原未知队列事实；不导出nonce或媒体。
// complete表示观测从空队列开始、未溢出且received=evicted+retained，不表示播放完整。
struct ReplyPreSignalDiagnostic {
    std::uint32_t observation = 0;  // 本端成功新选择的观察次序，不是协议事件或回复标识。
    std::uint32_t received = 0;     // 该nonce选择前实际进入未知缓存的包数。
    std::uint32_t evicted = 0;      // 原满16时实际丢弃的该nonce包数。
    std::uint32_t retained = 0;     // 选择时原队列仍保留的该nonce包数。
    bool complete = false;          // 统计槽/计数未溢出且没有历史补采。
};
// ReplyDatagram 只接纳当前回复数据报；提前到达的未知回复有限暂存，旧回复绝不重新交给解码器。
// 本类不负责密钥认证，必须包装已绑定授权服务器的 DatagramPort。
class ReplyDatagram final : public DatagramPort {
public:
    explicit ReplyDatagram(DatagramPort& socket) : socket_(socket) {}
    ~ReplyDatagram() override { close(); }
    ReplyDatagram(const ReplyDatagram&) = delete;
    ReplyDatagram& operator=(const ReplyDatagram&) = delete;
    // select_nonce 只能来自已校验的回复信令；相同 nonce 幂等，退休 nonce 不得重新使用。
    bool select_nonce(const std::array<std::uint8_t, 8>& nonce);
    // enable_presignal_diagnostics仅允许从原空队列、未选择状态启用；迟启用不补造历史。
    bool enable_presignal_diagnostics();
    // presignal_diagnostic只读最近成功新选择的计数快照，不消费数据报或重读nonce。
    std::optional<ReplyPreSignalDiagnostic> presignal_diagnostic() const;
    bool open(std::string_view address, std::uint16_t port) override {
        return !closed_ && socket_.open(address, port);
    }
    bool set_peer(std::string_view address, std::uint16_t port) override {
        return !closed_ && socket_.set_peer(address, port);
    }
    std::uint16_t local_port() const override { return closed_ ? 0 : socket_.local_port(); }
    bool send(const std::vector<std::uint8_t>& packet) override {
        return !closed_ && socket_.send(packet);
    }
    // receive 最多检查16个网络包，限制陌生 nonce 消耗；最多缓存16个单 Opus 包。
    ReceiveResult receive(std::vector<std::uint8_t>& packet) override;
    // close 清除缓存和 nonce 历史后关闭底层端口，此对象不可重新启用。
    void close() override;
    std::size_t buffered() const { return pending_.size(); }  // 可观测缓存包数，不包含数据原文。
private:
    // PreSignalEntry只在显式观察开启后登记未知nonce；固定16槽且成功选择后回收，不限制业务回复总数。
    struct PreSignalEntry {
        std::array<std::uint8_t, 8> nonce{};  // 内部匹配标记，不能进入诊断输出。
        std::uint32_t received = 0;           // 真实入队次数，UINT32_MAX时饱和。
        std::uint32_t evicted = 0;            // 原erase实际驱逐次数，同样饱和。
        bool used = false;                    // 当前槽正在统计尚未选择的未来回复。
        bool complete = true;                 // 该槽计数没有饱和。
    };
    // observe_received/observe_evicted仅附着原真实队列操作；关闭开关不调用或额外解析包。
    void observe_received(const std::array<std::uint8_t, 8>& nonce);
    void observe_evicted(const std::array<std::uint8_t, 8>& nonce);
    // freeze_presignal只在原新nonce成功选择后冻结该nonce，不重新收包或改变队列。
    void freeze_presignal(const std::array<std::uint8_t, 8>& nonce);
    bool presignal_enabled_ = false;           // 默认关闭，无额外解析/分配/输出。
    bool presignal_incomplete_ = false;        // 统计槽饱和后保守标不完整，不能据伪零证明没有丢包。
    std::uint32_t presignal_observation_ = 0;  // 有界本机观察序号，饱和后停止产生新快照。
    std::array<PreSignalEntry, 16> presignal_{};  // 固定内存，不增加原pending16容量。
    std::optional<ReplyPreSignalDiagnostic> presignal_snapshot_;  // 最近一次成功选择的不可变计数。
    // packet_nonce 校验当前新通道单 Opus 包边界，不进行加密或播放。
    static std::optional<std::array<std::uint8_t, 8>> packet_nonce(
        const std::vector<std::uint8_t>& packet);
    DatagramPort& socket_;                               // 平台端点，寿命覆盖本适配器。
    std::optional<std::array<std::uint8_t, 8>> active_;  // 已收到回复信令授权的 nonce。
    std::vector<std::array<std::uint8_t, 8>> retired_;  // 最多512个，满时重新建会话而非忘记旧回复。
    std::vector<std::vector<std::uint8_t>> pending_;    // 先于 MQTT 到达的有界加密帧。
    bool closed_ = false;                               // 终态不复活。
};
}  // namespace ur::device
