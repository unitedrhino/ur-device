// 文件说明：本地视频界面只提交固定控制和完整SDP，字段归一化后再分派。
#include "ur/device/json_parse.hpp"
#include "browser_call_wire.hpp"
#include <cJSON.h>
#include <algorithm>
#include <memory>
#include "ur/device/envelope.hpp"

namespace ur::device {
bool decode_call_browser(std::string_view body, CallBrowserRequest& out) {
    out = {};
    std::string canonical;
    if (canonical_json_object(body, canonical) != DecodeResult::Ok)
        return false;
    using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
    Json root(ur::device::parse_json(canonical.c_str()), cJSON_Delete);
    const auto* kind = cJSON_GetObjectItemCaseSensitive(root.get(), "kind");
    if (!cJSON_IsString(kind))
        return false;
    const std::string_view type(kind->valuestring);
    if (type == "callControl") {
        const auto* operation = cJSON_GetObjectItemCaseSensitive(root.get(), "operation");
        if (!cJSON_IsString(operation))
            return false;
        const std::string_view action(operation->valuestring);
        if (action != "cancel" && action != "hold" && action != "resume" && action != "transfer" &&
            action != "cancelTransfer")
            return false;
        if (cJSON_GetArraySize(root.get()) != (action == "transfer" ? 3 : 2))
            return false;
        if (action == "transfer") {
            const auto* target = cJSON_GetObjectItemCaseSensitive(root.get(), "target");
            if (!cJSON_IsString(target))
                return false;
            const std::string_view value(target->valuestring);
            const auto separator = value.find(':');
            if (value.size() < 3 || value.size() > 128 || separator == 0 ||
                separator == std::string_view::npos || separator == value.size() - 1 ||
                value.find_first_of("/+#") != std::string_view::npos ||
                std::any_of(value.begin(), value.end(),
                            [](unsigned char byte) { return byte < 32 || byte == 127; }))
                return false;
            out.target = value;
        }
        out.kind = type;
        out.operation = action;
        return true;
    }
    if (type == "callOutputApplied" || type == "callHoldApplied") {
        const bool output = type == "callOutputApplied";
        const auto string_value = [&](const char* key, std::size_t limit, bool empty) {
            const auto* value = cJSON_GetObjectItemCaseSensitive(root.get(), key);
            if (!cJSON_IsString(value))
                return false;
            const std::string_view text(value->valuestring);
            return (empty || !text.empty()) && text.size() <= limit &&
                   std::none_of(text.begin(), text.end(),
                                [](unsigned char ch) { return ch < 32 || ch == 127; });
        };
        if (cJSON_GetArraySize(root.get()) != (output ? 6 : 8) ||
            !string_value("sessionId", 64, output) || !string_value("peerEpoch", 20, false) ||
            !string_value(output ? "gateTicket" : "ticket", 20, false) ||
            !cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(root.get(), "applied")) ||
            !cJSON_IsBool(
                cJSON_GetObjectItemCaseSensitive(root.get(), output ? "enabled" : "held")))
            return false;
        if (!output && (!string_value("controlToken", 128, false) ||
                        !cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(root.get(), "revision"))))
            return false;
        out.kind = type;
        out.control_body = std::move(canonical);
        return true;
    }
    if (type == "callDial") {
        const auto* target = cJSON_GetObjectItemCaseSensitive(root.get(), "target");
        if (cJSON_GetArraySize(root.get()) != 2 || !cJSON_IsString(target))
            return false;
        const std::string_view value(target->valuestring);
        const auto separator = value.find(':');
        if (value.size() < 3 || value.size() > 128 || separator == 0 ||
            separator == std::string_view::npos || separator == value.size() - 1 ||
            value.find_first_of("/+#") != std::string_view::npos ||
            std::any_of(value.begin(), value.end(),
                        [](unsigned char byte) { return byte < 32 || byte == 127; }))
            return false;
        out.kind = type;
        out.target = value;
        return true;
    }
    if (type == "callOffer") {
        const auto* sdp = cJSON_GetObjectItemCaseSensitive(root.get(), "sdp");
        if (cJSON_GetArraySize(root.get()) != 2 || !cJSON_IsString(sdp))
            return false;
        std::string value(sdp->valuestring);
        if (value.size() < 8 || value.size() > 49152 || value.rfind("v=0\r\n", 0) != 0)
            return false;
        out.kind = type;
        out.sdp = std::move(value);
        return true;
    }
    if (type == "callTrack") {
        const auto* track = cJSON_GetObjectItemCaseSensitive(root.get(), "trackId");
        const auto* state = cJSON_GetObjectItemCaseSensitive(root.get(), "state");
        if (cJSON_GetArraySize(root.get()) != 3 || !cJSON_IsString(track) || !cJSON_IsString(state))
            return false;
        const std::string_view track_id(track->valuestring), actual(state->valuestring);
        if ((track_id != "mic-1" && track_id != "video-1") ||
            (actual != "active" && actual != "paused"))
            return false;
        out.kind = type;
        out.track_id = track_id;
        out.state = actual;
        return true;
    }
    if (type != "ready" && type != "callAccept" && type != "callReject" &&
        type != "callConnected" && type != "callHangup")
        return false;
    if (cJSON_GetArraySize(root.get()) != 1)
        return false;
    out.kind = type;
    return true;
}
}  // namespace ur::device
