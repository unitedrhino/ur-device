// 最近网络回放：验证排序持久化、十槽淘汰、凭据隔离与提交中断恢复。
#include "ur_network_runtime.h"
#include <cassert>
#include <string>
using namespace watcher;

// Seed 生成十个仅用于测试的开放网络，槽位顺序为 net0 到 net9。
static void Seed() {
    fake_nvs::data.clear();
    fake_nvs::fail_after = -1;
    for (int i = 0; i < 10; ++i) {
        const auto suffix = i ? std::to_string(i) : "";
        fake_nvs::data["wifi"]["ssid" + suffix] = "net" + std::to_string(i);
        fake_nvs::data["wifi"]["password" + suffix] = std::string();
    }
}

// main 用真实适配器和故障 NVS 检查正常重连不写口令，失败不丢既有网络。
int main() {
    Seed();
    const auto credentials = fake_nvs::data.at("wifi");
    NetworkRuntime runtime;
    assert(runtime.Boot(false, 0) == NetworkBoot::Normal);
    assert(!runtime.RememberConnection("unknown"));
    assert(!runtime.RememberConnection(nullptr));
    assert(runtime.RememberConnection("net9"));
    assert(runtime.RememberConnection("net4"));
    assert(fake_nvs::data.at("wifi") == credentials);
    // 已在首项重复取得 IP 时不触发任何持久化写入。
    fake_nvs::fail_after = 0;
    assert(runtime.RememberConnection("net4"));
    assert(fake_nvs::fail_after == 0);
    fake_nvs::fail_after = -1;
    NetworkStore store;
    SavedWifi list[10]{};
    uint8_t count = 0;
    assert(store.ReadWifi(list, count) && count == 10);
    assert(std::strcmp(list[0].ssid, "net4") == 0);
    assert(std::strcmp(list[1].ssid, "net9") == 0);
    assert(std::strcmp(list[9].ssid, "net8") == 0);
    const auto before = fake_nvs::data;
    // 新增第十一项时淘汰最近排序末项，而非原物理槽位末项。
    for (int fault = -1; fault < 37; ++fault) {
        fake_nvs::data = before;
        fake_nvs::fail_after = -1;
        NetworkRuntime start;
        assert(start.Boot(false, 0) == NetworkBoot::Normal);
        assert(start.Begin("0123456789abcdef", 1));
        NetworkRuntime ble;
        assert(ble.Boot(true, 2) == NetworkBoot::Ble);
        SavedWifi candidate{};
        std::strcpy(candidate.ssid, "new");
        assert(ble.AcceptCandidate(candidate, 3));
        assert(!ble.RememberConnection("net0"));
        fake_nvs::fail_after = fault;
        const auto result = ble.CommitConnected("new", 4);
        if (fault == -1) assert(result == NetworkResult::Committed);
        else assert(result != NetworkResult::Committed);
        fake_nvs::fail_after = -1;
        NetworkRuntime boot;
        assert(boot.Boot(false, 0) == NetworkBoot::Normal);
        assert(store.ReadWifi(list, count) && count == 10);
        if (boot.Stage() == NetworkStage::Committed) {
            assert(std::strcmp(list[0].ssid, "new") == 0);
            assert(std::strcmp(list[1].ssid, "net4") == 0);
            assert(std::strcmp(list[2].ssid, "net9") == 0);
            for (const auto& item : list) assert(std::strcmp(item.ssid, "net8") != 0);
        } else {
            assert(std::strcmp(list[0].ssid, "net4") == 0);
            assert(std::strcmp(list[1].ssid, "net9") == 0);
            assert(std::strcmp(list[9].ssid, "net8") == 0);
        }
        assert(fake_nvs::handles.empty());
    }
    // 损坏的排序不能损坏或隐藏 Wi-Fi 凭据，也不能被成功事件静默覆盖。
    Seed();
    fake_nvs::data["ur_network"]["recency"] = std::vector<uint8_t>{1, 2};
    assert(store.ReadWifi(list, count) && count == 10);
    assert(std::strcmp(list[0].ssid, "net0") == 0);
    assert(!WifiRecency().Remember("net0"));
    assert(fake_nvs::data.at("wifi") == credentials);
}
