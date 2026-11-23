// 文件说明：固定源解析与原业务许可；不将焦点mask、平台邀请截止或模型参数视为内容授权。
#include "ur/device/json_parse.hpp"
#include "browser_file_catalog.hpp"
#include <cJSON.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <set>

namespace ur::device {
bool decode_browser_file_catalog(std::string_view json, std::uint64_t now, std::uint64_t wall,
                                 std::vector<BrowserFileBinding>& entries) {
    entries.clear();
    std::string canonical;
    if (!wall || json.size() > 65536 || canonical_json_object(json, canonical) != DecodeResult::Ok)
        return false;
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(ur::device::parse_json(canonical.c_str()),
                                                         cJSON_Delete);
    const auto* array = cJSON_GetObjectItemCaseSensitive(root.get(), "entries");
    if (!root || cJSON_GetArraySize(root.get()) != 1 || !cJSON_IsArray(array) ||
        cJSON_GetArraySize(array) < 1 || cJSON_GetArraySize(array) > 128)
        return false;
    std::vector<BrowserFileBinding> decoded;  // 完整校验之后才提交，不保留部分有效授权。
    for (const auto* item = array->child; item; item = item->next) {
        BrowserFileBinding binding;
        if (!cJSON_IsObject(item) || cJSON_GetArraySize(item) != 7)
            return false;
        const auto text = [&](const char* key, std::string& value) {
            const auto* field = cJSON_GetObjectItemCaseSensitive(item, key);
            if (!cJSON_IsString(field))
                return false;
            value = field->valuestring;
            return true;
        };  // 来源字符串随后统一复核，嵌入NUL/重复键已由公共入口拒绝。
        const auto* audio = cJSON_GetObjectItemCaseSensitive(item, "audio");
        const auto* expires = cJSON_GetObjectItemCaseSensitive(item, "expiresAt");
        const auto* bytes = cJSON_GetObjectItemCaseSensitive(item, "maxBytes");
        if (!text("contentRef", binding.content_ref) || !text("url", binding.url) ||
            !text("origin", binding.origin) || !text("mime", binding.mime) ||
            !cJSON_IsBool(audio) || !cJSON_IsNumber(expires) ||
            !std::isfinite(expires->valuedouble) || expires->valuedouble <= 0 ||
            expires->valuedouble > 9007199254740991.0 ||
            std::floor(expires->valuedouble) != expires->valuedouble || !cJSON_IsNumber(bytes) ||
            !std::isfinite(bytes->valuedouble) || bytes->valuedouble < 1 ||
            bytes->valuedouble > 67108864 || std::floor(bytes->valuedouble) != bytes->valuedouble)
            return false;
        const auto absolute = static_cast<std::uint64_t>(expires->valuedouble);
        if (absolute <= wall || absolute - wall > 3600000 ||
            now > std::numeric_limits<std::uint64_t>::max() - (absolute - wall))
            return false;
        binding.expires_at = now + (absolute - wall);
        binding.max_bytes = static_cast<std::uint32_t>(bytes->valuedouble);
        binding.audio = cJSON_IsTrue(audio);
        decoded.push_back(std::move(binding));
    }
    BrowserFileCatalog validated(decoded, [] { return std::optional<std::string>{}; });
    if (!validated.valid())
        return false;
    entries = std::move(decoded);
    return true;
}

BrowserFileCatalog::BrowserFileCatalog(std::vector<BrowserFileBinding> entries,
                                       BrowserFileParent parent)
    : entries_(std::move(entries)), parent_(std::move(parent)) {
    if (!parent_ || entries_.empty() || entries_.size() > 128)
        return;
    std::set<std::string> references;             // 同引用不能因条目顺序选择不同来源或输出。
    const auto origin = entries_.front().origin;  // 本地桥只允许一个预先配置的HTTPS域或固定资产。
    for (const auto& entry : entries_) {
        if (!entry.session.empty() || !entry.playback_id.empty() || entry.origin != origin ||
            !references.insert(entry.content_ref).second)
            return;
        auto candidate = entry;
        candidate.session = "catalog-validation";
        candidate.playback_id = "catalog-validation";
        if (!valid_browser_file_binding(candidate))
            return;
    }
    valid_ = true;
}

std::optional<BrowserFileResolution> BrowserFileCatalog::resolve(const PlaybackInvite& invite,
                                                                 std::uint64_t now) {
    if (clock_ && now < *clock_)
        fenced_ = true;
    if (!valid_ || fenced_)
        return {};
    clock_ = now;
    const auto parent = parent_();
    if (!parent || parent->empty() || *parent == invite.envelope.session)
        return {};
    const auto found = std::find_if(entries_.begin(), entries_.end(), [&](const auto& entry) {
        return entry.content_ref == invite.content_ref;
    });
    if (found == entries_.end() || now >= found->expires_at)
        return {};
    const bool video = found->mime == "video/mp4" || found->mime == "video/webm";
    std::vector<std::string> outputs;  // 从固定媒体声明取得完整组，不以邀请请求提升音轨权限。
    if (found->audio)
        outputs.push_back("audio");
    if (video)
        outputs.push_back("video");
    auto requested = invite.output_modalities;
    std::sort(requested.begin(), requested.end());
    if (requested != outputs)
        return {};
    auto binding = *found;
    binding.session = invite.envelope.session;
    binding.playback_id = invite.playback_id;
    if (!valid_browser_file_binding(binding))
        return {};
    // 原许可独立于焦点mask；一旦观察到结束/换父/回退/到期，永远不能恢复旧节目许可。
    auto observed = std::make_shared<std::pair<std::uint64_t, bool>>(now, false);
    auto permission = [scope = parent_, parent = *parent, deadline = binding.expires_at,
                       audio = binding.audio, video,
                       observed](bool actual_audio, bool actual_video, std::uint64_t current) {
        const auto live = scope();
        if (observed->second || current < observed->first || current >= deadline ||
            actual_audio != audio || actual_video != video || !live || *live != parent) {
            observed->second = true;
            return false;
        }
        observed->first = current;
        return true;
    };
    return BrowserFileResolution{std::move(binding), std::move(permission)};
}
}  // namespace ur::device
