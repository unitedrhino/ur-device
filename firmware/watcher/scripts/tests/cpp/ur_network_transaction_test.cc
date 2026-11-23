// 换网事务故障注入：持久化失败、鉴权/回执关联、全部活动阶段异常重启与边界超时。
#include "ur_network_transaction.h"
#include <cassert>
#include <limits>

// main 使用内存持久化替身模拟断电，不能替代真实 NVS 写入与物理断电验收。
int main() {
    using namespace watcher;
    NetworkRecord disk{};
    bool writable = true;
    auto save = [&](const NetworkRecord& record) { if (!writable) return false; disk = record; return true; };
    NetworkTransaction transaction(save);
    SavedWifi old{"old-network", "old-password", 6};
    SavedWifi candidate{"new-network", "new-password", 0};
    assert(!transaction.Begin("bad", &old, 1, 0));
    writable = false;
    assert(!transaction.Begin("0123456789abcdef", &old, 1, 0));
    assert(transaction.Record().stage == NetworkStage::Idle);
    writable = true;
    assert(transaction.Begin("0123456789abcdef", &old, 1, 0));
    assert(!transaction.Begin("fedcba9876543210", &old, 1, 0));
    assert(transaction.Boot(100));
    assert(transaction.Record().stage == NetworkStage::Ble);
    assert(transaction.AcceptCandidate(candidate, 100));
    assert(std::strcmp(disk.previous[0].ssid, "old-network") == 0);
    assert(transaction.Expired(180100));
    assert(!transaction.Expired(180099));
    assert(!transaction.AcceptCandidate(candidate, 180100));
    assert(!transaction.GotIp(180100));
    assert(transaction.GotIp(101));
    assert(transaction.Record().stage == NetworkStage::TrialReady);
    NetworkTransaction restarted(save);
    assert(restarted.Restore(disk));
    assert(restarted.Boot(0));
    assert(restarted.Record().stage == NetworkStage::Trial);
    assert(!restarted.Expired(89999));
    assert(restarted.Expired(90000));
    assert(!restarted.ConfirmReport("old-token", 200, true, 10));
    assert(restarted.ReportSent("current-token"));
    assert(!restarted.ConfirmReport("current-token", 200, true, 90000));
    assert(!restarted.ConfirmReport("other-token", 200, true, 10));
    assert(!restarted.ConfirmReport("current-token", 500, true, 10));
    assert(!restarted.ConfirmReport("current-token", 200, false, 10));
    restarted.ClearReport();
    assert(!restarted.ConfirmReport("current-token", 200, true, 10));
    assert(restarted.ReportSent("new-connection-token"));
    writable = false;
    assert(!restarted.ConfirmReport("new-connection-token", 200, true, 10));
    assert(restarted.Record().stage == NetworkStage::Trial);
    writable = true;
    assert(restarted.ConfirmReport("new-connection-token", 200, true, 10));
    assert(restarted.Record().stage == NetworkStage::Committing);
    // 写正式列表期间断电，Boot 必须拒绝继续，仍保留旧快照供适配器恢复。
    NetworkTransaction interrupted(save);
    assert(interrupted.Restore(disk));
    assert(!interrupted.Boot(0));
    assert(std::strcmp(interrupted.Record().previous[0].password, "old-password") == 0);
    assert(interrupted.Fail(NetworkError::Restart, 0));
    assert(!interrupted.Active());
    assert(disk.candidate.password[0] == 0 && disk.previous_count == 0);
    // 假定适配器已经写入并回读正式配置，成功终态同样清除事务凭据副本。
    assert(restarted.Complete(20));
    assert(disk.stage == NetworkStage::Committed && disk.candidate.ssid[0] == 0);
    assert(!restarted.Begin("0123456789abcdef", &old, 1, 30));
    // 对各个非受控重启阶段逐项验证，不把历史 Trial / BLE 误当成新窗口。
    for (auto stage : {NetworkStage::Ble, NetworkStage::Candidate, NetworkStage::Trial, NetworkStage::Committing}) {
        NetworkRecord record{};
        record.stage = stage;
        std::strcpy(record.request_id, "1111111111111111");
        record.candidate = candidate;
        record.previous[0] = old; record.previous_count = 1;
        NetworkTransaction booted(save);
        assert(booted.Restore(record));
        assert(!booted.Boot(0));
        assert(booted.Record().stage == stage);
        assert(booted.Fail(NetworkError::Restart, 1));
    }
    NetworkTransaction wrapped(save);
    assert(wrapped.Begin("2222222222222222", &old, 1, 0));
    assert(wrapped.Boot(std::numeric_limits<uint32_t>::max() - 50));
    assert(!wrapped.Expired(100));
    assert(wrapped.Expired(180000));
    NetworkRecord invalid{}; invalid.magic = 0;
    assert(!wrapped.Restore(invalid));
}
