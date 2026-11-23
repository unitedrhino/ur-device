// 文件说明：固定文件输出路径本地桥票据关联；实际事实与云端焦点租约、节目状态分离。
#include "ur/device/json_parse.hpp"
#include "browser_file_lane.hpp"
#include <cJSON.h>
#include <charconv>
#include <limits>
#include <memory>
#include "ur/device/envelope.hpp"

namespace ur::device {
BrowserFileLane::BrowserFileLane(std::string lane) : lane_(std::move(lane)) {
    failed_ = lane_ != "audio" && lane_ != "video" && lane_ != "audioVideo";
}

bool BrowserFileLane::poll(std::uint64_t now) {
    if ((clock_ && now < *clock_) || (pending_ && now >= deadline_))
        failed_ = true;
    if (!failed_)
        clock_ = now;
    return !failed_;
}

std::optional<std::uint64_t> BrowserFileLane::request_output_gate(bool enabled, std::uint64_t now) {
    if (!poll(now) || pending_ || command_ || (!ticket_ && enabled) ||
        ticket_ == std::numeric_limits<std::uint64_t>::max() ||
        now > std::numeric_limits<std::uint64_t>::max() - 2000)
        return {};
    ++ticket_;
    enabled_ = enabled;
    deadline_ = now + 2000;
    pending_ = true;
    sent_ = false;
    command_ = "{\"kind\":\"fileLaneGate\",\"lane\":\"" + lane_ + "\",\"gateTicket\":\"" +
               std::to_string(ticket_) + "\",\"enabled\":" + (enabled ? "true}" : "false}");
    return ticket_;
}

bool BrowserFileLane::output_gate_confirmed(std::uint64_t ticket, bool enabled) const {
    return !failed_ && ticket != 0 && ticket == ticket_ && ticket == confirmed_ &&
           enabled == enabled_ && !pending_;
}

std::optional<std::string> BrowserFileLane::take_command() {
    if (failed_ || !command_)
        return {};
    auto result = std::move(command_);
    command_.reset();
    sent_ = true;
    return result;
}

bool BrowserFileLane::receive(std::string_view body, std::uint64_t now) {
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
        std::string_view(kind->valuestring) != "fileLaneGated" || !cJSON_IsString(lane) ||
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
        return true;  // 原严格迟到事实不改当前状态，不能刷新截止。
    if (!sent_ || cJSON_IsTrue(enabled) != enabled_)
        return false;
    confirmed_ = ticket;
    pending_ = false;
    return true;
}
}  // namespace ur::device
