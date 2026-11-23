// 文件说明：显式AI接续候选端的单路双向资源屏障；复用唯一输入门控与麦克风/扬声器仲裁器。
#pragma once
#include "browser_duplex_focus.hpp"
#include "browser_input_ownership_gate.hpp"
#include "ur/device/ai_endpoint_focus.hpp"
#include <functional>
#include <memory>

namespace ur::device {
// BrowserAIEndpointFocus
// 不创建采集源或网络；仅在核心候选双提交事实齐全后准备原完整组。
class BrowserAIEndpointFocus final : public EndpointFocusPreparePort,
                                     public FocusAuthority {
public:
  using Scope = std::function<
      std::optional<AIEndpointFocusScope>()>; // 事件线程读取核心当前候选。
  using Emit = std::function<bool(
      const std::string &)>; // 唯一已配对桥；受理不代替实际驱动回执。
  using Released = std::function<bool(std::string_view)>; // 原FocusEndpoint整组release事实及回执已消费。
  // output/source/fence/scope/emit均须来自原宿主；route区分原UDP或AI
  // Sender，默认旧UDP不变。
  BrowserAIEndpointFocus(
      OutputGroupGatePort &output, std::uint64_t boot, std::uint64_t deadline,
      std::uint64_t expires, BrowserInputOwnershipGate::Source source,
      BrowserInputOwnershipGate::Fence fence, Scope scope, Emit emit,
      BrowserInputRouteKind route = BrowserInputRouteKind::Udp, Released released = {});
  bool initialize(
      std::uint64_t now); // 仅请求原输出关闭，输入idle仍等真实Worklet探针。
  bool receive(std::string_view body,
               std::uint64_t now); // 仅原输入idle/gated回执。
  bool poll(std::uint64_t now,
            std::uint64_t wall); // 有界推进原输入及双向票据，不启动采播。
  bool initial_idle() const;     // 完整原首次输入/输出idle，不从构造推定。
  FocusDriver &driver() {
    return driver_;
  } // 借给同一FocusEndpoint，禁止再装输出仲裁器。
  EndpointFocusPreparation prepare(const AIEndpointFocusScope &scope,
                                   const FocusCommand &command,
                                   std::uint64_t now,
                                   std::uint64_t wall) override;
  void cancel() override; // 仅开始原scope实际关闭，不假报释放。
  bool settled() const override;
  bool control_recovery_supported() const override; // 仅已装配原正式released旁路时支持。
  bool suspend_control(const AIEndpointFocusScope& original, std::uint64_t now,
                       std::uint64_t wall) override; // 与原scope精确关联，不借新socket假ACK。
  std::optional<FocusIdleObservation> control_idle_observation() const override;
  std::optional<FocusPermit> authorize(const FocusCommand &command,
                                       std::uint64_t now,
                                       std::uint64_t wall) override;
  bool valid(const FocusPermit &permit, std::uint64_t now,
             std::uint64_t wall) const override;
  // valid_observed 保留原HAL状态/时钟/authority短路次序，固定首因不持候选身份。
  bool valid_observed(const FocusPermit& permit, std::uint64_t now, std::uint64_t wall,
                      ScopeFailure* observation) const override;
  bool closed_release_valid(const FocusPermit &permit, std::uint64_t now,
                            std::uint64_t wall) const override;

private:
  bool matches(const AIEndpointFocusScope &scope)
      const; // 借用view只在当前调用比较，不存跨生命周期。
  std::optional<BrowserDuplexScope>
  current() const;     // 当前真实候选与固定原绑定的交集。
  bool emit_binding(); // 只发送原可信路由关联，不授予输入许可。
  bool observe_time(std::uint64_t now,
                    std::uint64_t wall); // 双时钟只能前进，异常永久停流。
  Scope scope_;                          // 核心事实读取；缺失默认拒绝。
  Released released_;                    // 纯读取真实原焦点端点，不从网页或bool推断许可。
  BrowserInputRouteKind
      route_; // 原实际装配的输入端口，不从邀请正文决定媒体驱动。
  Emit emit_; // 原唯一桥发布；失败固定关闭。
  BrowserInputOwnershipGate input_; // 唯一输入所有权端口，不另建麦克风。
  DuplexFocusDriver driver_;        // 单路仍使用同一完整组CAS与实际两端票据。
  std::unique_ptr<BrowserDuplexFocusAuthority>
      authority_; // 仅原scope实际绑定后建立。
  std::optional<FocusCommand>
      prepared_; // 固定原准备请求，不让同token变更字段或新请求自行授权。
  std::optional<BrowserDuplexBinding>
      binding_; // 固定候选会话和原焦点，不复用旧路由。
  std::string input_track_, output_track_,
      operation_; // 原核心轨与当前原控制token，仅进程内比较。
  std::uint64_t media_revision_ = 0, deadline_, expires_, now_ = 0,
                wall_ = 0; // 固定宿主及候选媒体代次；双时钟水位不可回退。
  std::uint64_t endpoint_epoch_ = 0, capability_revision_ = 0; // 当前正式控制绑定，与媒体路由分开。
  bool initialized_ = false, cancelled_ = false,
       failed_ = false; // 停止/失败不可逆，不自动重新绑定。
  bool control_recovering_ = false; // 唯一可信失联分支允许原正式release后下一代受限重绑定。
};
} // namespace ur::device
