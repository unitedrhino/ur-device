/* 无线发送观察测试：实际解析/队列/SDK适配代码，只替换SDK和日志硬件边界。 */
#include "ur_ai_runtime.h"
#include "ur_udp_tx_trace_state.h"
#include "ur_udp_wifi_trace.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

/* 硬件边界保存回调和SDK返回，不模拟真实空口发送或云端收到。 */
using esp_err_t = int;
constexpr int ESP_OK = 0;
constexpr int WIFI_IF_STA = 0;
using wifi_tx_done_cb_t = void (*)(uint8_t, uint8_t*, uint16_t*, bool);
static std::atomic<wifi_tx_done_cb_t> sdk_callback{nullptr};
static int sdk_result = 0;
static int sdk_calls = 0;
static thread_local int critical_depth = 0;
static std::mutex log_mutex;
static std::vector<std::string> logs;
using portMUX_TYPE = std::mutex;
#define portMUX_INITIALIZER_UNLOCKED \
    {                                \
    }
/* 临界区替身保留真实互斥语义，并断言SDK/日志没有发生在临界区。 */
void portENTER_CRITICAL(portMUX_TYPE* mux) {
    mux->lock();
    ++critical_depth;
}
void portEXIT_CRITICAL(portMUX_TYPE* mux) {
    --critical_depth;
    mux->unlock();
}
int64_t esp_timer_get_time() { return 987654; }
int esp_wifi_set_tx_done_cb(wifi_tx_done_cb_t callback) {
    assert(critical_depth == 0);
    ++sdk_calls;
    if (sdk_result == ESP_OK)
        sdk_callback.store(callback);
    return sdk_result;
}
/* Log 仅收集实际诊断格式，检查不输出帧、路由或音频。 */
template <typename... Arguments>
void Log(const char*, const char* format, Arguments... arguments) {
    assert(critical_depth == 0);
    char line[512];
    const int size = std::snprintf(line, sizeof(line), format, arguments...);
    assert(size > 0 && static_cast<size_t>(size) < sizeof(line));
    std::lock_guard<std::mutex> lock(log_mutex);
    logs.emplace_back(line);
}
#define ESP_LOGI(...) Log(__VA_ARGS__)
#define ESP_LOGW(...) Log(__VA_ARGS__)

// PRODUCTION_DRIVER

/* Nonce 提供仅属于本测试的路由哨兵，不能将其泄露到日志。 */
std::array<uint8_t, 16> Nonce() {
    std::array<uint8_t, 16> nonce{};
    nonce[0] = 1;
    std::memcpy(nonce.data() + 4, "HIDE", 4);
    return nonce;
}
/* Write16 写测试网络序整数，供构造确定性边界帧。 */
void Write16(uint8_t* destination, uint16_t value) {
    destination[0] = value >> 8;
    destination[1] = value & 0xff;
}
/* Frame 只构造本地合成Ethernet/IPv4/UDP，不向任何网络发送。 */
std::vector<uint8_t> Frame(uint32_t sequence = 7, size_t ip_header = 20) {
    const std::string payload = "PRIVATE_AUDIO_CONTENT";
    std::vector<uint8_t> frame(14 + ip_header + 8 + 16 + payload.size());
    Write16(frame.data() + 12, 0x0800);
    uint8_t* ip = frame.data() + 14;
    ip[0] = 0x40 | static_cast<uint8_t>(ip_header / 4);
    ip[9] = 17;
    Write16(ip + 2, static_cast<uint16_t>(frame.size() - 14));
    uint8_t* udp = ip + ip_header;
    Write16(udp + 2, 1234);
    Write16(udp + 4, static_cast<uint16_t>(8 + 16 + payload.size()));
    const auto nonce = Nonce();
    assert(UrAiBuildUdpHeader(nonce.data(), nonce.size(), payload.size(), 0, sequence, true,
                              udp + 8, 16));
    std::memcpy(udp + 8 + 16, payload.data(), payload.size());
    return frame;
}

/* MacFrame 按显式给定的标准头长构造自有MAC/SNAP向量，不由生产规则反推期望。 */
std::vector<uint8_t> MacFrame(size_t header_size, uint8_t subtype, uint8_t flags) {
    const auto ethernet = Frame();
    std::vector<uint8_t> frame(header_size + 8 + ethernet.size() - 14, 0);
    frame[0] = subtype;
    frame[1] = flags;
    std::memcpy(frame.data() + 4, "PRIVATE_MAC_BYTES", 17);
    const uint8_t snap[] = {0xaa, 0xaa, 0x03, 0, 0, 0, 0x08, 0};
    std::memcpy(frame.data() + header_size, snap, sizeof(snap));
    std::copy(ethernet.begin() + 14, ethernet.end(), frame.begin() + header_size + 8);
    return frame;
}

