/* 固定SDK诊断交接点：包装两种esp-netif发送路径，原函数仅调用一次并原样返回。
 * 交接前复制代际/序号/长度，交接后只记录真实返回码，禁止重读可能被驱动消费的帧。
 * 临界区只做有界内存操作，SDK与日志始终在临界区之外；不替换驱动、不猜MAC偏移。
 * 交接ESP_OK仅表示该调用成功，不能推导为空口ACK或服务器收到。 */
#include "ur_udp_netif_trace.h"
#include "ur_udp_tx_trace_state.h"

#include <esp_log.h>
#include <esp_netif.h>
#include <esp_netif_net_stack.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>

#include <mutex>

/* __real函数由链接器解析为原SDK实现，不替换SDK源码或调用路径中的参数。 */
extern "C" esp_err_t __real_esp_netif_transmit(esp_netif_t* netif, void* data, size_t size);
extern "C" esp_err_t __real_esp_netif_transmit_wrap(esp_netif_t* netif, void* data, size_t size,
                                                    void* netstack_buf);

namespace {
constexpr char kTag[] = "UrUdpNetifTrace";              // 所有日志仅含本地代际和数值交接结果。
UrUdpTxTraceState netif_state;                          // 与Wi-Fi异步回调使用独立质量计数及队列。
portMUX_TYPE netif_mux = portMUX_INITIALIZER_UNLOCKED;  // 不在锁内调用SDK或日志。
std::mutex netif_lifecycle;                             // 串行开启/结束普通任务观察。
esp_netif_t* watched_netif = nullptr;                   // 只在内存筛选STA句柄，不输出地址。
bool netif_started = false;                             // 仅在生命周期锁内读写，避免重复END。

/* CaptureBefore 仅观察当前STA；返回值不含借用指针，驱动可在交接中消费原始帧。 */
UrUdpTxTraceEvent CaptureBefore(esp_netif_t* netif, const void* data, size_t size) {
    UrUdpTxTraceEvent event;
    portENTER_CRITICAL(&netif_mux);
    if (watched_netif != nullptr && netif == watched_netif)
        netif_state.Capture(static_cast<const uint8_t*>(data), size, &event);
    portEXIT_CRITICAL(&netif_mux);
    return event;
}

/* CompleteAfter 原样记录返回码；关闭/跨代际时安全丢弃，不改变业务结果。 */
void CompleteAfter(UrUdpTxTraceEvent event, esp_err_t result) {
    if (event.generation == 0)
        return;
    const uint64_t now = static_cast<uint64_t>(esp_timer_get_time());
    portENTER_CRITICAL(&netif_mux);
    netif_state.Complete(event, result == ESP_OK, now, result);
    portEXIT_CRITICAL(&netif_mux);
}

/* StopNetifLocked 先停用，再输出有界快照；未返回交接数保持可见，不冒充完整窗口。 */
void StopNetifLocked() {
    portENTER_CRITICAL(&netif_mux);
    watched_netif = nullptr;
    netif_state.End();
    const auto stats = netif_state.Stats();
    portEXIT_CRITICAL(&netif_mux);
    if (!netif_started)
        return;
    netif_started = false;
    for (int batch = 0; batch < 4; ++batch)
        UrUdpNetifTraceFlush();
    ESP_LOGI(
        kTag, "[UR_NETIF_TX] END gen=%lu calls=%lu matched=%lu dropped=%lu ignored=%lu pending=%lu",
        static_cast<unsigned long>(stats.generation), static_cast<unsigned long>(stats.callbacks),
        static_cast<unsigned long>(stats.matched), static_cast<unsigned long>(stats.queue_dropped),
        static_cast<unsigned long>(stats.ignored_layout),
        static_cast<unsigned long>(stats.pending_results));
}
}  // namespace

/* __wrap_esp_netif_transmit 只在已校准Ethernet交接点观察，全部参数和返回值保持原样。 */
extern "C" esp_err_t __wrap_esp_netif_transmit(esp_netif_t* netif, void* data, size_t size) {
    const auto event = CaptureBefore(netif, data, size);
    const esp_err_t result = __real_esp_netif_transmit(netif, data, size);
    CompleteAfter(event, result);
    return result;
}

/* __wrap_esp_netif_transmit_wrap 覆盖Wi-Fi带pbuf引用的交接路径，不复制或持有pbuf。 */
extern "C" esp_err_t __wrap_esp_netif_transmit_wrap(esp_netif_t* netif, void* data, size_t size,
                                                    void* netstack_buf) {
    const auto event = CaptureBefore(netif, data, size);
    const esp_err_t result = __real_esp_netif_transmit_wrap(netif, data, size, netstack_buf);
    CompleteAfter(event, result);
    return result;
}

bool UrUdpNetifTraceStart(const uint8_t* nonce, size_t nonce_size, int port) {
    std::lock_guard<std::mutex> lock(netif_lifecycle);
    StopNetifLocked();
    // 只复用已有Wi-Fi STA实例；不创建第二驱动或截获其他接口。
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    portENTER_CRITICAL(&netif_mux);
    const bool valid = netif != nullptr && netif_state.Begin(nonce, nonce_size, port);
    if (valid)
        watched_netif = netif;
    const auto stats = netif_state.Stats();
    portEXIT_CRITICAL(&netif_mux);
    if (!valid) {
        ESP_LOGW(kTag, "[UR_NETIF_TX] DISABLED unavailable_or_invalid=1");
        return false;
    }
    netif_started = true;
    ESP_LOGI(kTag, "[UR_NETIF_TX] BEGIN gen=%lu", static_cast<unsigned long>(stats.generation));
    return true;
}

void UrUdpNetifTraceStop() {
    std::lock_guard<std::mutex> lock(netif_lifecycle);
    StopNetifLocked();
}

void UrUdpNetifTraceFlush() {
    for (int index = 0; index < 8; ++index) {
        UrUdpTxTraceEvent event;
        portENTER_CRITICAL(&netif_mux);
        const bool available = netif_state.Pop(&event);
        portEXIT_CRITICAL(&netif_mux);
        if (!available)
            return;
        ESP_LOGI(kTag,
                 "[UR_NETIF_TX] DONE gen=%lu seq=%lu bytes=%u result=%ld header=%d uptime_us=%llu",
                 static_cast<unsigned long>(event.generation),
                 static_cast<unsigned long>(event.sequence), static_cast<unsigned>(event.bytes),
                 static_cast<long>(event.result_code), event.header_valid,
                 static_cast<unsigned long long>(event.uptime_us));
    }
}
