// 换网协调链回放：真实事务/存储协调，仅 NVS 用故障替身；不模拟无线或平台成功。
#include "ur_network_runtime.h"
#include <cassert>
using namespace watcher;

// Seed 放入已有正式网络并返回首次换网快照。
static void Seed() {
    fake_nvs::data.clear();
    fake_nvs::fail_after = -1;
    fake_nvs::data["wifi"]["ssid"] = std::string("old-network");
    fake_nvs::data["wifi"]["password"] = std::string();
}
// Candidate 构造开放测试网络，无真实凭据。
static SavedWifi Candidate() {
    SavedWifi wifi{};
    std::strcpy(wifi.ssid, "new-network");
    return wifi;
}
// AssertWifi 验证真正持久列表首项，不以协调器返回值冒充配置正确。
static void AssertWifi(const char* expected) {
    NetworkStore store;
    SavedWifi wifi[10]{};
    uint8_t count = 0;
    assert(store.ReadWifi(wifi, count) && count > 0);
    assert(std::strcmp(wifi[0].ssid, expected) == 0);
    assert(fake_nvs::handles.empty());
}

// main 覆盖成功链、异常重启、超时、鉴权失败以及终态持久化不确定性。
int main() {
    // 本地换网不需要 MQTT：取得 IP 后可靠保存，保持旧网络和设备密钥。
    Seed();
    fake_nvs::data["ur_dev"]["secret"] = std::string("fixture-only");
    NetworkRuntime local_start;
    assert(local_start.Boot(false, 0) == NetworkBoot::Normal);
    assert(local_start.Begin("abcdef0123456789", 1));
    NetworkRuntime local;
    assert(local.Boot(true, 2) == NetworkBoot::Ble);
    assert(local.CommitConnected("new-network", 3) == NetworkResult::Ignored);
    assert(local.AcceptCandidate(Candidate(), 4));
    const auto local_candidate = fake_nvs::data;
    assert(local.CommitConnected("new-network", 5) == NetworkResult::Committed);
    assert(local.CommitConnected("new-network", 6) == NetworkResult::Ignored);
    AssertWifi("new-network");
    NetworkStore local_store;
    SavedWifi local_list[10]{};
    uint8_t local_count = 0;
    assert(local_store.ReadWifi(local_list, local_count) && local_count == 2);
    assert(std::strcmp(local_list[1].ssid, "old-network") == 0);
    assert(fake_nvs::data["ur_dev"] == local_candidate.at("ur_dev"));

    // 错误 AP、缺失关联及超时 IP 事件不能提交候选，也不能覆盖旧配置。
    for (const char* associated_ssid : {"old-network", static_cast<const char*>(nullptr)}) {
        Seed();
        NetworkRuntime preparing;
        assert(preparing.Boot(false, 0) == NetworkBoot::Normal);
        assert(preparing.Begin("abcdef0123456789", 1));
        NetworkRuntime rejected;
        assert(rejected.Boot(true, 2) == NetworkBoot::Ble);
        assert(rejected.AcceptCandidate(Candidate(), 3));
        assert(rejected.CommitConnected(associated_ssid, 4) == NetworkResult::Rollback);
        assert(rejected.Record().error == NetworkError::Network);
        AssertWifi("old-network");
    }
    Seed();
    NetworkRuntime deadline_start;
    assert(deadline_start.Boot(false, 0) == NetworkBoot::Normal);
    assert(deadline_start.Begin("abcdef0123456789", 1));
    NetworkRuntime deadline;
    assert(deadline.Boot(true, 2) == NetworkBoot::Ble);
    assert(deadline.AcceptCandidate(Candidate(), 3));
    assert(deadline.CommitConnected("new-network", 180002) == NetworkResult::Rollback);
    assert(deadline.Record().error == NetworkError::Timeout);
    AssertWifi("old-network");

    // 保存失败逐点注入：既不能假成功，也不能丢旧网络或改 DeviceSecret。
    for (int fault = 0; fault < 37; ++fault) {
        Seed();
        fake_nvs::data["ur_dev"]["secret"] = std::string("fixture-only");
        NetworkRuntime preparing;
        assert(preparing.Boot(false, 0) == NetworkBoot::Normal);
        assert(preparing.Begin("abcdef0123456789", 1));
        NetworkRuntime attempt;
        assert(attempt.Boot(true, 2) == NetworkBoot::Ble);
        assert(attempt.AcceptCandidate(Candidate(), 3));
        fake_nvs::fail_after = fault;
        assert(attempt.CommitConnected("new-network", 4) != NetworkResult::Committed);
        fake_nvs::fail_after = -1;
        NetworkRuntime recovered_local;
        assert(recovered_local.Boot(false, 0) == NetworkBoot::Normal);
        AssertWifi(recovered_local.Stage() == NetworkStage::Committed ? "new-network" : "old-network");
        assert(fake_nvs::data["ur_dev"] == local_candidate.at("ur_dev"));
    }

    Seed();
    NetworkRuntime start;
    assert(!start.Begin("0123456789abcdef", 0));
    assert(start.Boot(false, 0) == NetworkBoot::Normal);
    assert(start.Begin("0123456789abcdef", 0));
    const auto armed = fake_nvs::data;
    NetworkRuntime ble;
    assert(ble.Boot(true, 10) == NetworkBoot::Ble);
    assert(ble.AcceptCandidate(Candidate(), 11));
    AssertWifi("old-network");
    const auto candidate = fake_nvs::data;
    assert(ble.GotIp(12));
    const auto ready = fake_nvs::data;
    NetworkRuntime trial;
    assert(trial.Boot(true, 20) == NetworkBoot::Trial);
    const auto verifying = fake_nvs::data;
    assert(trial.ReportSent("first"));
    trial.Disconnected();
    assert(trial.ReportReply("first", 200, true, 21) == NetworkResult::Ignored);
    assert(trial.ReportSent("second"));
    assert(trial.ReportReply("second", 500, true, 21) == NetworkResult::Ignored);
    assert(trial.ReportReply("second", 200, false, 21) == NetworkResult::Ignored);
    AssertWifi("old-network");
    assert(trial.ReportReply("second", 200, true, 22) == NetworkResult::Committed);
    AssertWifi("new-network");
    NetworkRuntime done;
    assert(done.Boot(false, 0) == NetworkBoot::Normal);
    assert(done.Stage() == NetworkStage::Committed);
    assert(done.Record().previous_count == 0 && done.Record().candidate.ssid[0] == 0);
    assert(!done.Begin("0123456789abcdef", 0));

    // 每个未完成阶段真实掉电均终止，不借旧 IP/旧在线继续验证。
    for (const auto& snapshot : {armed, candidate, ready, verifying}) {
        fake_nvs::data = snapshot;
        NetworkRuntime interrupted;
        assert(interrupted.Boot(false, 0) == NetworkBoot::Normal);
        assert(interrupted.Stage() == NetworkStage::Failed);
        assert(interrupted.Record().error == NetworkError::Restart);
        AssertWifi("old-network");
    }
    fake_nvs::data = candidate;
    NetworkRuntime unexpected_software_reset;
    assert(unexpected_software_reset.Boot(true, 0) == NetworkBoot::Normal);
    assert(unexpected_software_reset.Stage() == NetworkStage::Failed);

    fake_nvs::data = armed;
    NetworkRuntime timeout_ble;
    assert(timeout_ble.Boot(true, 0) == NetworkBoot::Ble);
    assert(timeout_ble.Tick(179999) == NetworkResult::Continue);
    assert(timeout_ble.Tick(180000) == NetworkResult::Rollback);
    fake_nvs::data = ready;
    NetworkRuntime timeout_trial;
    assert(timeout_trial.Boot(true, 0) == NetworkBoot::Trial);
    assert(timeout_trial.ReportSent("late"));
    assert(timeout_trial.ReportReply("late", 200, true, 90000) == NetworkResult::Rollback);
    AssertWifi("old-network");

    fake_nvs::data = ready;
    NetworkRuntime auth;
    assert(auth.Boot(true, 0) == NetworkBoot::Trial);
    assert(auth.Fail(NetworkError::Auth, 10) == NetworkResult::Rollback);
    AssertWifi("old-network");

    // Confirm 两次写、Wi-Fi 三十次写+commit、排序重置两次、Complete 两次写：逐个失败。
    // 无论中间返回什么，下一次异常启动都不得留下半份 Wi-Fi 或损坏日志。
    for (int fault = 0; fault < 37; ++fault) {
        fake_nvs::data = ready;
        fake_nvs::fail_after = -1;
        NetworkRuntime failing;
        assert(failing.Boot(true, 0) == NetworkBoot::Trial);
        assert(failing.ReportSent("proof"));
        fake_nvs::fail_after = fault;
        assert(failing.ReportReply("proof", 200, true, 1) != NetworkResult::Committed);
        fake_nvs::fail_after = -1;
        NetworkRuntime recovered;
        assert(recovered.Boot(false, 0) == NetworkBoot::Normal);
        // 终态 commit 返回错误但 blob 已生效时保留已经确认并完整写入的新网。
        AssertWifi(recovered.Stage() == NetworkStage::Committed ? "new-network" : "old-network");
    }
    // 存储不可用不能误走首次配网或正常启动；恢复失败不得丢掉旧快照。
    fake_nvs::data = ready;
    NetworkRuntime committing;
    assert(committing.Boot(true, 0) == NetworkBoot::Trial);
    assert(committing.ReportSent("recover"));
    fake_nvs::fail_after = 35;  // 正式配置及排序已提交，终态 blob 写入失败。
    assert(committing.ReportReply("recover", 200, true, 1) == NetworkResult::Halt);
    fake_nvs::fail_after = 0;
    NetworkRuntime halted;
    assert(halted.Boot(false, 0) == NetworkBoot::Halt);
    assert(!halted.Begin("fedcba9876543210", 0));
    fake_nvs::fail_after = -1;
    NetworkStore store;
    NetworkRecord retained{};
    bool exists = false;
    assert(store.Load(retained, exists) && exists);
    assert(retained.stage == NetworkStage::Committing && retained.previous_count == 1);
    NetworkRuntime retry;
    assert(retry.Boot(false, 0) == NetworkBoot::Normal);
    AssertWifi("old-network");
}
