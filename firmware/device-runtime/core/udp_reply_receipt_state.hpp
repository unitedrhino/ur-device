// 候选文件说明：只为selected安装确认分配有界事实，不创建并行回复许可或改16包媒体缓存。
#pragma once
#include "ur/device/voice_client.hpp"
#include <algorithm>
#include <utility>

namespace ur::device {
// UDPReplyReceiptState持一条原完整Start、一条原回执和512个只用于拒绝的token摘要。
// 32轮不关闭会话，512摘要满时拒绝新回复；摘要碰撞也只拒绝，永远不据摘要发200。
// 持久对象上限4608字节、四字符串容量合计上限4096字节（含终止符另4字节）；
// allocator元数据及解析/编码临时对象另计，不宣称整个堆仅这些字节。
struct VoiceClient::UDPReplyReceiptState {
    std::array<std::uint64_t, 512> seen{};    // 原token不可借新seq/nonce再次安装；不删除历史。
    std::size_t count = 0;                    // 上限与原回复nonce墓碑512一致，满时不淘汰。
    std::string original, receipt;            // 分别不超过1024/768字节，只有当前原完整字节可重放。
    std::string token, response;              // 各不超过128字节，仅内部关联，不进入网页或日志。
    std::uint64_t started = 0, observed = 0;  // 原首次单调时间，不随发布重试延长。
    bool clock_failed = false, installed = false, delivered = false, dirty = false,
         retired = true;  // 安装/曾交付/待重放独立，重复背压不撤销原成功。
    bool shared_managed = false, shared_owned = false,
         shared_permanent =
             false;  // 仅borrowed可信宿主同步整组；永久围栏锁存，默认独立客户端不启用。
    // digest只产生拒绝索引，非密码认证；碰撞保守拒绝，完整当前字节才可能重放历史200。
    static std::uint64_t digest(std::string_view value) {
        std::uint64_t out = 14695981039346656037ULL;
        for (const unsigned char ch : value) {
            out ^= ch;
            out *= 1099511628211ULL;
        }
        return out;
    }
    // observe锁存回退，不重新取起点或因回退回收去重事实。
    bool observe(std::uint64_t now) {
        if (clock_failed || (count && now < observed)) {
            clock_failed = true;
            retire();
            return false;
        }
        observed = now;
        return true;
    }
    // retire只丢未交付意图，保留原成功字节与全部已见摘要；不是播放停止事实。
    void retire() {
        retired = true;
        dirty = false;
    }
    // reject_shared
    // 失权合法Start只记拒绝摘要，不分配正文/安装/截止；摘要满保守拒绝新工作、不驱逐历史。
    void reject_shared(const VoiceEvent& event, std::uint64_t now) {
        const auto value = digest(event.envelope.token);
        if (observe(now) && count < seen.size() &&
            std::find(seen.begin(), seen.begin() + count, value) == seen.begin() + count)
            seen[count++] = value;
        retire();
    }
    // inspect先核完整当前原票据；历史或碰撞只拒绝，不用摘要判断参数相同。
    Result inspect(const VoiceEvent& event, std::uint64_t now) {
        if (!observe(now))
            return Result::Stale;
        if (event.envelope.token == token) {
            std::string body;
            if (encode_envelope(event.envelope, body) != DecodeResult::Ok || body != original)
                return Result::Invalid;
            if (retired || now < started || now - started >= 2000)
                return Result::Stale;
            // 原停止输入请求尚未准入时可继续同票据安装；首次起点与摘要不重新分配。
            if (!installed)
                return Result::Applied;
            dirty = true;
            return Result::Duplicate;
        }
        const auto value = digest(event.envelope.token);
        if (std::find(seen.begin(), seen.begin() + count, value) != seen.begin() + count)
            return Result::Stale;
        return Result::Applied;
    }
    // stage必须在任何HAL副作用前完成有界票据预留；生成候选字节尚不得发布或报告installed。
    Result stage(const VoiceEvent& event, std::uint64_t now) {
        static_assert(sizeof(UDPReplyReceiptState) <= 4608, "receipt_state_object_budget");
        const auto checked = inspect(event, now);
        if (checked != Result::Applied)
            return checked;
        if (event.envelope.token == token)
            return Result::Applied;
        if (count == seen.size() || (!retired && (!installed || dirty)))
            return Result::Busy;
        std::string start, fact;
        std::string new_token(event.envelope.token), new_response(event.response_id);
        Envelope envelope;
        if (encode_envelope(event.envelope, start) != DecodeResult::Ok || start.size() > 1024 ||
            reply_start_receipt(event, envelope) != WireResult::Ok ||
            encode_envelope(envelope, fact) != DecodeResult::Ok || fact.size() > 768 ||
            start.capacity() > 2048 || fact.capacity() > 1536 || new_token.capacity() > 256 ||
            new_response.capacity() > 256)
            return Result::Invalid;
        seen[count++] = digest(event.envelope.token);
        original = std::move(start);
        receipt = std::move(fact);
        token = std::move(new_token);
        response = std::move(new_response);
        started = now;
        retired = installed = delivered = dirty = false;
        return Result::Applied;
    }
};
}  // namespace ur::device
