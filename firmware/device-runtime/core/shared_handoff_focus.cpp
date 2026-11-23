// 文件说明：完整实际焦点结果的共享宿主装配；失败发布保留原结果，禁止以受理、诊断或网络下行伪造完成。
#include "ur/device/shared_handoff_focus.hpp"
#include <algorithm>

namespace ur::device {
namespace {
// complete_group 检查本宿主固定完整资源集合，顺序不影响组身份。
bool complete_group(const std::vector<std::string>& resources) {
    auto sorted = resources;
    std::sort(sorted.begin(), sorted.end());
    return sorted == std::vector<std::string>{"microphone", "speaker"};
}
// same_command 不允许调用者只修改结构体的操作或CAS而保留另一份信封。
bool same_command(const FocusCommand& left, const FocusCommand& right) {
    return left.operation == right.operation && left.expected_revision == right.expected_revision &&
           left.focus_id == right.focus_id && left.resources == right.resources &&
           left.priority == right.priority && left.resume_policy == right.resume_policy &&
           left.expires_at == right.expires_at;
}
}  // namespace

SharedHandoffFocusEndpoint::SharedHandoffFocusEndpoint(PublishPort& publisher, FocusDriver& driver,
                                                       FocusAuthority& authority, std::string up,
                                                       std::string down, std::uint64_t deadline)
    : publisher_(publisher),
      up_(std::move(up)),
      down_(std::move(down)),
      endpoint_(*this, driver, authority, {"microphone", "speaker"}, up_, down_, deadline) {
    const auto idle = driver.initial_idle_observation();
    supported_ = idle && complete_group(idle->resources) && idle->revision == driver.revision() &&
                 deadline && !up_.empty() && !down_.empty() && up_ != down_;
}

FocusEndpointResult SharedHandoffFocusEndpoint::receive(std::string_view topic,
                                                        std::string_view body, std::uint64_t now,
                                                        std::uint64_t wall) {
    if (!supported_)
        return FocusEndpointResult::Closed;
    // 通用端点允许能力全集子组，本宿主却只能按原完整麦克风／扬声器租约执行。
    // 先校验再准入，避免部分组启动后因无法出具完整回执而进入未知资源状态。
    if (topic == down_) {
        Envelope envelope;
        if (decode_envelope(body, envelope) != DecodeResult::Ok)
            return FocusEndpointResult::Invalid;
        if (envelope.method == "focusControl") {
            FocusCommand command;
            if (decode_focus_control(WireRoute::Signaling, body, command) != WireResult::Ok ||
                !complete_group(command.resources))
                return FocusEndpointResult::Invalid;
        }
    }
    return endpoint_.receive(topic, body, now, wall);
}

void SharedHandoffFocusEndpoint::poll(std::uint64_t now, std::uint64_t wall) {
    endpoint_.poll(now, wall);
}
bool SharedHandoffFocusEndpoint::supports_microphone_speaker() const { return supported_; }

FocusSubmission SharedHandoffFocusEndpoint::submit(const FocusCommand& command, std::uint64_t now,
                                                   std::uint64_t wall) {
    std::string bytes;
    FocusCommand decoded;
    if (!supported_ || encode_envelope(command.envelope, bytes) != DecodeResult::Ok ||
        decode_focus_control(WireRoute::Signaling, bytes, decoded) != WireResult::Ok ||
        !complete_group(command.resources) || !same_command(command, decoded))
        return {FocusAdmission::Invalid, {}};
    const auto original =
        std::find_if(correlations_.begin(), correlations_.end(), [&](const auto& record) {
            return record.command.envelope.session == command.envelope.session &&
                   record.command.envelope.token == command.envelope.token;
        });
    if (original != correlations_.end() && original->bytes != bytes)
        return {FocusAdmission::Conflict, {}};
    const bool fresh = original == correlations_.end();
    if (fresh && correlations_.size() >= 32)
        return {FocusAdmission::Full, {}};
    if (fresh)
        correlations_.push_back({command, bytes});
    const auto accepted = endpoint_.receive(down_, bytes, now, wall);
    if (accepted != FocusEndpointResult::Handled && fresh)
        correlations_.pop_back();
    switch (accepted) {
        case FocusEndpointResult::Handled:
            // 原重放也等待唯一执行器再次发布同字节，不凭历史诊断先交付新实际事实。
            return {FocusAdmission::Accepted, {}};
        case FocusEndpointResult::Full:
            return {FocusAdmission::Full, {}};
        case FocusEndpointResult::Closed:
            return {FocusAdmission::Closed, {}};
        default:
            return {FocusAdmission::Invalid, {}};
    }
}

bool SharedHandoffFocusEndpoint::publish(std::string_view topic, std::string_view body) {
    FocusState actual;
    if (topic != up_ || decode_focus_state(WireRoute::Signaling, body, actual) != WireResult::Ok ||
        !actual.envelope.code || !complete_group(actual.resources) || receipts_.full())
        return false;
    const auto original = std::find_if(
        correlations_.begin(), correlations_.end(),
        [&](const auto& record) { return focus_reply_matches(record.command, actual); });
    if ((original != correlations_.end() && results_.full()) || !publisher_.publish(topic, body))
        return false;
    if (original != correlations_.end())
        results_.push_back(AiHandoffFocusResult{actual, false});
    receipts_.push_back(std::move(actual));
    return true;
}

std::optional<AiHandoffFocusResult> SharedHandoffFocusEndpoint::take_result() {
    if (results_.empty())
        return {};
    auto result = std::move(*results_.front());
    results_.pop_front();
    return result;
}
std::optional<FocusState> SharedHandoffFocusEndpoint::take_receipt() {
    if (receipts_.empty())
        return {};
    auto result = std::move(*receipts_.front());
    receipts_.pop_front();
    return result;
}
void SharedHandoffFocusEndpoint::close() { endpoint_.close(); }
bool SharedHandoffFocusEndpoint::settled() const { return endpoint_.settled(); }
}  // namespace ur::device
