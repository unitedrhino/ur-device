// 换网纯状态机：持久化快照先于副作用，候选验证与正式 Wi-Fi 提交分离。
// 存储适配器必须原子保存 Record；正式配置仅能在 Committing 阶段写入。
#pragma once

#include <cstdint>
#include <cstring>
#include <functional>
#include <utility>

namespace watcher {

// NetworkStage 的数字属于持久化合同；新增值只能追加，不得重排。
enum class NetworkStage : uint8_t {
    Idle, Armed, Ble, Candidate, TrialReady, Trial, Committing, Committed, Failed
};
// NetworkError 不携带 SSID/密码/密钥，可安全用于属性状态与 UI。
enum class NetworkError : uint8_t { None, Restart, Timeout, Cancelled, Auth, Network, Storage };

// SavedWifi 保存正式配置的恢复快照，不包含 DeviceSecret 或云端绑定信息。
struct SavedWifi {
    char ssid[33]{};  // UTF-8 SSID 最多 32 字节。
    char password[65]{};  // WPA 密码或 64 位十六进制 PSK。
    uint8_t channel = 0;  // 原有信道提示，0 表示未知。
};

// NetworkRecord 持久化完整事务；magic/version/长度由 NVS 适配器验证后才能恢复。
struct NetworkRecord {
    uint32_t magic = 0x55524e31;  // URN1，标识当前结构版本。
    NetworkStage stage = NetworkStage::Idle;  // 已持久化的阶段。
    NetworkError error = NetworkError::None;  // 非敏感终态原因。
    char request_id[17]{};  // 16 位十六进制关联 ID，BLE 中编码为 8 字节。
    SavedWifi candidate{};  // 独立候选，不得提前写入正式列表。
    SavedWifi previous[10]{};  // 覆盖正式列表最大容量，支持提交中断恢复。
    uint8_t previous_count = 0;  // 有效旧配置数量。
};

// NetworkTransaction 由拥有者串行调用；Save 失败时不推进内存阶段。
class NetworkTransaction {
public:
    // Save 原子保存完整快照；返回 false 必须阻止后续重启、写正式配置等副作用。
    using Save = std::function<bool(const NetworkRecord&)>;
    explicit NetworkTransaction(Save save) : save_(std::move(save)) {}

    // Restore 仅恢复已校验快照；Boot 再决定本次启动是否允许继续。
    bool Restore(const NetworkRecord& record) {
        if (record.magic != 0x55524e31 || record.previous_count > 10 ||
            record.stage > NetworkStage::Failed || record.error > NetworkError::Storage) return false;
        if (record.stage != NetworkStage::Idle && !ValidId(record.request_id)) return false;
        if (record.stage >= NetworkStage::Candidate && record.stage <= NetworkStage::Committing &&
            !ValidWifi(record.candidate)) return false;
        for (uint8_t i = 0; i < record.previous_count; ++i) if (!ValidWifi(record.previous[i])) return false;
        record_ = record;
        ClearReport();
        entered_at_ = 0;
        return true;
    }

    // Begin 只能在物理确认和互斥校验后调用；重复请求不重新开启窗口。
    bool Begin(const char* request_id, const SavedWifi* previous, uint8_t count, uint32_t now) {
        if (!ValidId(request_id) || count > 10 || (count && !previous) || Active()) return false;
        if (std::strcmp(record_.request_id, request_id) == 0) return false;
        NetworkRecord next{};
        next.stage = NetworkStage::Armed;
        std::memcpy(next.request_id, request_id, 17);
        next.previous_count = count;
        for (uint8_t i = 0; i < count; ++i) {
            if (!ValidWifi(previous[i])) return false;
            next.previous[i] = previous[i];
        }
        return Persist(next, now);
    }

    // Boot 仅 Armed/TrialReady 属于受控重启；其他活动阶段必须先恢复旧配置再终止。
    bool Boot(uint32_t now) {
        if (record_.stage == NetworkStage::Armed) return Move(NetworkStage::Ble, now);
        if (record_.stage == NetworkStage::TrialReady) return Move(NetworkStage::Trial, now);
        // 不能在这里直接写 Failed，否则丢失 Committing 恢复旧列表的依据。
        return !Active();
    }

    // AcceptCandidate BLE 受理成功仍不写正式 Wi-Fi，窗口总时限不因重复报文延长。
    bool AcceptCandidate(const SavedWifi& wifi, uint32_t now) {
        if ((record_.stage != NetworkStage::Ble && record_.stage != NetworkStage::Candidate) ||
            !ValidWifi(wifi) || Expired(now)) return false;
        auto next = record_;
        next.candidate = wifi;
        next.stage = NetworkStage::Candidate;
        return Persist(next, entered_at_);
    }

