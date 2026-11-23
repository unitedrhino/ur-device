/* UDP 发送取证测试：执行生产函数，只模拟硬件加密和套接字返回。 */
#include "ur_ai_runtime.h"

#include <array>
#include <cassert>
#include <climits>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

/* logs 保存纯数值取证，禁止以音频或nonce作为日志内容。 */
static std::vector<std::string> logs;
/* kTag 与生产文件相同，便于直接编译实际发送函数。 */
[[maybe_unused]] constexpr char kTag[] = "UdpAudio";

/* CaptureLog 模拟ESP日志边界，保留实际格式与参数供契约断言。 */
template <typename... Arguments>
void CaptureLog(const char*, const char* format, Arguments... arguments) {
    char line[256];
    const int length = std::snprintf(line, sizeof(line), format, arguments...);
    assert(length > 0 && static_cast<size_t>(length) < sizeof(line));
    logs.emplace_back(line);
}
#define ESP_LOGI(...) CaptureLog(__VA_ARGS__)

/* FakeUdp 模拟成功、失败和短返回，保存真实构造的数据报以校验帧头。 */
struct FakeUdp {
    /* result 为指定返回值；INT_MIN 表示返回完整数据报长度。 */
    int result = INT_MIN;
    /* packets 仅在测试内保存发送数据，供实际帧头解析器核验。 */
    std::vector<std::string> packets;
    /* Send 提供可控返回值，不据此声明真实对端已收到。 */
    int Send(const std::string& datagram) {
        packets.push_back(datagram);
        return result == INT_MIN ? static_cast<int>(datagram.size()) : result;
    }
};
/* Udp 是生产函数依赖的套接字接口替身，不模拟网络传输成功。 */
using Udp = FakeUdp;

/* UdpAudioChannel 仅提供实际SendPayload依赖字段，公开以注入硬件故障。 */
class UdpAudioChannel {
public:
    /* 以下字段仅复刻发送函数依赖状态，序号由生产函数递增。 */
    static constexpr size_t kHeaderSize = kUrAiUdpHeaderSize;
    std::mutex channel_mutex_;
    std::shared_ptr<Udp> udp_ = std::make_shared<Udp>();
    std::string nonce_template_ = std::string(kHeaderSize, 'K');
    bool preserve_nonce_timestamp_ = false;
    uint32_t local_sequence_ = 0;
    bool crypto_ok = true;
    /* 测试模板使用生产协议的有效类型字节，其余固定值仅作为私密内容哨兵。 */
    UdpAudioChannel() { nonce_template_[0] = 0x01; }
    /* SendPayload 的实际定义直接取自生产源码，不另写测试实现。 */
    bool SendPayload(const uint8_t* payload, size_t payload_size, uint32_t timestamp = 0);
    /* CryptAesCtr 只替换加密硬件边界；密码算法由既有独立向量测试覆盖。 */
    bool CryptAesCtr(const uint8_t* input, size_t size, const uint8_t*, uint8_t* output) {
        if (!crypto_ok) return false;
        std::memcpy(output, input, size);
        return true;
    }
};

// PRODUCTION_SEND_PAYLOAD

/* main 验证取证开关及原有成功判据，不将短返回改成新的业务行为。 */
int main() {
    UdpAudioChannel channel;
    const std::string audio = "PRIVATE_AUDIO_CONTENT";
    const auto* bytes = reinterpret_cast<const uint8_t*>(audio.data());
    assert(!channel.SendPayload(nullptr, audio.size()));
    assert(!channel.SendPayload(bytes, 0));
    assert(!channel.SendPayload(bytes, UINT16_MAX + size_t{1}));
    assert(logs.empty() && channel.local_sequence_ == 0);
    assert(channel.SendPayload(bytes, audio.size(), 900));
    channel.udp_->result = -1;
    assert(!channel.SendPayload(bytes, audio.size(), 960));
    channel.udp_->result = 3;
    assert(channel.SendPayload(bytes, audio.size(), 1020));
    channel.udp_->result = INT_MIN;
    assert(channel.SendPayload(bytes, audio.size(), 1080));
    assert(channel.udp_->packets.size() == 4 && channel.local_sequence_ == 4);
    for (size_t index = 0; index < channel.udp_->packets.size(); ++index) {
        const auto& packet = channel.udp_->packets[index];
        UrAiUdpHeader header;
        assert(UrAiParseUdpHeader(reinterpret_cast<const uint8_t*>(packet.data()), packet.size(), &header));
        assert(header.sequence == index + 1 && header.payload_size == audio.size());
        assert(header.timestamp == 900 + index * 60);
    }
#if CONFIG_UR_AI_E2E_TEST
    assert(logs.size() == 4);
    assert(logs[0] == "[UR_UDP_E2E] SEND seq=1 bytes=37 sent=37");
    assert(logs[1] == "[UR_UDP_E2E] SEND seq=2 bytes=37 sent=-1");
    assert(logs[2] == "[UR_UDP_E2E] SEND seq=3 bytes=37 sent=3");
    assert(logs[3] == "[UR_UDP_E2E] SEND seq=4 bytes=37 sent=37");
    for (const auto& line : logs) {
        assert(line.find(audio) == std::string::npos);
        assert(line.find(channel.nonce_template_) == std::string::npos);
    }
#else
    assert(logs.empty());
#endif
    channel.crypto_ok = false;
    assert(!channel.SendPayload(bytes, audio.size()));
    assert(channel.udp_->packets.size() == 4);
    channel.udp_.reset();
    assert(!channel.SendPayload(bytes, audio.size()));
}