/* RunCase 逐项检验生产核心；失败用例不把SDK替身当作真实Wi-Fi复现。 */
void RunCase(const std::string& name) {
    UrUdpTxTraceState core;
    const auto nonce = Nonce();
    assert(core.Begin(nonce.data(), nonce.size(), 1234));
    auto frame = Frame();
    UrUdpTxTraceEvent event;
    if (name == "valid") {
        core.Record(frame.data(), frame.size(), true, 100);
        frame.assign(frame.size(), 0);  // 回调结束后原缓冲失效，结果不能借用它。
        assert(core.Pop(&event) && event.sequence == 7 && event.bytes == 37);
        assert(event.generation == 1 && event.uptime_us == 100 && event.header_valid);
        assert(event.reported_success && !core.Pop(&event));
    } else if (name == "capture_complete") {
        UrUdpTxTraceEvent captured;
        assert(core.Capture(frame.data(), frame.size(), &captured));
        assert(core.Stats().pending_results == 1 && !core.Pop(&event));
        frame.assign(frame.size(), 0);  // SDK可能消费借用缓冲，交接返回后不能重读。
        core.Complete(captured, false, 104, 0x3005);
        assert(core.Stats().pending_results == 0 && core.Pop(&event));
        assert(event.sequence == 7 && event.bytes == 37 && event.header_valid);
        assert(!event.reported_success && event.uptime_us == 104 && event.result_code == 0x3005);
        assert(!core.Pop(&event));
        assert(!core.Capture(frame.data(), frame.size(), &captured));
        assert(captured.generation == 0 && !core.Capture(frame.data(), frame.size(), nullptr));
    } else if (name == "capture_late") {
        UrUdpTxTraceEvent captured;
        assert(core.Capture(frame.data(), frame.size(), &captured));
        core.End();
        core.Complete(captured, true, 105, 0);
        assert(!core.Pop(&event) && core.Stats().pending_results == 1);
        assert(core.Begin(nonce.data(), nonce.size(), 1234));
        UrUdpTxTraceEvent current;
        assert(core.Capture(frame.data(), frame.size(), &current));
        core.Complete(captured, true, 106, 0);
        assert(!core.Pop(&event) && core.Stats().pending_results == 1);
        core.Complete(current, true, 107, 0);
        assert(core.Pop(&event) && event.generation == 2 && event.result_code == 0);
        assert(core.Stats().pending_results == 0 && !core.Pop(&event));
    } else if (name == "driver_failure") {
        core.Record(frame.data(), frame.size(), false, 101);
        assert(core.Pop(&event) && !event.reported_success && event.header_valid);
    } else if (name == "truncation") {
        for (size_t size = 0; size < frame.size(); ++size)
            core.Record(frame.data(), size, true, size);
        core.Record(nullptr, 100, true, 0);
        assert(!core.Pop(&event) && core.Stats().matched == 0);
        assert(core.Stats().ignored_layout == frame.size() + 1);
    } else if (name == "scope") {
        frame[14 + 20 + 8 + 4] ^= 1;
        core.Record(frame.data(), frame.size(), true, 0);
        frame = Frame();
        Write16(frame.data() + 14 + 20 + 2, 1235);
        core.Record(frame.data(), frame.size(), true, 0);
        assert(core.Stats().callbacks == 2 && core.Stats().matched == 0 && !core.Pop(&event));
    } else if (name == "invalid_layout") {
        for (int mode = 0; mode < 6; ++mode) {
            frame = Frame();
            if (mode == 0)
                Write16(frame.data() + 12, 0x8100);  // VLAN不猜测偏移。
            if (mode == 1)
                frame[14] = 0x65;  // IPv6不能当成IPv4。
            if (mode == 2)
                frame[14 + 9] = 6;  // TCP不是音频UDP。
            if (mode == 3)
                frame[14 + 6] = 0x20;  // 片段缺少完整UDP，不进行重组。
            if (mode == 4)
                frame[14] = 0x44;  // 非法IHL。
            if (mode == 5)
                frame.resize(1537);  // 固定解析上限。
            core.Record(frame.data(), frame.size(), true, 0);
        }
        assert(!core.Pop(&event) && core.Stats().ignored_layout == 6);
    } else if (name == "reject_reasons") {
        core.Record(nullptr, 100, true, 0);
        core.Record(frame.data(), 1, true, 0);
        core.Record(frame.data(), 65536, true, 0);  // 超限先拒绝，不访问借用缓冲。
        Write16(frame.data() + 12, 0x8100);
        core.Record(frame.data(), frame.size(), true, 0);
        frame = Frame();
        frame[14] = 0x65;
        core.Record(frame.data(), frame.size(), true, 0);
        frame = Frame();
        core.Record(frame.data(), frame.size(), true, 0);
        const auto stats = core.Stats();
        assert(stats.callbacks == 6 && stats.matched == 1 && stats.ignored_layout == 5);
        assert(stats.null_frames == 1 && stats.short_frames == 1 && stats.large_frames == 1);
        assert(stats.ethertype_rejected == 1 && stats.ip_rejected == 1);
        assert(stats.min_frame_size == 1 && stats.max_frame_size == 65535);
        assert(core.Pop(&event) && event.sequence == 7 && !core.Pop(&event));
    } else if (name == "shape_candidates") {
        std::vector<uint8_t> raw_ip(frame.begin() + 14, frame.end());
        core.Record(raw_ip.data(), raw_ip.size(), true, 0);
        std::vector<uint8_t> wireless(60, 0);
        wireless[0] = 0x88;  // 仅计数QoS Data类型候选，不猜测LLC或音频位置。
        core.Record(wireless.data(), wireless.size(), true, 0);
        wireless[0] = 0x89;  // 非零协议版本不算候选。
        core.Record(wireless.data(), wireless.size(), true, 0);
        wireless[0] = 0x80;  // 管理帧不算数据类型候选。
        core.Record(wireless.data(), wireless.size(), true, 0);
        wireless[0] = 0x88;
        core.Record(wireless.data(), 23, true, 0);  // 缺少最小MAC头，不计候选。
        raw_ip[0] = 0x44;
        core.Record(raw_ip.data(), raw_ip.size(), true, 0);
        const auto stats = core.Stats();
        assert(stats.candidate_ipv4 == 1 && stats.candidate_80211_data == 1);
        assert(stats.callbacks == 6 && stats.matched == 0 && stats.ignored_layout == 6);
        assert(!core.Pop(&event));
        assert(core.Begin(nonce.data(), nonce.size(), 1234));
        assert(core.Stats().callbacks == 0 && core.Stats().min_frame_size == 0);
        assert(core.Stats().candidate_ipv4 == 0 && core.Stats().candidate_80211_data == 0);
    } else if (name == "mac_layout") {
        for (const auto& vector : std::vector<std::array<size_t, 3>>{{24, 0x08, 0x01},
                                                                     {26, 0x88, 0x41},
                                                                     {30, 0x08, 0x03},
                                                                     {32, 0x88, 0x03},
                                                                     {30, 0x88, 0x81},
                                                                     {36, 0x88, 0x83}}) {
            auto wireless = MacFrame(vector[0], vector[1], vector[2]);
            core.Record(wireless.data(), wireless.size(), true, 0);
        }
        const auto stats = core.Stats();
        assert(stats.mac_h24 == 1 && stats.mac_h26 == 1 && stats.mac_h30 == 2);
        assert(stats.mac_h32 == 1 && stats.mac_h36 == 1 && stats.mac_qos == 4);
        assert(stats.mac_addr4 == 3 && stats.mac_protected == 1 && stats.mac_incomplete == 0);
        assert(stats.llc_ipv4 == 6 && stats.llc_ip_valid == 6 && stats.llc_udp_valid == 6);
        assert(stats.matched == 0 && !core.Pop(&event));  // 布局候选不成为目标语音事件。
        assert(core.Begin(nonce.data(), nonce.size(), 1234));
        assert(core.Stats().mac_h24 == 0 && core.Stats().llc_udp_valid == 0);
    } else if (name == "mac_truncation") {
        for (const auto& vector : std::vector<std::array<size_t, 3>>{{24, 0x08, 0x01},
                                                                     {26, 0x88, 0x01},
                                                                     {30, 0x08, 0x03},
                                                                     {32, 0x88, 0x03},
                                                                     {30, 0x88, 0x81},
                                                                     {36, 0x88, 0x83}}) {
            const auto wireless = MacFrame(vector[0], vector[1], vector[2]);
            for (size_t size = 0; size < wireless.size(); ++size) {
                assert(core.Begin(nonce.data(), nonce.size(), 1234));
                core.Record(wireless.data(), size, true, 0);
                const auto stats = core.Stats();
                assert(stats.llc_ipv4 == (size >= vector[0] + 8 ? 1u : 0u));
                assert(stats.llc_ip_valid == 0 && stats.llc_udp_valid == 0);
                assert(stats.mac_incomplete == (size >= 24 && size < vector[0] ? 1u : 0u));
                assert(stats.matched == 0 && !core.Pop(&event));
            }
        }
    } else if (name == "mac_rejects") {
        auto wireless = MacFrame(26, 0x88, 0x01);
        wireless[26 + 3] = 1;  // 非RFC1042 OUI不能被猜测为IPv4。
        core.Record(wireless.data(), wireless.size(), true, 0);
        wireless = MacFrame(26, 0x88, 0x01);
        wireless.insert(wireless.begin() + 26, 1, 0);  // 正确标记在错误偏移，也不能扫描。
        core.Record(wireless.data(), wireless.size(), true, 0);
        wireless = MacFrame(26, 0x88, 0x41);
        wireless.insert(wireless.begin() + 26, 8, 0);  // 不猜测或跳过安全头。
        core.Record(wireless.data(), wireless.size(), true, 0);
        wireless = MacFrame(26, 0x88, 0x01);
        wireless[22] = 1;
        core.Record(wireless.data(), wireless.size(), true, 0);
        wireless[22] = 0;
        wireless[1] |= 0x04;
        core.Record(wireless.data(), wireless.size(), true, 0);
        wireless = MacFrame(26, 0xc8, 0x01);  // QoS Null即使伪造SNAP也不解释为载荷。
        core.Record(wireless.data(), wireless.size(), true, 0);
        wireless = MacFrame(26, 0x88, 0x01);
        wireless[24] = 0x80;  // A-MSDU需另一种封装，本诊断不展开聚合帧。
        core.Record(wireless.data(), wireless.size(), true, 0);
        const auto stats = core.Stats();
        assert(stats.mac_fragmented == 2 && stats.mac_no_payload == 1 && stats.mac_amsdu == 1);
        assert(stats.llc_ipv4 == 0 && stats.llc_ip_valid == 0 && stats.llc_udp_valid == 0);
        assert(stats.matched == 0 && !core.Pop(&event));
    } else if (name == "mac_network_bounds") {
        for (int mode = 0; mode < 5; ++mode) {
            auto wireless = MacFrame(24, 0x08, 0x01);
            auto* ip = wireless.data() + 32;
            if (mode == 0)
                ip[0] = 0x60;
            if (mode == 1)
                Write16(ip + 2, 1536);
            if (mode == 2)
                ip[9] = 6;
            if (mode == 3)
                Write16(ip + 6, 0x2000);
            if (mode == 4)
                Write16(ip + 20 + 4, 7);
            core.Record(wireless.data(), wireless.size(), true, 0);
        }
        const auto stats = core.Stats();
        assert(stats.llc_ipv4 == 5 && stats.llc_ip_valid == 3 && stats.llc_udp_valid == 0);
        assert(stats.matched == 0 && !core.Pop(&event));
    } else if (name == "header_invalid") {
        for (int mode = 0; mode < 3; ++mode) {
            frame = Frame();
            if (mode == 0)
                frame[14 + 20 + 8] = 2;
            if (mode == 1)
                frame[14 + 20 + 8 + 3] ^= 1;
            if (mode == 2)
                frame[14 + 20 + 5] ^= 1;
            core.Record(frame.data(), frame.size(), true, 0);
            assert(core.Pop(&event) && event.sequence == 7 && !event.header_valid);
        }
    } else if (name == "options_padding") {
        frame = Frame(9, 60);
        frame.resize(frame.size() + 4, 0);  // Ethernet填充不属于UDP载荷。
        core.Record(frame.data(), frame.size(), true, 0);
        assert(core.Pop(&event) && event.sequence == 9 && event.bytes == 37 && event.header_valid);
    } else if (name == "overflow") {
        for (int sequence = 1; sequence <= 40; ++sequence) {
            frame = Frame(sequence);
            core.Record(frame.data(), frame.size(), true, sequence);
        }
        assert(core.Stats().matched == 40 && core.Stats().queue_dropped == 8);
        for (int sequence = 1; sequence <= 32; ++sequence) {
            assert(core.Pop(&event) && event.sequence == static_cast<uint32_t>(sequence));
        }
        assert(!core.Pop(&event));
    } else if (name == "lifecycle") {
        core.Record(frame.data(), frame.size(), true, 0);
        core.End();
        core.Record(frame.data(), frame.size(), true, 0);
        assert(core.Stats().matched == 1 && core.Pop(&event));
        assert(core.Begin(nonce.data(), nonce.size(), 1234));
        assert(core.Stats().generation == 2 && core.Stats().matched == 0);
        core.Record(frame.data(), frame.size(), true, 0);
        assert(core.Pop(&event) && event.generation == 2);
        assert(!core.Pop(nullptr));
        assert(!core.Begin(nullptr, 16, 1234));
        assert(!core.Begin(nonce.data(), 15, 1234));
        assert(!core.Begin(nonce.data(), 16, 0));
        assert(!core.Begin(nonce.data(), 16, 65536));
        core.Record(frame.data(), frame.size(), true, 0);
        assert(core.Stats().matched == 1);
    } else if (name == "adapter") {
        assert(!UrUdpWifiTraceStart(nullptr, 0, 1234) && sdk_calls == 0);
        assert(UrUdpWifiTraceStart(nonce.data(), 16, 1234));
        const auto callback = sdk_callback.load();
        uint16_t size = frame.size();
        const size_t before_callback = logs.size();
        callback(1, frame.data(), &size, true);  // AP接口不能污染STA证据。
        callback(0, frame.data(), &size, false);
        assert(logs.size() == before_callback);  // 回调中不打印。
        UrUdpWifiTraceStop();
        assert(sdk_callback.load() == nullptr && sdk_calls == 2);
        assert(logs.size() == 4);
        assert(logs[1].find("seq=7 bytes=37 status=0 header=1") != std::string::npos);
        assert(logs[2].find("LAYOUT gen=1 h24=0") != std::string::npos);
        assert(logs[3].find("callbacks=1 matched=1 dropped=0") != std::string::npos);
        callback(0, frame.data(), &size, true);  // 迟到旧回调只访问静态停用状态。
        UrUdpWifiTraceFlush();
        assert(logs.size() == 4);
        UrUdpWifiTraceStop();
        assert(sdk_calls == 2);
    } else if (name == "registration_failure") {
        sdk_result = -3;
        assert(!UrUdpWifiTraceStart(nonce.data(), 16, 1234));
        assert(sdk_callback.load() == nullptr && sdk_calls == 1);
        UrUdpWifiTraceStop();
        assert(sdk_calls == 1 && logs.size() == 1);
        assert(logs[0].find("DISABLED register=-3") != std::string::npos);
    } else if (name == "unregistration_failure") {
        assert(UrUdpWifiTraceStart(nonce.data(), 16, 1234));
        const auto callback = sdk_callback.load();
        sdk_result = -4;
        UrUdpWifiTraceStop();
        assert(logs.back().find("unregister=-4") != std::string::npos);
        uint16_t size = frame.size();
        callback(0, frame.data(), &size, true);
        UrUdpWifiTraceFlush();
        assert(logs.size() == 3);  // SDK未注销也不能访问已结束目标或输出晚到数据。
        sdk_result = ESP_OK;
        assert(UrUdpWifiTraceStart(nonce.data(), 16, 1234));
        UrUdpWifiTraceStop();
        assert(sdk_callback.load() == nullptr);
    } else if (name == "concurrent") {
        assert(UrUdpWifiTraceStart(nonce.data(), 16, 1234));
        const auto callback = sdk_callback.load();
        std::thread producer([&]() {
            for (int sequence = 1; sequence <= 400; ++sequence) {
                auto packet = Frame(sequence);
                uint16_t size = packet.size();
                callback(0, packet.data(), &size, true);
            }
        });
        for (int repeat = 0; repeat < 100; ++repeat)
            UrUdpWifiTraceFlush();
        producer.join();
        UrUdpWifiTraceStop();
        assert(logs.back().find("callbacks=400 matched=400") != std::string::npos);
    } else {
        assert(false && "unknown case");
    }
    for (const auto& line : logs) {
        assert(line.find("HIDE") == std::string::npos);
        assert(line.find("PRIVATE_AUDIO_CONTENT") == std::string::npos);
        assert(line.find("PRIVATE_MAC_BYTES") == std::string::npos);
    }
}

/* main 按名字执行独立进程用例，避免静态状态让测试互相污染。 */
int main(int argc, char** argv) {
    assert(argc == 2);
    RunCase(argv[1]);
}
