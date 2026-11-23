// 链接实际 ur_network.cc，验证 C 入口、RTC 授权、断线隔离与持久化结果。
#include "ur_network.h"
#include "ur_network_store.h"
#include <esp_system.h>
#include <esp_timer.h>
#include <cassert>
#include "ur_wifi_scan_order.h"

// main 每个场景独立进程以模拟冷启动的静态状态，不修改生产私有变量。
int main(int argc, char** argv) {
    assert(argc == 2);
    const std::string scenario = argv[1];
    if (scenario == "recency") {
        fake_nvs::data["wifi"]["ssid"] = std::string("old");
        fake_nvs::data["wifi"]["password"] = std::string();
        fake_nvs::data["wifi"]["ssid1"] = std::string("recent");
        fake_nvs::data["wifi"]["password1"] = std::string();
        assert(ur_network_boot() == UR_NETWORK_NORMAL);
        assert(ur_network_remember_connection("recent"));
        char order[10][33]{};
        assert(ur_network_copy_wifi_order(order) == 2);
        assert(std::strcmp(order[0], "recent") == 0);
        assert(!ur_network_remember_connection("unknown"));
        // 原始结果按信号强度降序；同名接入点仍保持该顺序，陌生热点排末尾。
        struct AccessPoint { char ssid[33]; int signal; };
        AccessPoint points[] = {{"unknown", -10}, {"old", -20}, {"recent", -40}, {"recent", -70}};
        watcher::PrioritizeWifiScan(points, 4);
        assert(std::strcmp(points[0].ssid, "recent") == 0 && points[0].signal == -40);
        assert(std::strcmp(points[1].ssid, "recent") == 0 && points[1].signal == -70);
        assert(std::strcmp(points[2].ssid, "old") == 0);
        assert(std::strcmp(points[3].ssid, "unknown") == 0);
        watcher::PrioritizeWifiScan<AccessPoint>(nullptr, 0);
        assert(ur_network_begin("0123456789abcdef"));
        assert(ur_network_copy_wifi_order(order) == 0);
        assert(!ur_network_remember_connection("old"));
        return 0;
    }
    if (scenario == "confirmation") {
        assert(ur_network_boot() == UR_NETWORK_NORMAL);
        assert(ur_network_prepare_request("bad") == 400);
        assert(ur_network_prepare_request("0123456789abcdef") == 200);
        assert(ur_network_protect_credentials());
        assert(!ur_network_begin("0123456789abcdef")); // 未发布回执，不接受物理确认。
        assert(ur_network_prepare_request("0123456789abcdef") == 200);
        assert(ur_network_prepare_request("fedcba9876543210") == 409);
        assert(ur_network_activate_request("0123456789abcdef"));
        assert(!ur_network_activate_request("0123456789abcdef"));
        ur_network_cancel_request("0123456789abcdef", UR_NETWORK_ERROR_NETWORK);
        ur_network_cancel_request("fedcba9876543210", UR_NETWORK_ERROR_CANCELLED);
        ur_network_status_t pending{};
        ur_network_status(&pending);
        assert(pending.stage == 9);
        ur_network_cancel_request("0123456789abcdef", UR_NETWORK_ERROR_TIMEOUT);
        ur_network_status(&pending);
        assert(pending.stage == 8 && pending.error == UR_NETWORK_ERROR_TIMEOUT);
        assert(!ur_network_protect_credentials());
        assert(ur_network_prepare_request("0123456789abcdef") == 409);
        assert(ur_network_prepare_request("fedcba9876543210") == 200);
        assert(ur_network_activate_request("fedcba9876543210"));
        assert(ur_network_begin("fedcba9876543210"));
        ur_network_status(&pending);
        assert(pending.stage == 1 && std::strcmp(pending.request_id,"fedcba9876543210") == 0);
        return 0;
    }
    watcher::NetworkRecord record{};
    record.stage = watcher::NetworkStage::TrialReady;
    std::strcpy(record.request_id, "0123456789abcdef");
    std::strcpy(record.candidate.ssid, "candidate");
    std::strcpy(record.previous[0].ssid, "original");
    record.previous_count = 1;
    watcher::NetworkStore store;
    assert(store.Save(record));
    fake_nvs::data["wifi"]["ssid"] = std::string("original");
    fake_nvs::data["wifi"]["password"] = std::string();
    if (scenario == "binding" || scenario == "binding-cold") {
        record.stage = watcher::NetworkStage::Armed;
        assert(store.Save(record));
        try { ur_network_restart_binding(); } catch (int) {}
        fake_reset_reason = scenario == "binding-cold" ? ESP_RST_POWERON : ESP_RST_SW;
        assert(ur_network_boot() == (scenario == "binding" ? UR_NETWORK_BIND : UR_NETWORK_NORMAL));
        assert(std::get<std::string>(fake_nvs::data["wifi"]["ssid"]) == "original");
        return 0;
    }
    if (scenario != "unmarked") {
        try { ur_network_restart(); } catch (int) {}
    }
    fake_reset_reason = scenario == "cold" ? ESP_RST_POWERON : ESP_RST_SW;
    if (scenario == "cold" || scenario == "unmarked") {
        assert(ur_network_boot() == UR_NETWORK_NORMAL);
        ur_network_status_t status{};
        ur_network_status(&status);
        assert(status.stage == 8 && status.error == 1);
        assert(!ur_network_protect_credentials());
        return 0;
    }
    assert(ur_network_boot() == UR_NETWORK_TRIAL);
    assert(ur_network_protect_credentials());
    char ssid[33]{}, password[65]{};
    assert(ur_network_candidate(ssid, password));
    assert(std::strcmp(ssid, "candidate") == 0);
    ur_network_report_sent("old-connection");
    ur_network_disconnected();
    assert(ur_network_report_reply("old-connection", 200, true) == 0);
    ur_network_report_sent("current-connection");
    if (scenario == "timeout") {
        fake_time_us = 90000000;
        assert(ur_network_tick() == 3);
    } else if (scenario == "auth") {
        assert(ur_network_fail(4) == 3);
    } else {
        assert(ur_network_report_reply("current-connection", 200, true) == 2);
    }
    ur_network_status_t status{};
    ur_network_status(&status);
    assert(status.stage == (scenario == "success" ? 7 : 8));
    assert(!status.blocked && !ur_network_protect_credentials());
    assert(std::strcmp(status.request_id, "0123456789abcdef") == 0);
    watcher::SavedWifi wifi[10]{};
    uint8_t count = 0;
    assert(store.ReadWifi(wifi, count));
    assert(std::strcmp(wifi[0].ssid, scenario == "success" ? "candidate" : "original") == 0);
}
