// 文件播放器：严格接纳→驱动实际完成→固定回执；输出失败时切断实例，不扩大到AI/直播。
#include "ur/device/playback_engine.hpp"
#include <cJSON.h>
#include <memory>
#include <utility>

namespace ur::device {
namespace {
// safe_integer 保持JSON跨语言精度；修订号不能因溢出复用旧状态。
constexpr std::uint64_t safe_integer = 9007199254740991ULL;
// Json 保证回执编码失败时释放临时树。
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
// add_integer 仅用于已校验uint的十进制字面量，避免cJSON先近似2^53-1而破坏实际位置。
bool add_integer(cJSON* object, const char* key, std::uint64_t value) {
    return cJSON_AddRawToObject(object, key, std::to_string(value).c_str()) != nullptr;
}
// encode_fact 仅输出固定字段，驱动错误信息或媒体凭据不得出现在报文。
bool encode_fact(const PlaybackFact& fact, std::uint64_t revision, const std::string& playback_id,
                 const char* reason, std::string& payload) {
    Json object(cJSON_CreateObject(), cJSON_Delete);
    if (!object || !cJSON_AddStringToObject(object.get(), "state", fact.state.c_str()) ||
        !add_integer(object.get(), "revision", revision) ||
        !cJSON_AddStringToObject(object.get(), "playbackId", playback_id.c_str()))
        return false;
    if (fact.state == "failed") {
        if (!cJSON_AddStringToObject(object.get(), "reason", reason))
            return false;
    } else {
        if (!add_integer(object.get(), "positionMs", fact.position_ms) ||
            !cJSON_AddNumberToObject(object.get(), "rate", fact.rate) ||
            (fact.duration_ms && !add_integer(object.get(), "durationMs", *fact.duration_ms)))
            return false;
    }
    std::unique_ptr<char, decltype(&cJSON_free)> text(cJSON_PrintUnformatted(object.get()),
                                                      cJSON_free);
    if (!text)
        return false;
    payload = text.get();
    return true;
}
}  // namespace

PlaybackEngine::PlaybackEngine(PlaybackDriver& driver, std::string session, std::string playback_id,
                               std::uint64_t deadline, std::uint64_t timeout_ms)
    : driver_(driver),
      session_(std::move(session)),
      playback_id_(std::move(playback_id)),
      deadline_(deadline),
      timeout_ms_(timeout_ms) {
    closed_ = session_.empty() || session_.size() > 128 || playback_id_.empty() ||
              playback_id_.size() > 128 || deadline == 0 || timeout_ms == 0 || timeout_ms > 30000;
    history_.reserve(32);
}
PlaybackEngine::~PlaybackEngine() { close(); }
bool PlaybackEngine::terminal() const {
    return state_ == "stopped" || state_ == "ended" || state_ == "failed";
}
bool PlaybackEngine::observe(std::uint64_t now) {
    if (last_time_ && now < *last_time_) {
        terminate("clockInvalid");
        return false;
    }
    last_time_ = now;
    if (now >= deadline_) {
        terminate("sessionExpired");
        return false;
    }
    return true;
}
PlaybackSubmission PlaybackEngine::submit(std::string_view json, std::uint64_t now) {
    PlaybackCommand command;
    if (decode_playback_control(WireRoute::Signaling, json, command) != WireResult::Ok ||
        !command.envelope.event_sequence.has_value() || command.envelope.session != session_ ||
        command.playback_id != playback_id_ || command.media_revision)
        return {};
    const auto sequence =
        *command.envelope.event_sequence;  // 原解码要求显式水位，准入与移动前使用同一已验证值。
    const bool live = observe(now);
    for (const auto& record : history_) {
        if (record.command.envelope.token != command.envelope.token)
            continue;
        if (record.command.envelope.payload != command.envelope.payload ||
            record.command.envelope.event_sequence != command.envelope.event_sequence)
            return {PlaybackAdmission::Conflict, {}};
        if (!record.reply.empty())
            return {PlaybackAdmission::Replay, record.reply};
        return {PlaybackAdmission::Pending, {}};
    }
    if (!live && now >= deadline_)
        return {PlaybackAdmission::Expired, {}};
    if (closed_)
        return {PlaybackAdmission::Closed, {}};
    if (active_ || replies_.full())
        return {PlaybackAdmission::Busy, {}};
    if (history_.size() == 32)
        return {PlaybackAdmission::Full, {}};
    if (command.expected_revision != revision_ || sequence <= event_sequence_)
        return {PlaybackAdmission::Stale, {}};
    const bool can_play = state_ == "loaded" || state_ == "paused" || state_ == "playing";
    if (terminal() || (state_ == "seeking" && command.operation != "stop") ||
        (command.operation == "load" && !state_.empty()) ||
        (command.operation != "load" && state_.empty()) ||
        (command.operation == "play" && !can_play) ||
        (command.operation == "pause" && state_ != "playing" && state_ != "paused"))
        return {PlaybackAdmission::StateConflict, {}};
    if (command.operation == "play" && !driver_.output_permitted())
        return {PlaybackAdmission::ResourceBusy, {}};
    event_sequence_ = sequence;
    history_.push_back({std::move(command), {}});
    active_ = history_.size() - 1;
    admitted_at_ = now;
    started_ = false;
    return {PlaybackAdmission::Accepted, {}};
}
bool PlaybackEngine::valid_fact(const PlaybackFact& fact) const {
    if (fact.generation != generation_ || fact.code != 200 || fact.position_ms > safe_integer ||
        (fact.duration_ms &&
         (*fact.duration_ms > safe_integer || *fact.duration_ms < fact.position_ms)) ||
        (fact.rate != 0.5 && fact.rate != 1 && fact.rate != 1.5 && fact.rate != 2))
        return false;
    if (fact.spontaneous)
        return !active_ && !terminal() && !state_.empty() &&
               ((fact.state == "ended" && state_ == "playing") || fact.state == "failed" ||
                (state_ == "seeking" && fact.state == seeking_resume_state_ &&
                 (fact.state != "playing" || driver_.output_permitted())));
    if (!active_)
        return false;
    const auto& operation = history_[*active_].command.operation;
    if (operation == "load")
        return fact.state == "loaded";
    if (operation == "play")
        return fact.state == "playing" && driver_.output_permitted();
    if (operation == "pause")
        return fact.state == "paused";
    if (operation == "stop")
        return fact.state == "stopped";
    if (operation == "seek" && fact.state == "seeking")
        return true;  // 长seek先回实际seeking，随后用独立事件确认原状态及实际可达位置。
    // 短seek/setRate在实际可达位置/倍率稳定后确认，不回显目标位置冒充执行结果。
    const auto& rate = history_[*active_].command.rate;  // 本次在途操作的已解析倍率，不补默认值。
    return fact.state == state_ &&
           (operation != "setRate" || (rate.has_value() && fact.rate == *rate));
}
bool PlaybackEngine::emit(const PlaybackFact& fact, bool spontaneous, const char* reason) {
    // 操作回执缺少在途关联时失败关闭，不把它变成自发事实。
    if ((!spontaneous && !active_.has_value()) || revision_ == safe_integer)
        return false;
    Envelope envelope;
    envelope.request = false;
    envelope.method = "playbackState";
    envelope.session = session_;
    envelope.token = spontaneous ? "player-event-" + std::to_string(generation_) + "-" +
                                       std::to_string(revision_ + 1)
                                 : history_[*active_].command.envelope.token;
    if (!spontaneous)
        envelope.code = 200;
    if (!encode_fact(fact, revision_ + 1, playback_id_, reason, envelope.payload))
        return false;
    std::string json;
    if (encode_envelope(envelope, json) != DecodeResult::Ok)
        return false;
    if (!spontaneous)
        history_[*active_].reply = json;
    if (!replies_.push_back(std::move(json)))
        return false;
    ++revision_;
    if (fact.state == "seeking") {
        seeking_resume_state_ = state_;
        seeking_at_ = last_time_.value_or(0);
    } else {
        seeking_resume_state_.clear();
    }
    state_ = fact.state;
    if (!spontaneous) {
        active_.reset();
        started_ = false;
    }
    return true;
}
void PlaybackEngine::fail(int code, const char* reason) {
    if (!active_)
        return;
    auto& record = history_[*active_];
    Envelope envelope;
    envelope.token = record.command.envelope.token;
    envelope.method = "playbackState";
    envelope.session = session_;
    envelope.request = false;
    envelope.code = code;
    envelope.payload = std::string("{\"reason\":\"") + reason + "\"}";
    if (encode_envelope(envelope, record.reply) != DecodeResult::Ok ||
        !replies_.push_back(std::string(record.reply))) {
        closed_ = true;      // 仍保留原token结果，不驱逐以重新执行副作用。
        driver_.stop_all();  // 序列化失败也必须切断输出，不能因closed标志跳过释放。
    }
    active_.reset();
    started_ = false;
}
void PlaybackEngine::terminate(const char* reason) {
    if (!closed_) {
        closed_ = true;
        driver_.stop_all();  // 即使结果未知也关闭实例，禁止下一命令接管旧输出。
    }
    fail(504, reason);
    if (!terminal() && !state_.empty() && !terminal_reason_)
        terminal_reason_ = reason;  // 原命令失败与当前播放器失败是不同事实，不覆盖原回执。
    flush_terminal();
}
void PlaybackEngine::flush_terminal() {
    if (!terminal_reason_ || replies_.full())
        return;
    PlaybackFact fact;  // 关闭失败只报告实例/修订号/固定原因，不伪造实际停止位置。
    fact.generation = generation_;
    fact.spontaneous = true;
    fact.state = "failed";
    if (emit(fact, true, terminal_reason_->c_str()))
        terminal_reason_.reset();
}
void PlaybackEngine::poll(std::uint64_t now) {
    if (!observe(now) || closed_ || terminal())
        return;
    if ((state_ == "playing" || (state_ == "seeking" && seeking_resume_state_ == "playing") ||
         (active_ && started_ && history_[*active_].command.operation == "play")) &&
        !driver_.output_permitted()) {
        // 异步play受理后也可能已出声，不能等playing回执；正常焦点移交必须先实际pause。
        terminate("resourceLost");
        return;
    }
    if ((active_ && now - admitted_at_ >= timeout_ms_) ||
        (state_ == "seeking" && now - seeking_at_ >= timeout_ms_)) {
        // 操作超时无法判断已执行位置；切断整个本实例，不猜测pause/play成功。
        terminate("resultUnknown");
        return;
    }
    if (active_ && !started_) {
        const auto& command = history_[*active_].command;
        if (command.operation == "play" && !driver_.output_permitted()) {
            fail(409, "resourceBusy");
            return;
        }
        const auto candidate = generation_ + 1;  // 未受理不能失效仍在播放的旧媒体事件。
        if (!driver_.begin(command, candidate)) {
            fail(503, "driverUnavailable");
            return;
        }
        generation_ = candidate;
        started_ = true;
    }
    // 平台未消费回执时保留驱动事实；自然结束也不能先取走再丢失，下一轮继续收敛。
    if (replies_.full())
        return;
    for (unsigned count = 0; count < 4 && !replies_.full(); ++count) {
        auto fact = driver_.take_fact();
        if (!fact)
            break;
        if (fact->generation != generation_ || terminal())
            continue;  // 迟到事实不能覆盖当前实例、已完成请求或终态。
        if (!fact->spontaneous && !active_)
            continue;  // 重复实际完成不推进修订号。
        if (!fact->spontaneous && fact->code >= 400 && fact->code <= 599) {
            // 非成功事实不能假设驱动没有产生副作用；先切断输出再固定失败。
            fail(fact->code, "deviceRejected");
            terminate("deviceRejected");
            return;
        }
        if (!valid_fact(*fact) || !emit(*fact, fact->spontaneous)) {
            terminate("invalidDriverResult");
            return;
        }
        if (terminal()) {
            driver_.stop_all();
            return;
        }
    }
}
std::optional<std::string> PlaybackEngine::take_reply() {
    flush_terminal();  // 内存或队列背压恢复后仍可补交唯一失败事实。
    if (replies_.empty())
        return std::nullopt;
    auto result = std::move(*replies_.front());
    replies_.pop_front();
    flush_terminal();  // 只补交未排入的唯一关闭事实，既有命令回执仍按顺序返回。
    return result;
}
void PlaybackEngine::close() { terminate("resultUnknown"); }
}  // namespace ur::device
