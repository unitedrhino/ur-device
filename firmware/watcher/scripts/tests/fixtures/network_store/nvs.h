// 主机 NVS 故障替身：立即可见的逐键写入模拟最不利断电边界，不代替真机 Flash 测试。
#pragma once
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <variant>
#include <vector>
using nvs_handle_t = unsigned;  // 替身句柄。
using esp_err_t = int;  // 与适配器一致的成功/失败约定。
constexpr int ESP_OK = 0, ESP_ERR_NVS_NOT_FOUND = 1, NVS_READONLY = 0, NVS_READWRITE = 1;
namespace fake_nvs {
using Value = std::variant<std::string, uint8_t, std::vector<uint8_t>>;  // 保留类型错误检测。
inline std::map<std::string, std::map<std::string, Value>> data;  // 模拟持久介质。
inline std::map<unsigned, std::string> handles;  // 跟踪句柄泄漏。
inline unsigned next = 1;  // 下一句柄，不返回无效的零。
inline int fail_after = -1;  // 写入/commit 故障位置，负数不注入。
// Fault 让恰好一个指定持久化操作失败，后续调用可用于恢复。
inline bool Fault() { if (fail_after < 0) return false; return fail_after-- == 0; }
}
// nvs_open 限定生产适配器可触及的两个命名空间。
inline int nvs_open(const char* name, int mode, unsigned* out) {
    if (std::strcmp(name, "wifi") && std::strcmp(name, "ur_network")) return 9;
    if (!mode && !fake_nvs::data.count(name)) return ESP_ERR_NVS_NOT_FOUND;
    fake_nvs::data[name]; *out = fake_nvs::next++; fake_nvs::handles[*out] = name; return 0;
}
// nvs_close 回收句柄而不自动提交。
inline void nvs_close(unsigned h) { fake_nvs::handles.erase(h); }
// nvs_commit 提供独立于 set 的故障点。
inline int nvs_commit(unsigned) { return fake_nvs::Fault() ? 9 : 0; }
// ReadTyped 保留缺失、类型不匹配的不同返回值。
template<class T> inline int ReadTyped(unsigned h, const char* key, T& out) {
    auto& values = fake_nvs::data.at(fake_nvs::handles.at(h));
    auto item = values.find(key);
    if (item == values.end()) return ESP_ERR_NVS_NOT_FOUND;
    auto value = std::get_if<T>(&item->second);
    if (!value) return 9;
    out = *value; return 0;
}
// WriteTyped 模拟单键成功后即持久可见，用于检验多键恢复日志。
template<class T> inline int WriteTyped(unsigned h, const char* key, T value) {
    if (fake_nvs::Fault()) return 9;
    fake_nvs::data.at(fake_nvs::handles.at(h))[key] = value; return 0;
}
// nvs_get_str 遵循含终止符的缓冲长度合同。
inline int nvs_get_str(unsigned h, const char* key, char* out, size_t* size) {
    std::string value; int error = ReadTyped(h, key, value); if (error) return error;
    if (*size < value.size() + 1) return 9;
    *size = value.size() + 1; std::memcpy(out, value.c_str(), *size); return 0;
}
// nvs_set_str 复制输入内容，不借用调用栈。
inline int nvs_set_str(unsigned h, const char* key, const char* value) { return WriteTyped(h, key, std::string(value)); }
// nvs_get_u8 / nvs_set_u8 保留旧信道提示。
inline int nvs_get_u8(unsigned h, const char* key, uint8_t* value) { return ReadTyped(h, key, *value); }
inline int nvs_set_u8(unsigned h, const char* key, uint8_t value) { return WriteTyped(h, key, value); }
// nvs_set_blob 单个日志 blob 原子替换。
inline int nvs_set_blob(unsigned h, const char* key, const void* value, size_t size) {
    const auto* bytes = static_cast<const uint8_t*>(value);
    return WriteTyped(h, key, std::vector<uint8_t>(bytes, bytes + size));
}
// nvs_get_blob 支持先查询大小再读数据。
inline int nvs_get_blob(unsigned h, const char* key, void* out, size_t* size) {
    std::vector<uint8_t> value; int error = ReadTyped(h, key, value); if (error) return error;
    if (out && *size < value.size()) return 9;
    if (out) std::memcpy(out, value.data(), value.size());
    *size = value.size(); return 0;
}
// nvs_erase_key 提供槽位清理中断点，不支持擦除整个命名空间。
inline int nvs_erase_key(unsigned h, const char* key) {
    if (fake_nvs::Fault()) return 9;
    return fake_nvs::data.at(fake_nvs::handles.at(h)).erase(key) ? 0 : ESP_ERR_NVS_NOT_FOUND;
}
