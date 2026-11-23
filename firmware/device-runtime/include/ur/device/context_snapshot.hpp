// 文件说明：转人工私有完成文本的便携解析与有界接收；不依赖桌面驱动，不将正文写日志。
#pragma once
#include <optional>
#include <vector>
#include "ur/device/voice_wire.hpp"

namespace ur::device {
// ContextSnapshotTurn 只携带本人授权的完成文本，顺序与MQTT事件水位独立。
struct ContextSnapshotTurn {
    std::string message_id;        // 原消息标识，不能在同快照重复。
    std::uint64_t sequence = 0;    // 原消息顺序，严格递增安全整数。
    std::uint64_t turn_index = 0;  // 原轮次，user依次递增。
    std::string role;              // 仅user或assistant。
    std::string text;              // 私有UTF-8正文，不得进入普通诊断。
};
// ContextSnapshot
// 保留完整授权wire，摘要是服务端私有原快照摘要，设备不能用删减wire重算。
struct ContextSnapshot {
    Envelope envelope;                       // 当前人工腿的原messageSeq及投递身份。
    std::string delivery_id;                 // 固定小写SHA256投递键，与msgToken相同。
    std::string consent_id;                  // 本人原授权键。
    std::string source_session_id;           // 原AI会话，不是当前人工腿。
    std::string service_id;                  // 原精确人工服务，不接受通配符。
    std::string service_revision;            // 原目录修订，不随重投更新。
    std::string snapshot_version;            // 原完成快照版本。
    std::string snapshot_digest;             // 服务端完整私有快照摘要，原样回显。
    std::uint64_t first_turn = 0;            // 原范围首轮。
    std::uint64_t last_turn = 0;             // 原范围末轮。
    std::uint64_t expires_at = 0;            // 原授权墙钟截止，重投不延期。
    std::vector<ContextSnapshotTurn> turns;  // 完整有序文本，最多64条及32轮。
};
// ContextSnapshotAck 只表示实际设备留存，不表示用户阅读、发声或重新授权。
struct ContextSnapshotAck {
    Envelope envelope;             // 原token/session，code=200，无事件水位。
    std::string delivery_id;       // 原投递键。
    std::string consent_id;        // 原授权键。
    std::string snapshot_version;  // 原不可变版本。
    std::string snapshot_digest;   // 原服务端私有摘要。
};
// decode_context_snapshot
// 失败清空out；只验证严格MSG契约，接收方及通话状态由接收器核验。
WireResult decode_context_snapshot(WireRoute route, std::string_view body, ContextSnapshot& out);
// decode_context_snapshot_ack 解析严格原接收确认，不以broker确认替代此事件。
WireResult decode_context_snapshot_ack(WireRoute route, std::string_view body,
                                       ContextSnapshotAck& out);
// context_snapshot_ack 完整校验已留存值后生成固定回执；失败清空out。
WireResult context_snapshot_ack(const ContextSnapshot& snapshot, Envelope& out);

// ContextSnapshotInbox
// 当前一次转人工仅一份授权；超预算拒绝，不淘汰尚有效原文来接受另一份。
// 单事件线程持有，最多一份64KiB
// wire及其有界解析/ACK，逻辑预算不是分配器堆占用保证。
class ContextSnapshotInbox {
public:
    // receive
    // 校验当前腿、实际连通/保持状态与原截止，先完整留存再输出可发布的原ACK。
    // 原字节规范化一致的重投可重复ACK；不同投递或同键改正文拒绝，不续期或重新留存。
    bool receive(std::string_view body, std::string_view session, bool connected, bool held,
                 std::uint64_t wall_ms, Envelope& ack);
    // expire 墙钟只升不降，到期删除正文但保留本腿水位，迟到消息不能重开授权。
    void expire(std::uint64_t wall_ms);
    // clear 仅在原通话结束/断连时清理私有留存及水位，不改变媒体或其他会话。
    void clear();
    // snapshot
    // 返回只读实际留存供授权消费，引用在expire/clear后失效，不是用户已阅读证明。
    const std::optional<ContextSnapshot>& snapshot() const { return retained_; }

private:
    std::optional<ContextSnapshot> retained_;  // 完整解析留存，不使用UI布尔伪造收到。
    std::string canonical_wire_;               // 规范化原完整wire用于严格幂等，不写日志。
    Envelope ack_;                             // 固定原接收确认，网络失败后重投不换身份。
    std::uint64_t message_highwater_ = 0;      // 同腿已接收水位，过期不清零。
    std::uint64_t wall_highwater_ = 0;         // 防止校时回退复活授权。
};
}  // namespace ur::device
