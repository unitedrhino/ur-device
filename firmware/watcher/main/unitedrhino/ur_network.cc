// 换网 C 桥接：互斥保护唯一运行协调器，MQTT 回调仅递增连接代次。
#include "ur_network.h"
#include "ur_network_runtime.h"
#include <atomic>
#include <mutex>
#include <esp_attr.h>
#include <esp_system.h>
#include <esp_timer.h>

namespace {
std::mutex network_mutex;  // BLE/MQTT/启动 worker 的短期串行化边界。
watcher::NetworkRuntime runtime;  // 唯一事务，不创建第二 MQTT 或 Wi-Fi 管理器。
bool blocked = false;  // 不可确定存储失败必须阻止后续凭据副作用。
bool booted = false;  // 防止重复初始化消费重启标记。
std::atomic<uint32_t> connection_epoch{0};  // 断线回调不等待 NVS 锁。
uint32_t report_epoch = 0;  // 上报成功时对应的连接代次。
RTC_NOINIT_ATTR uint32_t restart_marker;  // 仅软件复位可消费，冷启动不得信任残留值。
constexpr uint32_t kRestartMarker = 0x554e5253;  // URNS 一次性受控重启标记。
constexpr uint32_t kBindingMarker = 0x554e4244;  // UNBD，仅下一次软件启动开启绑定窗口。
char pending_request[17]{};  // 尚未物理确认的请求，仅保存在 RAM。
uint8_t pending_state = 0;  // 0 无、1 预留、2 已展示、3 已取消；重复请求不覆盖终态。
uint8_t pending_error = 0;  // 取消/超时等非敏感原因。
// Now 使用单调毫秒，状态机按无符号差处理回绕。
uint32_t Now() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }
// Result 把存储故障保留为门禁；不得让调用方误走首次配网。
int Result(watcher::NetworkResult result) {
    if (result == watcher::NetworkResult::Halt) blocked = true;
    return static_cast<int>(result);
}
}

