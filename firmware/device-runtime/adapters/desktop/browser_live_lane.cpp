// 文件说明：节目末端mask实际票据桥；完整组关闭不伪造track paused或源授权撤销。
#include "ur/device/json_parse.hpp"
#include "browser_live_lane.hpp"
#include <cJSON.h>
#include <charconv>
#include <limits>
#include <memory>
#include "ur/device/envelope.hpp"

namespace ur::device {
// BrowserLiveLane 仅接可信三种物理组，构造不证明真实初始关闭。
BrowserLiveLane::BrowserLiveLane(std::string lane) : lane_(std::move(lane)) {
    failed_ = lane_ != "audio" && lane_ != "video" && lane_ != "audioVideo";
}
// poll 在实际票据超时/时钟回退后保持失败，不借迟到消息恢复输出。
bool BrowserLiveLane::poll(std::uint64_t now) {
    if ((clock_ && now < *clock_) || (pending_ && now >= deadline_))
        failed_ = true;
    if (!failed_)
        clock_ = now;
    return !failed_;
}
// request_output_gate 首次仅实际关闭；每次新操作分配唯一原票据，未知不能换票重试。
std::optional<std::uint64_t> BrowserLiveLane::request_output_gate(bool enabled, std::uint64_t now) {
    if (!poll(now) || pending_ || command_ || (!ticket_ && enabled) ||
        ticket_ == std::numeric_limits<std::uint64_t>::max() ||
        now > std::numeric_limits<std::uint64_t>::max() - 2000)
        return {};
    ++ticket_;
    enabled_ = enabled;
    deadline_ = now + 2000;
    pending_ = true;
    sent_ = false;
    command_ = "{\"kind\":\"liveLaneGate\",\"lane\":\"" + lane_ + "\",\"gateTicket\":\"" +
               std::to_string(ticket_) + "\",\"enabled\":" + (enabled ? "true}" : "false}");
    return ticket_;
}
// output_gate_confirmed 只提供当前实际票据，无输出或节目原意图的派生授权。
bool BrowserLiveLane::output_gate_confirmed(std::uint64_t ticket, bool enabled) const {
    return !failed_ && ticket && ticket == ticket_ && ticket == confirmed_ && enabled == enabled_ &&
           !pending_;
}
// permitted 仅当前末端mask的有限投影，不自动开启节目轨或绕过来源授权。
bool BrowserLiveLane::permitted() const { return enabled_ && output_gate_confirmed(ticket_, true); }
// idle 当前实际关闭才可安全复用；构造、旧false票据或在途均不是证明。
bool BrowserLiveLane::idle() const { return output_gate_confirmed(ticket_, false); }
// permitted_ticket 不输出平台租约身份，不把历史票据变成新的播放授权。
std::optional<std::uint64_t> BrowserLiveLane::permitted_ticket() const {
    return permitted() ? std::optional<std::uint64_t>(ticket_) : std::nullopt;
}
// take_command 移交唯一原字节，实际确认必须晚于移交；队列为空不是实际输出完成。
std::optional<std::string> BrowserLiveLane::take_command() {
    if (failed_ || !command_)
        return {};
    auto command = std::move(command_);
    command_.reset();
    sent_ = true;
    return command;
}
// receive 核严格本地结构与原票据；旧实际关闭只能隔离，不能确认新租约。
bool BrowserLiveLane::receive(std::string_view body, std::uint64_t now) {
    if (!poll(now))
        return false;
    std::string canonical;
    if (canonical_json_object(body, canonical) != DecodeResult::Ok)
        return false;
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> object(ur::device::parse_json(canonical.c_str()),
                                                           cJSON_Delete);
    const auto* kind = cJSON_GetObjectItemCaseSensitive(object.get(), "kind");
    const auto* lane = cJSON_GetObjectItemCaseSensitive(object.get(), "lane");
    const auto* raw = cJSON_GetObjectItemCaseSensitive(object.get(), "gateTicket");
    const auto* enabled = cJSON_GetObjectItemCaseSensitive(object.get(), "enabled");
    if (!object || cJSON_GetArraySize(object.get()) != 4 || !cJSON_IsString(kind) ||
        std::string_view(kind->valuestring) != "liveLaneGated" || !cJSON_IsString(lane) ||
        std::string_view(lane->valuestring) != lane_ || !cJSON_IsString(raw) ||
        !cJSON_IsBool(enabled))
        return false;
    const std::string text = raw->valuestring;
    std::uint64_t ticket = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), ticket);
    if (text.empty() || text.front() == '0' || parsed.ec != std::errc{} ||
        parsed.ptr != text.data() + text.size() || !ticket || ticket > ticket_)
        return false;
    if (ticket < ticket_)
        return true;  // 原迟到严格事实只隔离，不刷新当前截止或许可。
    if (!sent_ || cJSON_IsTrue(enabled) != enabled_)
        return false;
    confirmed_ = ticket;
    pending_ = false;
    return true;
}
}  // namespace ur::device
