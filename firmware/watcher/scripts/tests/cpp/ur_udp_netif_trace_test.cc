/* 网络交接测试：生产包装器与解析核心，替换SDK边界，不模拟空口或云端送达。 */
#include "ur_udp_netif_trace.h"
#include "ur_ai_runtime.h"

#include <array>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

/* SDK替身仅提供参数与返回值，两个接口实例明确区分STA和其他接口。 */
using esp_err_t = int;
constexpr int ESP_OK = 0;
struct esp_netif_t {
    int identity;
};
static esp_netif_t sta{1}, other{2};
static bool sta_available = true;
static std::atomic<int> real_calls{0};
static int sdk_result = 0;
static bool consume_buffer = false;
/* expected参数只在单线程透传用例启用，核验SDK收到原句柄、指针及声明长度。 */
static bool check_arguments = false;
static esp_netif_t* expected_netif = nullptr;
static void* expected_data = nullptr;
static size_t expected_size = 0;
static void (*during_send)() = nullptr;  // 模拟交接返回前普通任务改变观察代际。
static thread_local int critical_depth = 0;
static std::mutex logs_mutex;
static std::vector<std::string> logs;  // 仅收集数值日志供隐私断言。
using portMUX_TYPE = std::mutex;
#define portMUX_INITIALIZER_UNLOCKED \
    {                                \
    }
void portENTER_CRITICAL(portMUX_TYPE* mux) {
    mux->lock();
    ++critical_depth;
}
void portEXIT_CRITICAL(portMUX_TYPE* mux) {
    --critical_depth;
    mux->unlock();
}
int64_t esp_timer_get_time() { return 654321; }
esp_netif_t* esp_netif_get_handle_from_ifkey(const char* key) {
    assert(critical_depth == 0 && std::strcmp(key, "WIFI_STA_DEF") == 0);
    return sta_available ? &sta : nullptr;
}

/* Log 检查日志不在临界区，并保留真实格式，不输出帧、nonce或地址。 */
template <typename... Arguments>
void Log(const char*, const char* format, Arguments... arguments) {
    assert(critical_depth == 0);
    char line[512];
    int size;
    if constexpr (sizeof...(arguments) == 0)
        size = std::snprintf(line, sizeof(line), "%s", format);
    else
        size = std::snprintf(line, sizeof(line), format, arguments...);
    assert(size > 0 && static_cast<size_t>(size) < sizeof(line));
    std::lock_guard<std::mutex> lock(logs_mutex);
    logs.emplace_back(line);
}
#define ESP_LOGI(...) Log(__VA_ARGS__)
#define ESP_LOGW(...) Log(__VA_ARGS__)

/* Nonce/Frame 构造只用于主机的自有Ethernet向量，路由与音频哨兵禁止进入日志。 */
std::array<uint8_t, 16> Nonce() {
    std::array<uint8_t, 16> nonce{};
    nonce[0] = 1;
    std::memcpy(nonce.data() + 4, "HIDE", 4);
    return nonce;
}
void Write16(uint8_t* p, uint16_t value) {
    p[0] = value >> 8;
    p[1] = value & 0xff;
}
std::vector<uint8_t> Frame(uint32_t sequence = 7) {
    const std::string payload = "PRIVATE_AUDIO_CONTENT";
    std::vector<uint8_t> frame(14 + 20 + 8 + 16 + payload.size());
    Write16(frame.data() + 12, 0x0800);
    auto* ip = frame.data() + 14;
    ip[0] = 0x45;
    ip[9] = 17;
    Write16(ip + 2, frame.size() - 14);
    auto* udp = ip + 20;
    Write16(udp + 2, 1234);
    Write16(udp + 4, 8 + 16 + payload.size());
    const auto nonce = Nonce();
    assert(UrAiBuildUdpHeader(nonce.data(), nonce.size(), payload.size(), 0, sequence, true,
                              udp + 8, 16));
    std::memcpy(udp + 24, payload.data(), payload.size());
    return frame;
}

/* RealSend 模拟原SDK消费借用缓冲，不能在包装器返回阶段依赖该缓冲。 */
int RealSend(esp_netif_t* netif, void* data, size_t size) {
    assert(critical_depth == 0 && (netif == &sta || netif == &other));
    if (check_arguments)
        assert(netif == expected_netif && data == expected_data && size == expected_size);
    ++real_calls;
    if (consume_buffer && data != nullptr)
        std::memset(data, 0, size);
    if (during_send != nullptr)
        during_send();
    return sdk_result;
}
extern "C" int __real_esp_netif_transmit(esp_netif_t* netif, void* data, size_t size) {
    return RealSend(netif, data, size);
}
extern "C" int __real_esp_netif_transmit_wrap(esp_netif_t* netif, void* data, size_t size,
                                              void* pbuf) {
    assert(pbuf == &other);  // 原引用指针须原样交给SDK，不能被包装器替换或保存。
    return RealSend(netif, data, size);
}

// PRODUCTION_NETIF

/* Start 仅开启诊断状态，不初始化网络或模拟设备上线。 */
void Start() {
    const auto nonce = Nonce();
    assert(UrUdpNetifTraceStart(nonce.data(), nonce.size(), 1234));
}
/* ChangeGeneration 模拟交接进行期间旧会话结束并建立新观察。 */
void ChangeGeneration() {
    UrUdpNetifTraceStop();
    Start();
}