extern "C" {
// ur_network_copy_wifi_order 每轮扫描只读取一次，避免比较器反复访问 NVS。
uint8_t ur_network_copy_wifi_order(char ssids[10][33]) {
    if (!ssids) return 0;
    std::lock_guard<std::mutex> lock(network_mutex);
    std::memset(ssids, 0, 10 * 33);
    if (!booted || blocked || runtime.Active()) return 0;
    watcher::SavedWifi entries[10]{};
    uint8_t count = 0;
    watcher::NetworkStore store;
    if (!store.ReadWifi(entries, count)) return 0;
    for (uint8_t i = 0; i < count; ++i) std::strcpy(ssids[i], entries[i].ssid);
    for (auto& entry : entries) entry = watcher::SavedWifi{};
    return count;
}

// ur_network_remember_connection 在正常取得 IP 后记录最近顺序；失败不清凭据或阻止联网。
bool ur_network_remember_connection(const char* ssid) {
    std::lock_guard<std::mutex> lock(network_mutex);
    return booted && !blocked && runtime.RememberConnection(ssid);
}

// ur_network_boot 在首次联网前恢复事务，只接受一次受控软件重启标记。
ur_network_boot_t ur_network_boot(void) {
    std::lock_guard<std::mutex> lock(network_mutex);
    if (booted) return UR_NETWORK_HALT;
    booted = true;
    bool binding = esp_reset_reason() == ESP_RST_SW && restart_marker == kBindingMarker;
    bool controlled = esp_reset_reason() == ESP_RST_SW && (restart_marker == kRestartMarker || binding);
    restart_marker = 0;
    auto mode = runtime.Boot(controlled, Now());
    blocked = mode == watcher::NetworkBoot::Halt;
    if (binding && mode == watcher::NetworkBoot::Ble) return UR_NETWORK_BIND;
    return static_cast<ur_network_boot_t>(mode);
}
// ur_network_begin 保存请求与旧配置快照，成功才允许调用受控重启。
bool ur_network_begin(const char* request_id) {
    std::lock_guard<std::mutex> lock(network_mutex);
    if (!booted || blocked || !request_id) return false;
    if ((pending_state == 1 || pending_state == 2) &&
        (pending_state != 2 || std::strcmp(request_id, pending_request) != 0)) return false;
    if (!runtime.Begin(request_id, Now())) return false;
    pending_state = 0;
    return true;
}
// ur_network_prepare_request 验证请求 ID 并预留唯一确认槽，不写候选日志。
int ur_network_prepare_request(const char* request_id) {
    if (!request_id || strnlen(request_id, 17) != 16) return 400;
    for (unsigned i = 0; i < 16; ++i)
        if (!((request_id[i] >= '0' && request_id[i] <= '9') ||
              (request_id[i] >= 'a' && request_id[i] <= 'f'))) return 400;
    std::lock_guard<std::mutex> lock(network_mutex);
    if (!booted) return 501;
    if (blocked || runtime.Active()) return 423;
    if (pending_state && std::strcmp(request_id, pending_request) == 0)
        return pending_state == 3 ? 409 : 200;
    if (pending_state == 1 || pending_state == 2) return 409;
    if (std::strcmp(request_id, runtime.Record().request_id) == 0) return 409;
    std::memcpy(pending_request, request_id, 17);
    pending_state = 1;
    pending_error = 0;
    return 200;
}
// ur_network_activate_request 只首次通过回执发送门禁，重复报文不能重开确认页。
bool ur_network_activate_request(const char* request_id) {
    std::lock_guard<std::mutex> lock(network_mutex);
    if (!request_id || pending_state != 1 || std::strcmp(request_id, pending_request) != 0) return false;
    pending_state = 2;
    return true;
}
// ur_network_cancel_request 与当前请求关联，迟到取消不得影响下一请求。
void ur_network_cancel_request(const char* request_id, uint8_t error) {
    std::lock_guard<std::mutex> lock(network_mutex);
    if (!request_id || (pending_state != 1 && pending_state != 2) ||
        std::strcmp(request_id, pending_request) != 0) return;
    // 重复 action 的回复发送失败，不能撤销已经展示的原始确认窗口。
    if (error == UR_NETWORK_ERROR_NETWORK && pending_state == 2) return;
    pending_state = 3;
    pending_error = error;
}
// ur_network_restart_binding 授权下一次软件启动进入绑定事务，冷启动不延续窗口。
void ur_network_restart_binding(void) {
    restart_marker = kBindingMarker;
    esp_restart();
}
// ur_network_restart 授权下一次软件启动延续换网事务，不应用于普通业务重启。
void ur_network_restart(void) {
    restart_marker = kRestartMarker;
    esp_restart();
}
// ur_network_candidate 输出固定长度副本，调用方用后清零。
bool ur_network_candidate(char ssid[33], char password[65]) {
    if (!ssid || !password) return false;
    std::lock_guard<std::mutex> lock(network_mutex);
    if (blocked || runtime.Stage() < watcher::NetworkStage::Candidate ||
        runtime.Stage() > watcher::NetworkStage::Trial) return false;
    std::memcpy(ssid, runtime.Record().candidate.ssid, 33);
    std::memcpy(password, runtime.Record().candidate.password, 65);
    return true;
}
// ur_network_accept 受理 BLE 候选，拒绝超长字符串与错误阶段。
bool ur_network_accept(const char* ssid, const char* password) {
    if (!ssid || !password || strnlen(ssid, 33) >= 33 || strnlen(password, 65) >= 65) return false;
    watcher::SavedWifi wifi{};
    std::strcpy(wifi.ssid, ssid);
    std::strcpy(wifi.password, password);
    std::lock_guard<std::mutex> lock(network_mutex);
    return !blocked && runtime.AcceptCandidate(wifi, Now());
}
// ur_network_got_ip 授权释放蓝牙后重启验证，不写正式配置。
bool ur_network_got_ip(void) {
    std::lock_guard<std::mutex> lock(network_mutex);
    return !blocked && runtime.GotIp(Now());
}
// ur_network_commit_connected 在 BLE worker 串行执行本地提交，不使用云端回执。
int ur_network_commit_connected(const char* connected_ssid) {
    std::lock_guard<std::mutex> lock(network_mutex);
    if (blocked) return UR_NETWORK_FAULT;
    return Result(runtime.CommitConnected(connected_ssid, Now()));
}
// ur_network_protect_credentials 为动态注册、鉴权自愈提供统一否决门禁。
bool ur_network_protect_credentials(void) {
    std::lock_guard<std::mutex> lock(network_mutex);
    return blocked || runtime.Active() || pending_state == 1 || pending_state == 2;
}
// ur_network_report_sent 绑定成功发布 token 和当前连接代次。
void ur_network_report_sent(const char* token) {
    std::lock_guard<std::mutex> lock(network_mutex);
    if (!blocked && runtime.ReportSent(token)) report_epoch = connection_epoch.load();
}
// ur_network_disconnected 可从 MQTT 回调调用，不持有 NVS/业务锁。
void ur_network_disconnected(void) { connection_epoch.fetch_add(1); }
// ur_network_report_reply 过滤旧连接回执并返回提交/恢复决定。
int ur_network_report_reply(const char* token, int code, bool connected) {
    std::lock_guard<std::mutex> lock(network_mutex);
    if (blocked) return UR_NETWORK_FAULT;
    if (report_epoch != connection_epoch.load()) runtime.Disconnected();
    return Result(runtime.ReportReply(token, code, connected, Now()));
}
// ur_network_tick 独立于 MQTT 重连任务检查总时限。
int ur_network_tick(void) {
    std::lock_guard<std::mutex> lock(network_mutex);
    return Result(runtime.Tick(Now()));
}
// ur_network_fail 以非敏感错误结束事务，旧配置恢复失败时保留日志。
int ur_network_fail(uint8_t error) {
    std::lock_guard<std::mutex> lock(network_mutex);
    if (error == 0 || error > static_cast<uint8_t>(watcher::NetworkError::Storage)) return UR_NETWORK_FAULT;
    return Result(runtime.Fail(static_cast<watcher::NetworkError>(error), Now()));
}
// ur_network_status 只复制可供 UI/属性上报的无凭据字段。
void ur_network_status(ur_network_status_t* status) {
    if (!status) return;
    std::lock_guard<std::mutex> lock(network_mutex);
    std::memcpy(status->request_id, runtime.Record().request_id, 17);
    status->stage = static_cast<uint8_t>(runtime.Stage());
    status->error = static_cast<uint8_t>(runtime.Record().error);
    status->blocked = blocked;
    if (pending_state && !runtime.Active()) {
        std::memcpy(status->request_id, pending_request, 17);
        status->stage = pending_state == 3 ? 8 : 9;
        status->error = pending_error;
    }
}
}