    // ConfirmWifi 在候选取得 IP 后记录提交意图；正式列表仍由存储层写入并回读。
    bool ConfirmWifi(uint32_t now) {
        return record_.stage == NetworkStage::Candidate && !Expired(now) && Move(NetworkStage::Committing, now);
    }

    // GotIp 保留旧开发版本的平台验证路径；新 BLE worker 使用 ConfirmWifi 本地提交。
    bool GotIp(uint32_t now) {
        return record_.stage == NetworkStage::Candidate && !Expired(now) && Move(NetworkStage::TrialReady, now);
    }

    // ReportSent 保存当前连接中的上报关联；断线必须 ClearReport，历史回执不可提交。
    bool ReportSent(const char* token) {
        if (record_.stage != NetworkStage::Trial || !token || !token[0] ||
            strnlen(token, sizeof(report_token_)) >= sizeof(report_token_)) return false;
        std::strcpy(report_token_, token);
        return true;
    }
    // ClearReport 丢弃旧连接或失败发布对应的提交资格。
    void ClearReport() { report_token_[0] = '\0'; }

    // ConfirmReport 同时要求当前连接和匹配成功回执，先持久化提交意图再允许写正式列表。
    bool ConfirmReport(const char* token, int code, bool connected, uint32_t now) {
        if (record_.stage != NetworkStage::Trial || Expired(now) || !connected || code != 200 || !token ||
            !report_token_[0] || std::strcmp(token, report_token_) != 0) return false;
        if (!Move(NetworkStage::Committing, now)) return false;
        ClearReport();
        return true;
    }

    // Complete 只能在正式配置写入并回读一致后调用，成功后清掉候选/旧配置副本。
    bool Complete(uint32_t now) {
        if (record_.stage != NetworkStage::Committing) return false;
        auto next = record_;
        next.stage = NetworkStage::Committed;
        ClearCredentials(next);
        return Persist(next, now);
    }

    // Fail 由适配器完成必要的旧配置恢复后调用；失败持久化不丢失恢复所需快照。
    bool Fail(NetworkError error, uint32_t now) {
        if (!Active() || error == NetworkError::None) return false;
        auto next = record_;
        next.stage = NetworkStage::Failed;
        next.error = error;
        ClearCredentials(next);
        if (!Persist(next, now)) return false;
        ClearReport();
        return true;
    }

    // Expired 根据启动时的本地时钟计时；进入候选状态不会重置 180 秒总窗口。
    bool Expired(uint32_t now) const {
        if (record_.stage == NetworkStage::Ble || record_.stage == NetworkStage::Candidate)
            return uint32_t(now - entered_at_) >= 180000;
        if (record_.stage == NetworkStage::Trial) return uint32_t(now - entered_at_) >= 90000;
        return false;
    }
    // Active 覆盖整个不可并行的换网窗口，用来屏蔽 OTA、相机和密钥自愈。
    bool Active() const { return record_.stage >= NetworkStage::Armed && record_.stage <= NetworkStage::Committing; }
    // Record 返回只读快照供硬件适配器实施当前阶段；不得用于日志。
    const NetworkRecord& Record() const { return record_; }

private:
    // ValidId 固定长度且只允许十六进制，适合小 MTU 与 JSON 安全编码。
    static bool ValidId(const char* id) {
        if (!id || strnlen(id, 17) != 16) return false;
        for (size_t i = 0; i < 16; ++i)
            if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f'))) return false;
        return true;
    }
    // ValidWifi 校验定长结构中的终止符，不读取数组之外的内存。
    static bool ValidWifi(const SavedWifi& wifi) {
        return wifi.ssid[0] && strnlen(wifi.ssid, sizeof(wifi.ssid)) < sizeof(wifi.ssid) &&
               strnlen(wifi.password, sizeof(wifi.password)) < sizeof(wifi.password);
    }
    // ClearCredentials 终态记录只保留关联 ID、阶段及非敏感错误。
    static void ClearCredentials(NetworkRecord& record) {
        record.candidate = SavedWifi{};
        for (auto& wifi : record.previous) wifi = SavedWifi{};
        record.previous_count = 0;
    }
    // Move 复制快照后推进阶段，存储失败时旧快照仍可恢复。
    bool Move(NetworkStage stage, uint32_t now) {
        auto next = record_; next.stage = stage;
        return Persist(next, now);
    }
    // Persist 明确把持久化成功作为内存状态改变的先决条件。
    bool Persist(const NetworkRecord& next, uint32_t now) {
        if (!save_(next)) return false;
        record_ = next; entered_at_ = now;
        return true;
    }
    Save save_;  // 持久化边界，由 NVS 或故障注入替身提供。
    NetworkRecord record_{};  // 唯一事务快照。
    uint32_t entered_at_ = 0;  // 本次启动内阶段起始时刻，不跨重启复用。
    char report_token_[64]{};  // 仅当前连接的属性上报 token，永不持久化。
};

}  // namespace watcher
