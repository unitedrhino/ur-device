/* 诊断专用无线发送元数据：只保留当前路由的数值结果，不持有帧、地址或音频。 */
#ifndef UR_UDP_TX_TRACE_STATE_H
#define UR_UDP_TX_TRACE_STATE_H

#include <array>
#include <cstddef>
#include <cstdint>

/* UrUdpTxTraceEvent 关联一个交接或驱动回调；reported_success不等同于云端收到或空口ACK。 */
struct UrUdpTxTraceEvent {
    uint64_t uptime_us = 0;         // 回调发生时的设备单调时钟，不是服务器时间。
    uint32_t generation = 0;        // 本地观察代际，避免跨会话混用序号。
    uint32_t sequence = 0;          // 当前UDP帧的网络序序号，转为主机序后保留。
    uint16_t bytes = 0;             // UDP载荷长度，包含16字节加密头。
    bool reported_success = false;  // Wi-Fi回调txStatus或交接返回ESP_OK，不混淆两种来源。
    bool header_valid = false;      // 声明长度与现有UDP结构校验是否一致。
    int32_t result_code = 0;        // 网络栈交接返回码；Wi-Fi异步回调不提供该码，保持0。
};

/* UrUdpTxTraceStats 保留观察质量；丢队列或未支持的回调布局不能据此判发送完整。 */
struct UrUdpTxTraceStats {
    uint32_t generation = 0;
    uint32_t callbacks = 0;
    uint32_t matched = 0;
    uint32_t pending_results = 0;  // 已复制目标元数据但交接未返回，结束快照不能忽略此数量。
    uint32_t queue_dropped = 0;
    uint32_t ignored_layout = 0;
    uint32_t null_frames = 0;           // 空缓冲拒绝数，不尝试读取回调指针。
    uint32_t short_frames = 0;          // 短于现有Ethernet/IPv4/UDP解析最低长度，不代表丢音频。
    uint32_t large_frames = 0;          // 超过1536字节的有界解析上限，保留拒绝而非截断。
    uint32_t ethertype_rejected = 0;    // 有界帧的EtherType不是IPv4，不猜测其他封装偏移。
    uint32_t ip_rejected = 0;           // 原IPv4/UDP结构门禁拒绝，包括分片及声明长度异常。
    uint32_t candidate_ipv4 = 0;        // 开头满足基本IPv4结构的候选数，不用于目标匹配。
    uint32_t candidate_80211_data = 0;  // 数据类型候选，只核验标准网络头，不匹配目标路由。
    uint32_t mac_h24 = 0;               // 标准帧控制推导的24字节MAC头完整候选数。
    uint32_t mac_h26 = 0;               // 标准帧控制推导的26字节MAC头完整候选数。
    uint32_t mac_h30 = 0;               // 30字节MAC头候选，可能为四地址或QoS加HT。
    uint32_t mac_h32 = 0;               // 四地址QoS的32字节MAC头完整候选数。
    uint32_t mac_h36 = 0;               // 四地址QoS加HT的36字节MAC头完整候选数。
    uint32_t mac_qos = 0;               // 帧控制含QoS类型的候选数，不解释服务质量结果。
    uint32_t mac_addr4 = 0;             // ToDS和FromDS同时置位的四地址候选数，不保存地址。
    uint32_t mac_protected = 0;         // Protected位候选数，不推断回调载荷已加密或解密。
    uint32_t mac_fragmented = 0;        // 更多分片或非零分片序号，不展开分片载荷。
    uint32_t mac_amsdu = 0;             // QoS指示A-MSDU，不展开聚合子帧。
    uint32_t mac_incomplete = 0;        // 类型已识别但声明长度不足标准MAC头。
    uint32_t mac_no_payload = 0;        // 数据类型中的Null子类型，不把伪造SNAP视为载荷。
    uint32_t llc_short = 0;             // 完整MAC后不足八字节SNAP，不访问缺失字节。
    uint32_t llc_ipv4 = 0;              // 标准头尾紧接RFC1042/IPv4标记，不扫描安全头或偏移。
    uint32_t llc_ip_valid = 0;          // 固定SNAP之后IPv4版本/头长/声明长度完整，不证明目标。
    uint32_t llc_udp_valid = 0;         // 完整IPv4之后非分片UDP长度合法，不读取音频或路由。
    uint16_t min_frame_size = 0;        // 本代际回调最短声明长度，超uint16时饱和，不保存帧。
    uint16_t max_frame_size = 0;        // 本代际回调最长声明长度；无回调时最小/最大均为0。
};

/* UrUdpTxTraceState 是无分配、有界队列；调用方必须串行保护Begin/End/Record/Pop。
 * 目标事件仅支持经校准的Ethernet/IPv4/UDP；MAC/LLC仅计候选，不扫描偏移猜路由。
 * 目标路由只存在内存，End清除；所有可输出结果均为数值且不包含原始帧内容。 */
class UrUdpTxTraceState {
public:
    /* Begin 校验16字节nonce及目的UDP端口，开启新代际；非法输入保持停用。 */
    bool Begin(const uint8_t* nonce, size_t nonce_size, int port);
    /* End 停止接收并擦除路由；已入队数值结果仍可读取，不借用外部生命周期。 */
    void End();
    /* Record 仅消费当前回调内有效的帧；不保存指针、不改写数据、不调用日志。 */
    void Record(const uint8_t* frame, size_t frame_size, bool success, uint64_t uptime_us);
    /* Capture 在交接前复制当前目标的数值元数据；输出不得为空，失败时清零。
     * 不保存借用帧；成功后调用方必须且只能Complete一次，不将匹配当作交接成功。 */
    bool Capture(const uint8_t* frame, size_t frame_size, UrUdpTxTraceEvent* event);
    /* Complete 在交接返回后保存原始返回码；关闭或跨代际结果丢弃，不重读借用缓冲。
     * success只表示对应SDK报告语义，uptime_us为完成时单调时钟，result_code不输出秘密。 */
    void Complete(UrUdpTxTraceEvent event, bool success, uint64_t uptime_us,
                  int32_t result_code = 0);
    /* Pop 取出一个已观察结果；队列空或输出为空时返回false。 */
    bool Pop(UrUdpTxTraceEvent* event);
    /* Stats 返回观察质量快照，必须与同代际的发送/入站证据组合使用。 */
    UrUdpTxTraceStats Stats() const { return stats_; }

private:
    /* kCapacity 仅缓冲短暂worker调度延迟；溢出显式计数，不能静默补齐。 */
    static constexpr size_t kCapacity = 32;
    std::array<uint8_t, 4> route_{};                     // 私密路由前缀只用于内存匹配，不对外返回。
    std::array<UrUdpTxTraceEvent, kCapacity> events_{};  // 固定长度数值环形队列。
    UrUdpTxTraceStats stats_{};                          // 当前代际统计，重启观察时重置。
    uint16_t port_ = 0;                                  // 目标目的端口，不记录设备或服务端地址。
    size_t head_ = 0;                                    // 待读取位置。
    size_t count_ = 0;                                   // 现有数值结果数量。
    bool active_ = false;                                // 关闭后晚到回调不得污染下一轮。
};

#endif  // UR_UDP_TX_TRACE_STATE_H
