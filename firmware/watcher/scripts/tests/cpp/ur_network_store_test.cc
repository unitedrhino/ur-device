// 编译真实 NVS 适配器，覆盖部分写入、恢复重试、日志损坏和非凭据数据保护。
#include "ur_network_store.h"
#include <cassert>
using namespace watcher;

// Wifi 创建非生产测试网络，不使用真实凭据。
static SavedWifi Wifi(const char* name) {
    SavedWifi wifi{};
    std::strcpy(wifi.ssid, name);
    wifi.channel = 6;
    return wifi;
}

// main 每个写入位置故障后均必须能恢复，并保持设备身份命名空间不变。
int main() {
    NetworkStore store;
    NetworkRecord record{};
    bool exists = true;
    assert(store.Load(record, exists) && !exists);
    std::strcpy(record.request_id, "0123456789abcdef");
    record.stage = NetworkStage::Committing;
    record.candidate = Wifi("candidate");
    record.previous[0] = Wifi("old-a");
    record.previous[1] = Wifi("old-b");
    record.previous_count = 2;
    assert(store.Save(record));
    assert(store.RestorePrevious(record));
    fake_nvs::data["wifi"]["force_ap"] = uint8_t(0);
    fake_nvs::data["ur_dev"]["sentinel"] = std::string("unchanged");
    const auto baseline = fake_nvs::data;
    for (int fault = 0; fault <= 30; ++fault) {
        fake_nvs::data = baseline;
        fake_nvs::fail_after = fault;
        assert(!store.CommitCandidate(record));
        fake_nvs::fail_after = -1;
        NetworkRecord loaded{};
        assert(store.Load(loaded, exists) && exists);
        assert(loaded.stage == NetworkStage::Committing);
        assert(store.RestorePrevious(loaded));
        SavedWifi actual[10]{};
        uint8_t count = 0;
        assert(store.ReadWifi(actual, count) && count == 2);
        assert(std::strcmp(actual[0].ssid, "old-a") == 0);
        assert(std::strcmp(actual[1].ssid, "old-b") == 0);
        assert(fake_nvs::data["ur_dev"] == baseline.at("ur_dev"));
        assert(std::get<uint8_t>(fake_nvs::data["wifi"]["force_ap"]) == 0);
        assert(fake_nvs::handles.empty());
    }
    assert(store.CommitCandidate(record));
    SavedWifi actual[10]{};
    uint8_t count = 0;
    assert(store.ReadWifi(actual, count) && count == 3);
    assert(std::strcmp(actual[0].ssid, "candidate") == 0);
    // 恢复本身也可能断电；完整旧快照必须一直保留以支持再次恢复。
    const auto partially_committed = fake_nvs::data;
    for (int fault = 0; fault <= 30; ++fault) {
        fake_nvs::data = partially_committed;
        fake_nvs::fail_after = fault;
        assert(!store.RestorePrevious(record));
        fake_nvs::fail_after = -1;
        NetworkRecord loaded{};
        assert(store.Load(loaded, exists) && exists);
        assert(store.RestorePrevious(loaded));
        assert(store.ReadWifi(actual, count) && count == 2);
        assert(std::strcmp(actual[0].ssid, "old-a") == 0);
        assert(fake_nvs::handles.empty());
    }
    record.candidate = record.previous[1];
    assert(store.CommitCandidate(record));
    assert(store.ReadWifi(actual, count) && count == 2);
    assert(std::strcmp(actual[0].ssid, "old-b") == 0);
    record.stage = NetworkStage::Trial;
    assert(!store.CommitCandidate(record));
    assert(!store.RestorePrevious(record));
    // 错误版本、超限长度必须失败，不允许用默认空配置继续启动。
    auto invalid = record;
    invalid.magic = 0;
    assert(!store.Save(invalid));
    fake_nvs::data["ur_network"]["transaction"] = std::vector<uint8_t>(sizeof(NetworkRecord), 0);
    assert(!store.Load(record, exists));
    fake_nvs::data["ur_network"]["transaction"] = std::vector<uint8_t>{1};
    assert(!store.Load(record, exists));
    fake_nvs::data["wifi"].erase("password");
    assert(!store.ReadWifi(actual, count));
    assert(fake_nvs::handles.empty());
}
