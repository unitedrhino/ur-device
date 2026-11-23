// 换网 NVS 适配：独立事务日志先落盘，正式 Wi-Fi 只在提交/恢复阶段写入。
// 不打开 ur_dev 或 nvsfactory；不清空 wifi 命名空间中的其他设置。
#pragma once

#include "ur_network_transaction.h"
#include "ur_wifi_recency.h"
#include <nvs.h>
#include <cstdio>

namespace watcher {

// NetworkStore 只由换网拥有者串行调用，返回 false 时调用者必须保留日志并停止启动副作用。
class NetworkStore {
public:
    // Load 读取固定版本/长度的原子日志；不存在表示首次使用，损坏绝不能当作空日志。
    bool Load(NetworkRecord& record, bool& exists) {
        exists = false;
        Handle handle;
        auto error = nvs_open("ur_network", NVS_READONLY, &handle.value);
        if (error == ESP_ERR_NVS_NOT_FOUND) return true;
        if (error != ESP_OK) return false;
        size_t length = 0;
        error = nvs_get_blob(handle.value, "transaction", nullptr, &length);
        if (error == ESP_ERR_NVS_NOT_FOUND) return true;
        if (error != ESP_OK || length != sizeof(NetworkRecord)) return false;
        NetworkRecord loaded{};
        if (nvs_get_blob(handle.value, "transaction", &loaded, &length) != ESP_OK ||
            length != sizeof(loaded)) return false;
        NetworkTransaction validator([](const NetworkRecord&) { return false; });
        if (!validator.Restore(loaded)) return false;
        record = loaded;
        exists = true;
        return true;
    }

    // Save 原子替换单个 blob 并 commit；调用者只在成功后改变内存阶段。
    bool Save(const NetworkRecord& record) {
        if (!Valid(record)) return false;
        Handle handle;
        if (nvs_open("ur_network", NVS_READWRITE, &handle.value) != ESP_OK) return false;
        return nvs_set_blob(handle.value, "transaction", &record, sizeof(record)) == ESP_OK &&
               nvs_commit(handle.value) == ESP_OK;
    }

    // ReadWifi 读取既有 SsidManager 的十槽合同；半条记录/类型错误应阻止事务启动。
    bool ReadWifi(SavedWifi (&wifi)[10], uint8_t& count, bool recent_first = true) {
        count = 0;
        for (auto& entry : wifi) entry = SavedWifi{};
        Handle handle;
        auto error = nvs_open("wifi", NVS_READONLY, &handle.value);
        if (error == ESP_ERR_NVS_NOT_FOUND) return true;
        if (error != ESP_OK) return false;
        for (unsigned i = 0; i < 10; ++i) {
            SavedWifi entry{};
            char key[16]{};
            Key(key, "ssid", i);
            size_t length = sizeof(entry.ssid);
            error = nvs_get_str(handle.value, key, entry.ssid, &length);
            if (error == ESP_ERR_NVS_NOT_FOUND) continue;
            if (error != ESP_OK || !entry.ssid[0]) return false;
            Key(key, "password", i);
            length = sizeof(entry.password);
            if (nvs_get_str(handle.value, key, entry.password, &length) != ESP_OK) return false;
            Key(key, "channel", i);
            error = nvs_get_u8(handle.value, key, &entry.channel);
            if (error != ESP_OK && error != ESP_ERR_NVS_NOT_FOUND) return false;
            wifi[count++] = entry;
        }
        // 排序元数据损坏不影响凭据可用性；校验提交时必须读取物理顺序。
        if (recent_first) WifiRecency().Order(wifi, count);
        return true;
    }

    // CommitCandidate 只接受已持久化的 Committing 日志，候选优先且保留其他旧网络。
    bool CommitCandidate(const NetworkRecord& record) {
        if (record.stage != NetworkStage::Committing || !Valid(record)) return false;
        SavedWifi desired[10]{};
        uint8_t count = 1;
        desired[0] = record.candidate;
        for (uint8_t i = 0; i < record.previous_count && count < 10; ++i) {
            if (std::strcmp(record.previous[i].ssid, record.candidate.ssid) != 0)
                desired[count++] = record.previous[i];
        }
        return WriteAndVerify(desired, count) && WifiRecency().Reset();
    }

    // RestorePrevious 在提交异常时幂等恢复旧列表，成功前禁止清除日志中的旧快照。
    bool RestorePrevious(const NetworkRecord& record) {
        if (record.stage != NetworkStage::Committing || !Valid(record)) return false;
        return WriteAndVerify(record.previous, record.previous_count);
    }

private:
    // Handle 确保每个失败分支都释放句柄，不在析构中隐式提交。
    struct Handle {
        nvs_handle_t value = 0;  // IDF 句柄 0 无效。
        ~Handle() { if (value) nvs_close(value); }
        Handle() = default;
        Handle(const Handle&) = delete;
        Handle& operator=(const Handle&) = delete;
    };

    // Valid 复用状态机校验边界，拒绝被破坏的凭据长度与阶段。
    static bool Valid(const NetworkRecord& record) {
        NetworkTransaction validator([](const NetworkRecord&) { return false; });
        return validator.Restore(record);
    }

    // Key 与 SsidManager 保持索引零无后缀的持久化兼容。
    static void Key(char (&key)[16], const char* prefix, unsigned index) {
        if (index == 0) std::snprintf(key, sizeof(key), "%s", prefix);
        else std::snprintf(key, sizeof(key), "%s%u", prefix, index);
    }

    // EraseMissingOk 仅删除指定槽位，不影响 force_ap 等非凭据配置。
    static bool EraseMissingOk(nvs_handle_t handle, const char* key) {
        auto error = nvs_erase_key(handle, key);
        return error == ESP_OK || error == ESP_ERR_NVS_NOT_FOUND;
    }

    // WriteAndVerify 不假设多键 NVS 写入原子；中断后靠尚未清除的 Committing 日志恢复。
    bool WriteAndVerify(const SavedWifi* wifi, uint8_t count) {
        Handle handle;
        if (count > 10 || nvs_open("wifi", NVS_READWRITE, &handle.value) != ESP_OK) return false;
        for (unsigned i = 0; i < 10; ++i) {
            char ssid[16]{}, password[16]{}, channel[16]{};
            Key(ssid, "ssid", i);
            Key(password, "password", i);
            Key(channel, "channel", i);
            if (i < count) {
                if (nvs_set_str(handle.value, ssid, wifi[i].ssid) != ESP_OK ||
                    nvs_set_str(handle.value, password, wifi[i].password) != ESP_OK ||
                    nvs_set_u8(handle.value, channel, wifi[i].channel) != ESP_OK) return false;
            } else if (!EraseMissingOk(handle.value, ssid) || !EraseMissingOk(handle.value, password) ||
                       !EraseMissingOk(handle.value, channel)) return false;
        }
        if (nvs_commit(handle.value) != ESP_OK) return false;
        SavedWifi actual[10]{};
        uint8_t actual_count = 0;
        if (!ReadWifi(actual, actual_count, false) || actual_count != count) return false;
        for (uint8_t i = 0; i < count; ++i) {
            if (std::strcmp(actual[i].ssid, wifi[i].ssid) != 0 ||
                std::strcmp(actual[i].password, wifi[i].password) != 0 ||
                actual[i].channel != wifi[i].channel) return false;
        }
        return true;
    }
};

}  // namespace watcher
