// Watcher 扫描排序：最近成功网络优先，同一 SSID 保持既有信号强度顺序。
#pragma once
#include "ur_network.h"
#include <algorithm>
#include <cstring>

namespace watcher {
// CanUseSavedWifiChannels 仅单网络保留快速扫描；多网络的缓存信道可能遗漏新热点。
// 完整扫描后才可按最近顺序选择，不能让旧热点可见就阻止发现其他已保存网络。
inline bool CanUseSavedWifiChannels() {
    char order[10][33]{};
    return ur_network_copy_wifi_order(order) <= 1;
}

// PrioritizeWifiScan 在 SDK 按 RSSI 排序后调用；无保存顺序时不改变原结果。
// ApRecord 只需提供以零结束的 ssid 字段，不依赖 ESP-IDF 类型，便于回放测试。
template <class ApRecord>
void PrioritizeWifiScan(ApRecord* records, uint16_t count) {
    if (!records || count < 2) return;
    char order[10][33]{};
    const auto saved = ur_network_copy_wifi_order(order);
    if (saved == 0 || saved > 10) return;
    const auto rank = [&order, saved](const ApRecord& record) {
        for (uint8_t i = 0; i < saved; ++i)
            if (std::strncmp(reinterpret_cast<const char*>(record.ssid), order[i], 33) == 0) return i;
        return saved;
    };
    std::stable_sort(records, records + count, [&rank](const ApRecord& a, const ApRecord& b) {
        return rank(a) < rank(b);
    });
}
}  // namespace watcher
