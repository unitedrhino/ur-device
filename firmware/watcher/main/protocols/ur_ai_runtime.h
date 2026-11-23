/* 联犀 AI 可移植协议核心：统一会话关联、终态去重、表情与 UDP 帧头规则。 */
#ifndef UR_AI_RUNTIME_H
#define UR_AI_RUNTIME_H

#include <cstddef>
#include <cstdint>
#include <string_view>

/* kUrAiUdpHeaderSize 是 devicesim 契约规定的 AES-CTR nonce/帧头长度。 */
constexpr size_t kUrAiUdpHeaderSize = 16;

/* UrAiResponseDisposition 描述回复消息相对当前 respId 的处理方式。 */
enum class UrAiResponseDisposition {
    kAccept,
    kDefer,
    kReject,
};

/* UrAiTerminalEvent 表示单轮内必须去重的可靠终态消息。 */
enum class UrAiTerminalEvent : uint8_t {
    kSttDone = 0,
    kTextDone,
    kAudioDone,
    kEmotion,
};

/* UrAiUdpHeader 是解析后的联犀 UDP 音频帧头。 */
struct UrAiUdpHeader {
    uint16_t payload_size = 0;
    uint32_t timestamp = 0;
    uint32_t sequence = 0;
};

/* UrAiTerminalTracker 记录一轮回复中已经消费的终态，防止重复消息产生副作用。 */
class UrAiTerminalTracker {
public:
    /* Reset 清除上一轮全部终态标记。 */
    void Reset();
    /* Mark 首次记录指定终态并返回 true；重复终态返回 false。 */
    bool Mark(UrAiTerminalEvent event);
    /* Seen 返回指定终态是否已经消费。 */
    bool Seen(UrAiTerminalEvent event) const;

private:
    /* seen_mask_ 的每一位对应一种 UrAiTerminalEvent。 */
    uint8_t seen_mask_ = 0;
};

/* UrAiVadEndpoint 合并 AFE 短静音，过滤启动尾音；所有时间参数使用单调微秒。 */
class UrAiVadEndpoint {
public:
    /* silence_us 为判断句尾所需的连续静音时间，单位为微秒。 */
    explicit UrAiVadEndpoint(int64_t silence_us = 700000) : silence_us_(silence_us) {}
    /* Begin 开始新轮，清除上一轮语音及静音累计。 */
    void Begin(int64_t now_us);
    /* Update 记录 VAD 边沿；启动保护窗内的尾音不计为有效语音。 */
    void Update(bool speaking, int64_t now_us);
    /* ShouldStop 有有效语音且连续静音足够长时才允许结束本轮。 */
    bool ShouldStop(int64_t now_us) const;
private:
    /* 保护窗过滤唤醒/打断尾音，句尾等待覆盖 AFE 的 100ms 短静音。 */
    static constexpr int64_t kWarmupUs = 300000;
    static constexpr int64_t kMinSpeechUs = 180000;
    const int64_t silence_us_;  // 各音频入口的连续静音等待时间。
    int64_t ready_us_ = 0;
    int64_t speech_start_us_ = 0;
    int64_t speech_us_ = 0;
    int64_t silence_start_us_ = 0;
    bool speaking_ = false;
};

/* UrAiNormalizeEmotion 把平台 emoji/emotion 归一为设备支持的 21 类表情。 */
std::string_view UrAiNormalizeEmotion(std::string_view emoji, std::string_view emotion);

/* UrAiMatchesSession 校验可选 sessionId；缺省字段兼容，显式旧会话必须拒绝。 */
bool UrAiMatchesSession(std::string_view current_session, std::string_view incoming_session);

/* UrAiClassifyResponse 按当前 respId 判断回复应立即接收、延后还是拒绝。 */
UrAiResponseDisposition UrAiClassifyResponse(std::string_view current_response,
                                             std::string_view incoming_response,
                                             std::string_view method);

/* UrAiBuildUdpHeader 从服务端 nonce 模板生成一帧网络字节序 UDP 头。 */
bool UrAiBuildUdpHeader(const uint8_t* nonce_template, size_t nonce_size, size_t payload_size,
                        uint32_t timestamp, uint32_t sequence, bool preserve_nonce_timestamp,
                        uint8_t* output, size_t output_size);

/* UrAiParseUdpHeader 校验完整数据报长度并解析网络字节序 UDP 头。 */
bool UrAiParseUdpHeader(const uint8_t* datagram, size_t datagram_size, UrAiUdpHeader* output);

#endif  // UR_AI_RUNTIME_H
