#include "ur_signaling_webrtc_selftest.h"

#include <esp_heap_caps.h>
#include <esp_log.h>

#include <atomic>
#include <cstring>
#include <string>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "peer_connection.h"

namespace {
constexpr const char* kTag = "ur_rtc_test";

/* SelftestPeer 保存本地 Peer 与对端待交换的 SDP（信令直连，不经网络）。 */
struct SelftestPeer {
    PeerConnection* pc = nullptr;
    std::string pending_sdp;
    bool connected = false;
    bool audio_received = false;
    int role = 0;  // 0=offerer 1=answerer
};
SelftestPeer g_peers[2];

void OnIceState(PeerConnectionState state, void* ctx) {
    auto* self = static_cast<SelftestPeer*>(ctx);
    ESP_LOGI(kTag, "peer%d state -> %d", self->role, static_cast<int>(state));
    if (state == PEER_CONNECTION_CONNECTED) {
        self->connected = true;
    }
}

void OnAudioTrack(uint8_t* data, size_t size, void* ctx) {
    auto* self = static_cast<SelftestPeer*>(ctx);
    if (data != nullptr && size > 0) {
        self->audio_received = true;
    }
}

/* OpenOne 按角色创建 PeerConnection；G711A 16k（避开外部编码依赖）。 */
bool OpenOne(int idx) {
    auto* p = &g_peers[idx];
    p->role = idx;
    PeerConfiguration config = {};
    config.audio_codec = CODEC_PCMA;
    config.datachannel = DATA_CHANNEL_NONE;
    config.onaudiotrack = &OnAudioTrack;
    config.user_data = p;
    p->pc = peer_connection_create(&config);
    if (p->pc == nullptr) {
        ESP_LOGE(kTag, "peer%d create failed", idx);
        return false;
    }
    peer_connection_oniceconnectionstatechange(p->pc, &OnIceState);
    return true;
}
}  // namespace

/* SelftestImpl 双 peer 互连主循环：SDP 交换 + loop 驱动 + 音频往返。 */
static esp_err_t SelftestImpl() {
    ESP_LOGI(kTag, "selftest core: dual local peers");
    if (!OpenOne(0) || !OpenOne(1)) {
        peer_connection_destroy(g_peers[0].pc);
        peer_connection_destroy(g_peers[1].pc);
        g_peers[0] = {};
        g_peers[1] = {};
        return ESP_FAIL;
    }
    const char* offer = peer_connection_create_offer(g_peers[0].pc);
    if (offer == nullptr) {
        ESP_LOGE(kTag, "offer failed");
        peer_connection_destroy(g_peers[0].pc);
        peer_connection_destroy(g_peers[1].pc);
        g_peers[0] = {};
        g_peers[1] = {};
        return ESP_FAIL;
    }
    ESP_LOGI(kTag, "offer %d bytes", static_cast<int>(strlen(offer)));
    peer_connection_set_remote_description(g_peers[1].pc, offer, SDP_TYPE_OFFER);
    const char* answer = peer_connection_create_answer(g_peers[1].pc);
    if (answer == nullptr) {
        ESP_LOGE(kTag, "answer failed");
        peer_connection_destroy(g_peers[0].pc);
        peer_connection_destroy(g_peers[1].pc);
        g_peers[0] = {};
        g_peers[1] = {};
        return ESP_FAIL;
    }
    ESP_LOGI(kTag, "answer %d bytes", static_cast<int>(strlen(answer)));
    peer_connection_set_remote_description(g_peers[0].pc, answer, SDP_TYPE_ANSWER);

    uint8_t g711_frame[160] = {0};
    for (int i = 0; i < 200; ++i) {  // 20 秒 @100ms
        vTaskDelay(pdMS_TO_TICKS(100));
        peer_connection_loop(g_peers[0].pc);
        peer_connection_loop(g_peers[1].pc);
        if (g_peers[0].connected && g_peers[1].connected) {
            peer_connection_send_audio(g_peers[0].pc, g711_frame, sizeof(g711_frame));
            if (g_peers[1].audio_received) {
                ESP_LOGI(kTag, "selftest PASS: connected + audio roundtrip");
                peer_connection_destroy(g_peers[0].pc);
                peer_connection_destroy(g_peers[1].pc);
                g_peers[0] = {};
                g_peers[1] = {};
                return ESP_OK;
            }
        }
    }
    ESP_LOGE(kTag, "selftest FAIL: connected=%d/%d audio=%d", g_peers[0].connected,
             g_peers[1].connected, g_peers[1].audio_received);
    peer_connection_destroy(g_peers[0].pc);
    peer_connection_destroy(g_peers[1].pc);
    g_peers[0] = {};
    g_peers[1] = {};
    return ESP_FAIL;
}

/* SelftestTask 独立大栈任务：ICE 收包调用栈深，console 线程默认栈不足。 */
void SelftestTask(void* arg) {
    auto* result = static_cast<std::atomic<int>*>(arg);
    result->store(SelftestImpl() == ESP_OK ? 0 : 1);
    vTaskDelete(nullptr);
}

esp_err_t ur_signaling_webrtc_selftest(void) {
    ESP_LOGI(kTag, "selftest start: dual local peers (dedicated task)");
    static std::atomic<int> s_result{-1};
    s_result.store(-1);
    if (xTaskCreateWithCaps(SelftestTask, "ur_rtc_test", 32768, &s_result, 5, nullptr,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(kTag, "selftest task create failed");
        return ESP_ERR_NO_MEM;
    }
    for (int i = 0; i < 350; ++i) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if (s_result.load() >= 0) {
            return s_result.load() == 0 ? ESP_OK : ESP_FAIL;
        }
    }
    ESP_LOGE(kTag, "selftest timeout");
    return ESP_ERR_TIMEOUT;
}
