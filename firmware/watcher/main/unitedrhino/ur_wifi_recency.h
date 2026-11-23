// Wi-Fi 最近使用顺序：只持久化 SSID 排序，不复制密码，也不重写正式网络槽位。
// 单个 NVS blob 原子替换；存储异常只影响优先级，不得触发清密钥或重新绑定。
#pragma once

#include "ur_network_transaction.h"
#include <nvs.h>
#include <algorithm>

namespace watcher {

// WifiRecency 由网络拥有者串行调用；最多十项，与正式 Wi-Fi 容量一致。
class WifiRecency {
public:
    // Order 将已保存配置按成功使用顺序稳定排序；未知项保留相对顺序放在末尾。
    bool Order(SavedWifi* entries, uint8_t count) {
        if (!entries || count > 10) return false;
        Record record{};
        if (!Read(record)) return false;
        std::stable_sort(entries, entries + count, [&record](const SavedWifi& a, const SavedWifi& b) {
            return Rank(record, a.ssid) < Rank(record, b.ssid);
        });
        return true;
    }

    // Remember 仅在已保存网络取得 IP 后调用；首项重复成功不写 Flash。
    bool Remember(const char* ssid) {
        if (!ssid || !ssid[0] || strnlen(ssid, 33) > 32) return false;
        Record previous{};
        if (!Read(previous)) return false;
        if (previous.count && std::strcmp(previous.ssids[0], ssid) == 0) return true;
        Record next{};
        next.count = 1;
        std::strcpy(next.ssids[0], ssid);
        for (uint8_t i = 0; i < previous.count && next.count < 10; ++i) {
            if (std::strcmp(previous.ssids[i], ssid) != 0)
                std::strcpy(next.ssids[next.count++], previous.ssids[i]);
        }
        return Write(next);
    }

    // Reset 在候选列表可靠写入后清排序；正式槽位此时已按新网络及旧最近顺序排列。
    // 提交中断回滚使用相同排序的旧快照，因此清排序前后断电均不丢旧网顺序。
    bool Reset() { return Write(Record{}); }

private:
    // Record 为独立版本化 blob，不含口令，长度/重复值/终止符均须验证。
    struct Record {
        uint32_t version = 1;
        uint8_t count = 0;
        char ssids[10][33]{};
    };
    // Handle 保证错误路径释放 NVS 句柄，析构不隐式提交。
    struct Handle {
        nvs_handle_t value = 0;
        ~Handle() { if (value) nvs_close(value); }
    };
    // Rank 返回成功顺序，未记录项统一排在已知项之后。
    static uint8_t Rank(const Record& record, const char* ssid) {
        for (uint8_t i = 0; i < record.count; ++i)
            if (std::strcmp(record.ssids[i], ssid) == 0) return i;
        return 10;
    }
    // Read 不把损坏记录当作空表覆盖；不存在则按正式槽位原顺序使用。
    bool Read(Record& record) {
        Handle handle;
        auto error = nvs_open("ur_network", NVS_READONLY, &handle.value);
        if (error == ESP_ERR_NVS_NOT_FOUND) return true;
        if (error != ESP_OK) return false;
        size_t length = sizeof(record);
        error = nvs_get_blob(handle.value, "recency", &record, &length);
        if (error == ESP_ERR_NVS_NOT_FOUND) return true;
        if (error != ESP_OK || length != sizeof(record) || record.version != 1 || record.count > 10)
            return false;
        for (uint8_t i = 0; i < record.count; ++i) {
            if (!record.ssids[i][0] || !std::memchr(record.ssids[i], 0, 33)) return false;
            for (uint8_t j = 0; j < i; ++j)
                if (std::strcmp(record.ssids[i], record.ssids[j]) == 0) return false;
        }
        return true;
    }
    // Write 只更新排序 blob；失败时调用者不得把持久化成功当作已确认。
    bool Write(const Record& record) {
        Handle handle;
        if (nvs_open("ur_network", NVS_READWRITE, &handle.value) != ESP_OK) return false;
        return nvs_set_blob(handle.value, "recency", &record, sizeof(record)) == ESP_OK &&
               nvs_commit(handle.value) == ESP_OK;
    }
};

}  // namespace watcher