/* RunCase 只验证生产包装行为与质量边界，不据合成向量声明真机布局已校准。 */
void RunCase(const std::string& name) {
    auto frame = Frame();
    if (name == "passthrough") {
        check_arguments = true;
        expected_netif = &sta;
        expected_data = frame.data();
        expected_size = frame.size();
        sdk_result = -42;
        assert(__wrap_esp_netif_transmit(&sta, frame.data(), frame.size()) == -42);
        assert(__wrap_esp_netif_transmit_wrap(&sta, frame.data(), frame.size(), &other) == -42);
        assert(logs.empty() && real_calls == 2);
        Start();
        const size_t before = logs.size();
        expected_netif = &other;
        assert(__wrap_esp_netif_transmit(&other, frame.data(), frame.size()) == -42);
        assert(logs.size() == before && real_calls == 3);
        UrUdpNetifTraceStop();
        assert(logs.back() ==
               "[UR_NETIF_TX] END gen=1 calls=0 matched=0 dropped=0 ignored=0 pending=0");
    } else if (name == "results") {
        Start();
        for (int code : {0, 0x3005, -1}) {
            sdk_result = code;
            const size_t before = logs.size();
            assert(__wrap_esp_netif_transmit(&sta, frame.data(), frame.size()) == code);
            assert(__wrap_esp_netif_transmit_wrap(&sta, frame.data(), frame.size(), &other) ==
                   code);
            assert(logs.size() == before);
        }
        assert(real_calls == 6);
        UrUdpNetifTraceStop();
        assert(logs.size() == 8 && logs[3].find("result=12293 header=1") != std::string::npos);
        assert(logs[5].find("result=-1 header=1") != std::string::npos);
    } else if (name == "consumed_buffer") {
        Start();
        consume_buffer = true;
        assert(__wrap_esp_netif_transmit_wrap(&sta, frame.data(), frame.size(), &other) == 0);
        assert(frame == std::vector<uint8_t>(frame.size(), 0));
        UrUdpNetifTraceStop();
        assert(logs[1] ==
               "[UR_NETIF_TX] DONE gen=1 seq=7 bytes=37 result=0 header=1 uptime_us=654321");
    } else if (name == "late_return") {
        Start();
        during_send = ChangeGeneration;
        assert(__wrap_esp_netif_transmit(&sta, frame.data(), frame.size()) == 0);
        during_send = nullptr;
        assert(logs[1].find("pending=1") != std::string::npos);
        assert(__wrap_esp_netif_transmit(&sta, frame.data(), frame.size()) == 0);
        UrUdpNetifTraceStop();
        assert(logs.size() == 5 && logs[3].find("DONE gen=2") != std::string::npos);
    } else if (name == "boundaries") {
        Start();
        for (size_t size = 0; size < frame.size(); ++size)
            assert(__wrap_esp_netif_transmit(&sta, frame.data(), size) == 0);
        assert(__wrap_esp_netif_transmit(&sta, nullptr, 100) == 0);
        frame[46] ^= 1;  // 当前路由位于14+20+8+4，不扫描其他位置。
        assert(__wrap_esp_netif_transmit(&sta, frame.data(), frame.size()) == 0);
        frame = Frame();
        Write16(frame.data() + 36, 1235);
        assert(__wrap_esp_netif_transmit_wrap(&sta, frame.data(), frame.size(), &other) == 0);
        UrUdpNetifTraceStop();
        assert(logs.size() == 2 && logs.back().find("matched=0") != std::string::npos);
    } else if (name == "overflow") {
        Start();
        for (int index = 0; index < 40; ++index)
            assert(__wrap_esp_netif_transmit(&sta, frame.data(), frame.size()) == 0);
        UrUdpNetifTraceStop();
        assert(logs.size() == 34 && logs.back().find("matched=40 dropped=8") != std::string::npos);
    } else if (name == "concurrent") {
        Start();
        std::thread first([&] {
            for (int i = 0; i < 100; ++i)
                __wrap_esp_netif_transmit(&sta, frame.data(), frame.size());
        });
        std::thread second([&] {
            for (int i = 0; i < 100; ++i)
                __wrap_esp_netif_transmit_wrap(&sta, frame.data(), frame.size(), &other);
        });
        for (int i = 0; i < 30; ++i)
            UrUdpNetifTraceFlush();
        first.join();
        second.join();
        UrUdpNetifTraceStop();
        assert(real_calls == 200 && logs.back().find("calls=200 matched=200") != std::string::npos);
        assert(logs.back().find("pending=0") != std::string::npos);
    } else if (name == "unavailable") {
        sta_available = false;
        const auto nonce = Nonce();
        assert(!UrUdpNetifTraceStart(nonce.data(), nonce.size(), 1234));
        sta_available = true;
        assert(!UrUdpNetifTraceStart(nullptr, 16, 1234));
        Start();
        UrUdpNetifTraceStop();
        const size_t before = logs.size();
        UrUdpNetifTraceStop();
        assert(logs.size() == before);
    } else {
        assert(false && "unknown test case");
    }
    for (const auto& line : logs) {
        assert(line.find("HIDE") == std::string::npos);
        assert(line.find("PRIVATE_AUDIO_CONTENT") == std::string::npos);
    }
}

int main(int argc, char** argv) {
    assert(argc == 2);
    RunCase(argv[1]);
}
