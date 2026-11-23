/* 仅限固定IDF6/Watcher诊断包：驱动状态观测不是空口ACK或端到端送达保证。
 * SDK无回调getter；启用前必须审查应用/依赖及基线ELF没有其他注册者。
 * 全局静态状态避免迟到回调借用会话对象；回调只做有界解析，不分配、等待或打印。 */
#include "ur_udp_wifi_trace.h"
#include "ur_udp_tx_trace_state.h"

#include <esp_log.h>
#include <esp_private/wifi.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>

#include <mutex>

namespace {
/* kTag 为诊断日志标签；日志不含地址、路由、nonce或音频内容。 */
constexpr char kTag[] = "UrUdpTxTrace";
/* state及state_mux只保护有界内存操作，不在临界区调用SDK或日志。 */
UrUdpTxTraceState state;
portMUX_TYPE state_mux = portMUX_INITIALIZER_UNLOCKED;
/* lifecycle_mutex串行注册/注销；registered只在该普通任务互斥保护下读写。 */
std::mutex lifecycle_mutex;
bool registered = false;

/* TxDone 复制数值元数据；回调参数有效期仅限当前调用，不保存传入指针。 */
void TxDone(uint8_t interface, uint8_t* data, uint16_t* size, bool success) {
    if (interface != WIFI_IF_STA)
        return;
    const uint64_t now_us = static_cast<uint64_t>(esp_timer_get_time());
    portENTER_CRITICAL(&state_mux);
    state.Record(data, size == nullptr ? 0 : *size, success, now_us);
    portEXIT_CRITICAL(&state_mux);
}

/* StopLocked 仅由生命周期锁持有者调用，晚到回调先受active=false隔离。 */
void StopLocked() {
    portENTER_CRITICAL(&state_mux);
    state.End();
    const auto stats = state.Stats();
    portEXIT_CRITICAL(&state_mux);
    if (!registered)
        return;
    const esp_err_t result = esp_wifi_set_tx_done_cb(nullptr);
    registered = false;
    /* 最多32条，分四次从普通任务输出；不把队列溢出或未知布局当作全量证据。 */
    for (int batch = 0; batch < 4; ++batch)
        UrUdpWifiTraceFlush();
    // 独立数值快照不改变原END合同；标准结构候选仍不能代替目标路由匹配。
    ESP_LOGI(
        kTag,
        "[UR_WIFI_TX] LAYOUT gen=%lu h24=%lu h26=%lu h30=%lu h32=%lu h36=%lu "
        "qos=%lu addr4=%lu protected=%lu fragmented=%lu amsdu=%lu incomplete=%lu "
        "no_payload=%lu llc_short=%lu snap_ipv4=%lu ip_valid=%lu udp_valid=%lu",
        static_cast<unsigned long>(stats.generation), static_cast<unsigned long>(stats.mac_h24),
        static_cast<unsigned long>(stats.mac_h26), static_cast<unsigned long>(stats.mac_h30),
        static_cast<unsigned long>(stats.mac_h32), static_cast<unsigned long>(stats.mac_h36),
        static_cast<unsigned long>(stats.mac_qos), static_cast<unsigned long>(stats.mac_addr4),
        static_cast<unsigned long>(stats.mac_protected),
        static_cast<unsigned long>(stats.mac_fragmented),
        static_cast<unsigned long>(stats.mac_amsdu),
        static_cast<unsigned long>(stats.mac_incomplete),
        static_cast<unsigned long>(stats.mac_no_payload),
        static_cast<unsigned long>(stats.llc_short), static_cast<unsigned long>(stats.llc_ipv4),
        static_cast<unsigned long>(stats.llc_ip_valid),
        static_cast<unsigned long>(stats.llc_udp_valid));
    ESP_LOGI(
        kTag,
        "[UR_WIFI_TX] END gen=%lu callbacks=%lu matched=%lu dropped=%lu ignored=%lu unregister=%d "
        "null=%lu short=%lu large=%lu ethertype=%lu ip=%lu min=%u max=%u candidate_ipv4=%lu "
        "candidate_80211_data=%lu",
        static_cast<unsigned long>(stats.generation), static_cast<unsigned long>(stats.callbacks),
        static_cast<unsigned long>(stats.matched), static_cast<unsigned long>(stats.queue_dropped),
        static_cast<unsigned long>(stats.ignored_layout), result,
        static_cast<unsigned long>(stats.null_frames),
        static_cast<unsigned long>(stats.short_frames),
        static_cast<unsigned long>(stats.large_frames),
        static_cast<unsigned long>(stats.ethertype_rejected),
        static_cast<unsigned long>(stats.ip_rejected), static_cast<unsigned>(stats.min_frame_size),
        static_cast<unsigned>(stats.max_frame_size),
        static_cast<unsigned long>(stats.candidate_ipv4),
        static_cast<unsigned long>(stats.candidate_80211_data));
}
}  // namespace

bool UrUdpWifiTraceStart(const uint8_t* nonce, size_t nonce_size, int port) {
    std::lock_guard<std::mutex> lock(lifecycle_mutex);
    StopLocked();
    portENTER_CRITICAL(&state_mux);
    const bool valid = state.Begin(nonce, nonce_size, port);
    const auto stats = state.Stats();
    portEXIT_CRITICAL(&state_mux);
    if (!valid)
        return false;
    const esp_err_t result = esp_wifi_set_tx_done_cb(TxDone);
    if (result != ESP_OK) {
        portENTER_CRITICAL(&state_mux);
        state.End();
        portEXIT_CRITICAL(&state_mux);
        ESP_LOGW(kTag, "[UR_WIFI_TX] DISABLED register=%d", result);
        return false;
    }
    registered = true;
    ESP_LOGI(kTag, "[UR_WIFI_TX] BEGIN gen=%lu", static_cast<unsigned long>(stats.generation));
    return true;
}

void UrUdpWifiTraceStop() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex);
    StopLocked();
}

void UrUdpWifiTraceFlush() {
    for (int index = 0; index < 8; ++index) {
        UrUdpTxTraceEvent event;
        portENTER_CRITICAL(&state_mux);
        const bool available = state.Pop(&event);
        portEXIT_CRITICAL(&state_mux);
        if (!available)
            return;
        ESP_LOGI(kTag,
                 "[UR_WIFI_TX] DONE gen=%lu seq=%lu bytes=%u status=%d header=%d uptime_us=%llu",
                 static_cast<unsigned long>(event.generation),
                 static_cast<unsigned long>(event.sequence), static_cast<unsigned>(event.bytes),
                 event.reported_success, event.header_valid,
                 static_cast<unsigned long long>(event.uptime_us));
    }
}
