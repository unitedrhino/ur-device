// 软件音量动作：参数校验后修改播放驱动，回复读取驱动实态而不是回显输入。
#include "file_actions.hpp"
#include <charconv>
#include <utility>

namespace ur::device {
bool FileActions::begin(const ActionRequest& request) {
    if (completion_ || pending_token_)
        return false;
    int code = 501;
    if (request.action == "setVolume" || request.action == "getVolume") {
        std::string params;
        code = 422;
        if (canonical_json_object(request.params, params) == DecodeResult::Ok) {
            if (request.action == "getVolume" && params == "{}") {
                code = 200;
            } else if (request.action == "setVolume") {
                constexpr std::string_view prefix = "{\"volume\":";
                if (params.size() > prefix.size() + 1 &&
                    params.compare(0, prefix.size(), prefix) == 0 && params.back() == '}') {
                    unsigned volume = 0;
                    const auto* end = params.data() + params.size() - 1;
                    const auto parsed = std::from_chars(params.data() + prefix.size(), end, volume);
                    if (parsed.ec == std::errc{} && parsed.ptr == end && volume <= 100) {
                        if (deferred_) {
                            if (deferred_->request_volume(request.token, volume)) {
                                pending_token_ = request.token;
                                return true;
                            }
                            code = 503;
                        } else {
                            code = pcm_.set_volume(volume) ? 200 : 503;
                        }
                    }
                }
            }
        }
    }
    completion_ =
        ActionCompletion{request.token, code,
                         code == 200 ? "{\"volume\":" + std::to_string(pcm_.volume()) + "}" : "{}"};
    ++completed_count_;
    last_code_ = code;
    return true;
}
std::optional<ActionCompletion> FileActions::take_completion() {
    if (pending_token_ && deferred_) {
        const auto applied = deferred_->take_volume_applied();
        if (applied && applied->token == *pending_token_) {
            completion_ = ActionCompletion{*pending_token_, 200,
                                           "{\"volume\":" + std::to_string(applied->percent) + "}"};
            pending_token_.reset();
            ++completed_count_;
            last_code_ = 200;
        }
    }
    auto completion = std::move(completion_);
    completion_.reset();
    return completion;
}
bool FileActions::cancel(std::string_view token) {
    if (pending_token_ && *pending_token_ == token)
        return false;  // 命令已跨进程排队，不能证明浏览器尚未执行。
    if (completion_ && completion_->token == token)
        completion_.reset();
    return true;
}
}  // namespace ur::device
