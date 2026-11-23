// 换网运行协调：把持久化、启动原因和平台回执串成同一事务。
// 调用方串行访问；返回 Halt 时不得继续普通联网、擦密钥或启动首次配网。
#pragma once

#include "ur_network_store.h"

namespace watcher {

// NetworkBoot 明确区分正常联网、独占 BLE、候选联网和存储故障停机。
enum class NetworkBoot { Normal, Ble, Trial, Halt };
// NetworkResult 让硬件层区分忽略、继续、已提交、需要回旧网与禁止副作用。
enum class NetworkResult { Ignored, Continue, Committed, Rollback, Halt };

// NetworkRuntime 拥有唯一事务和存储适配器，不拥有 Wi-Fi/MQTT 或 UI 资源。
class NetworkRuntime {
public:
    // 构造后必须先 Boot；未初始化成功前所有外部请求均拒绝。
    NetworkRuntime() : transaction_([this](const NetworkRecord& record) { return store_.Save(record); }) {}
    NetworkRuntime(const NetworkRuntime&) = delete;
    NetworkRuntime& operator=(const NetworkRuntime&) = delete;

    // Boot 在 SsidManager 首次读取前调用；只有软件受控重启可延续 Armed/TrialReady。
    NetworkBoot Boot(bool controlled_restart, uint32_t now) {
        if (initialized_) return NetworkBoot::Halt;
        NetworkRecord record{};
        bool exists = false;
        if (!store_.Load(record, exists) || (exists && !transaction_.Restore(record))) return NetworkBoot::Halt;
        if (transaction_.Active()) {
            if (controlled_restart &&
                (record.stage == NetworkStage::Armed || record.stage == NetworkStage::TrialReady)) {
                if (!transaction_.Boot(now)) return NetworkBoot::Halt;
            } else {
                // 提交可能只写了一部分键，旧快照必须恢复完才允许丢弃日志。
                if (record.stage == NetworkStage::Committing && !store_.RestorePrevious(record))
                    return NetworkBoot::Halt;
                if (!transaction_.Fail(NetworkError::Restart, now)) return NetworkBoot::Halt;
            }
        }
        initialized_ = true;
        if (Stage() == NetworkStage::Ble) return NetworkBoot::Ble;
        if (Stage() == NetworkStage::Trial) return NetworkBoot::Trial;
        return NetworkBoot::Normal;
    }

    // Begin 由物理确认后调用，互斥由拥有者检查；任何存储失败均不授权重启。
    bool Begin(const char* request_id, uint32_t now) {
        if (!initialized_ || transaction_.Active()) return false;
        SavedWifi previous[10]{};
        uint8_t count = 0;
        return store_.ReadWifi(previous, count) && transaction_.Begin(request_id, previous, count, now);
    }

    // AcceptCandidate 仅在独占 BLE 启动内接收，不写正式列表。
    bool AcceptCandidate(const SavedWifi& wifi, uint32_t now) {
        return initialized_ && transaction_.AcceptCandidate(wifi, now);
    }
    // CommitConnected 只由取得候选 IP 的 BLE worker 调用，不依赖 MQTT 或平台可达性。
    // 保存意图、正式列表及成功终态全部完成后才允许向手机发送联网成功。
    NetworkResult CommitConnected(const char* connected_ssid, uint32_t now) {
        if (!initialized_) return NetworkResult::Halt;
        if (Stage() != NetworkStage::Candidate) return NetworkResult::Ignored;
        if (!connected_ssid || std::strcmp(connected_ssid, Record().candidate.ssid) != 0)
            return Fail(NetworkError::Network, now);
        if (transaction_.Expired(now)) return Fail(NetworkError::Timeout, now);
        if (!transaction_.ConfirmWifi(now)) {
            initialized_ = false;
            return NetworkResult::Halt;
        }
        if (!store_.CommitCandidate(transaction_.Record())) return Fail(NetworkError::Storage, now);
        if (!transaction_.Complete(now)) { initialized_ = false; return NetworkResult::Halt; }
        return NetworkResult::Committed;
    }

    // GotIp 保留旧开发版本的兼容入口，新 worker 不再发起平台验证重启。
    bool GotIp(uint32_t now) { return initialized_ && transaction_.GotIp(now); }
    // ReportSent 仅保存已成功发布的本次属性 token。
    bool ReportSent(const char* token) { return initialized_ && transaction_.ReportSent(token); }
    // Disconnected 使前一连接的成功回执失效。
    void Disconnected() { transaction_.ClearReport(); }

    // ReportReply 同步落盘提交意图、写正式列表、回读及终态，只有全部成功才返回 Committed。
    NetworkResult ReportReply(const char* token, int code, bool connected, uint32_t now) {
        if (!initialized_ || Stage() != NetworkStage::Trial) return NetworkResult::Ignored;
        if (transaction_.Expired(now)) return Fail(NetworkError::Timeout, now);
        if (!transaction_.ConfirmReport(token, code, connected, now)) return NetworkResult::Ignored;
        if (!store_.CommitCandidate(transaction_.Record())) return Fail(NetworkError::Storage, now);
        // Complete 的 commit 失败可能已写入终态 blob；不要再反向恢复旧网造成终态与 Wi-Fi 不一致。
        // 停止业务并让下次启动根据实际持久日志决定：Committed 用新网，Committing 恢复旧网。
        if (!transaction_.Complete(now)) { initialized_ = false; return NetworkResult::Halt; }
        return NetworkResult::Committed;
    }

    // Tick 必须由独立定时器/worker 驱动，不能依赖可能阻塞的 MQTT 重连循环。
    NetworkResult Tick(uint32_t now) {
        if (!initialized_) return NetworkResult::Halt;
        return transaction_.Expired(now) ? Fail(NetworkError::Timeout, now) : NetworkResult::Continue;
    }

    // Fail 不清 DeviceSecret；Committing 先恢复旧列表，失败则保留完整日志并禁止普通启动。
    NetworkResult Fail(NetworkError error, uint32_t now) {
        if (!initialized_ || error == NetworkError::None) return NetworkResult::Halt;
        if (!transaction_.Active()) return NetworkResult::Ignored;
        if (Stage() == NetworkStage::Committing && !store_.RestorePrevious(transaction_.Record())) {
            initialized_ = false;
            return NetworkResult::Halt;
        }
        if (!transaction_.Fail(error, now)) { initialized_ = false; return NetworkResult::Halt; }
        return NetworkResult::Rollback;
    }

    // RememberConnection 仅接受正常运行时已保存网络的成功连接，不记录候选或陌生 SSID。
    bool RememberConnection(const char* ssid) {
        if (!initialized_ || Active() || !ssid) return false;
        SavedWifi entries[10]{};
        uint8_t count = 0;
        if (!store_.ReadWifi(entries, count)) return false;
        for (uint8_t i = 0; i < count; ++i)
            if (std::strcmp(entries[i].ssid, ssid) == 0) return WifiRecency().Remember(ssid);
        return false;
    }

    // Active 与 Stage 供拥有者互斥及模式选择，不公开可变事务对象。
    bool Active() const { return transaction_.Active(); }
    NetworkStage Stage() const { return transaction_.Record().stage; }
    // Record 只用于复制状态/候选，含凭据，严禁日志输出。
    const NetworkRecord& Record() const { return transaction_.Record(); }

private:
    NetworkStore store_;  // 真实 NVS 适配器。
    NetworkTransaction transaction_;  // 单一状态机；Save 捕获本对象，禁止复制。
    bool initialized_ = false;  // 存储异常后关断后续操作。
};

}  // namespace watcher
