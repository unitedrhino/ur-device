// 文件说明：共享宿主沿唯一FocusEndpoint复用初始AI租约及移交操作；实际结果来自原执行器发布，不从诊断投影重建。
#pragma once
#include "ur/device/ai_human_handoff.hpp"
#include "ur/device/focus_endpoint.hpp"

namespace ur::device {
// SharedHandoffFocusEndpoint
// 借唯一发布器、整组驱动及可信权限；不创建网络、输入源或第二个AI执行器。
// 宿主先接纳实际初始整组idle再构造，按原已鉴权Topic交付初始控制；移交复用该原租约。
class SharedHandoffFocusEndpoint final : public AiHandoffFocusPort, private PublishPort {
public:
    // 构造只绑定真实端口，不授权任何原腿；缺完整首次idle、期限或相异Topic时拒绝新提交。
    SharedHandoffFocusEndpoint(PublishPort& publisher, FocusDriver& driver,
                               FocusAuthority& authority, std::string up, std::string down,
                               std::uint64_t deadline);
    SharedHandoffFocusEndpoint(const SharedHandoffFocusEndpoint&) = delete;
    SharedHandoffFocusEndpoint& operator=(const SharedHandoffFocusEndpoint&) = delete;
    // receive 供宿主处理初始AI及终态release；不把MQTT下行focusState视为驱动事实。
    FocusEndpointResult receive(std::string_view topic, std::string_view body, std::uint64_t now,
                                std::uint64_t wall);
    // poll 推进同一个原FocusEndpoint；驱动完成但本地网络未接纳前不交付移交结果。
    void poll(std::uint64_t now, std::uint64_t wall);
    bool supports_microphone_speaker() const override;
    // submit 复核结构体与原信封完全一致，并固定原操作身份；接纳不等于实际取得。
    FocusSubmission submit(const FocusCommand& command, std::uint64_t now,
                           std::uint64_t wall) override;
    // take_result
    // 仅原协调器submit关联的实际原回执；denied默认仍不证明没有副作用。
    std::optional<AiHandoffFocusResult> take_result() override;
    // take_receipt
    // 为宿主交付全部实际原回执，含初始AI；与协调结果是同一原事实的独立消费。
    std::optional<FocusState> take_receipt();
    // close 仅完整宿主结束时调用；不断共享网络，实际媒体释放仍须poll及驱动回读。
    void close();
    bool settled() const;  // 实际租约及原网络发布排空，不包含两个宿主消费队列。

private:
    // Correlation
    // 固定协调器原请求，最多32条不驱逐；同token冲突不能执行另一原操作。
    struct Correlation {
        FocusCommand command;  // 原完整组、租约及事件水位。
        std::string bytes;     // 原规范报文，不能用结构体外部字段覆写。
    };
    // publish
    // 只供本对象唯一执行器调用；先保留队列容量，网络成功后才保存原实际回执。
    bool publish(std::string_view topic, std::string_view body) override;
    PublishPort& publisher_;                       // 同一共享连接的固定原发布端口。
    std::string up_, down_;                        // 已鉴权设备精确路由。
    bool supported_ = false;                       // 原完整初始空闲证明，构造开关不能补造。
    std::vector<Correlation> correlations_;        // 同一原协调流程的不可变操作墓碑。
    FixedQueue<FocusState, 8> receipts_;           // 背压不能丢掉已接纳初始或释放事实。
    FixedQueue<AiHandoffFocusResult, 8> results_;  // 仅原submit操作的实际完成事实。
    FocusEndpoint endpoint_;                       // 每个原租约唯一执行器，初始AI与移交操作共用。
};
}  // namespace ur::device
