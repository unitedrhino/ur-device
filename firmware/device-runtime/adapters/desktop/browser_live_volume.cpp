// 文件说明：聚合节目与原AI实际gain，同步PCM/异步WebRTC共用原动作；失败保留未知而非伪造成功。
#include "ur/device/json_parse.hpp"
#include "browser_live_volume.hpp"
#include <cJSON.h>
#include <limits>
#include <memory>
#include "ur/device/envelope.hpp"
namespace ur::device {
void BrowserLiveVolume::ready(bool actual) {
    if (!closed_)
        ready_ = actual;
}
bool BrowserLiveVolume::set_volume(unsigned) { return false; }  // 跨线程只能走异步关联接口。
unsigned BrowserLiveVolume::volume() const { return pcm_.volume(); }
bool BrowserLiveVolume::request_volume(std::string_view token, unsigned percent) {
    if (!ready_ || closed_ || !clock_ || pending_ || applied_ || command_ || token.empty() ||
        token.size() > 128 || token.find('\0') != std::string_view::npos || percent > 100 ||
        ticket_ == std::numeric_limits<std::uint64_t>::max() ||
        *clock_ > std::numeric_limits<std::uint64_t>::max() - 2000)
        return false;
    ++ticket_;
    const auto ai_token = "live-volume-" + std::to_string(ticket_);
    if (deferred_ && !deferred_->request_volume(ai_token, percent))
        return false;  // 原AI未受理时不向Live发出另一次副作用；本地票据不回退重用。
    pending_ = VolumeApplied{std::string(token), percent};
    live_applied_ = ai_applied_ = false;
    deadline_ = *clock_ + 2000;
    command_ = "{\"kind\":\"liveVolume\",\"volumeTicket\":\"" + std::to_string(ticket_) +
               "\",\"percent\":" + std::to_string(percent) + "}";
    return true;
}
void BrowserLiveVolume::poll(std::uint64_t now) {
    if ((clock_ && now < *clock_) || (deadline_ && now >= *deadline_))
        close();
    clock_ = now;
    if (!closed_ && deferred_ && pending_) {
        if (auto fact = deferred_->take_volume_applied()) {
            if (fact->token != "live-volume-" + std::to_string(ticket_) ||
                fact->percent != pending_->percent || pcm_.volume() != pending_->percent) {
                close();
                return;
            }
            ai_applied_ = true;
            complete();
        }
    }
}
void BrowserLiveVolume::complete() {
    if (!closed_ && pending_ && live_applied_ && ai_applied_) {
        applied_ = std::move(pending_);
        pending_.reset();
        deadline_.reset();
    }
}
bool BrowserLiveVolume::receive(std::string_view body) {
    if (!ready_ || closed_ || !pending_ || applied_ || command_ || body.size() > 256)
        return false;
    std::string canonical;
    if (canonical_json_object(body, canonical) != DecodeResult::Ok)
        return false;
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(ur::device::parse_json(canonical.c_str()),
                                                         cJSON_Delete);
    if (!root || cJSON_GetArraySize(root.get()) != 3)
        return false;
    const auto* kind = cJSON_GetObjectItemCaseSensitive(root.get(), "kind");
    const auto* ticket = cJSON_GetObjectItemCaseSensitive(root.get(), "volumeTicket");
    const auto* percent = cJSON_GetObjectItemCaseSensitive(root.get(), "percent");
    if (!cJSON_IsString(kind) || std::string_view(kind->valuestring) != "liveVolumeApplied" ||
        !cJSON_IsString(ticket) ||
        std::string_view(ticket->valuestring) != std::to_string(ticket_) ||
        !cJSON_IsNumber(percent) || percent->valuedouble != pending_->percent)
        return false;
    if (!deferred_ && (!pcm_.set_volume(pending_->percent) || pcm_.volume() != pending_->percent))
        return false;
    live_applied_ = true;
    if (!deferred_)
        ai_applied_ = true;
    complete();
    return true;
}
std::optional<VolumeApplied> BrowserLiveVolume::take_volume_applied() {
    auto result = std::move(applied_);
    applied_.reset();
    return result;
}
std::optional<std::string> BrowserLiveVolume::take_command() {
    auto result = std::move(command_);
    command_.reset();
    return result;
}
void BrowserLiveVolume::close() {
    closed_ = true;
    ready_ = false;
    command_.reset();
}
}  // namespace ur::device
