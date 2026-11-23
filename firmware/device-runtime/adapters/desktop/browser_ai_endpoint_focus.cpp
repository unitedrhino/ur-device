// 文件说明：AI候选提交后先绑定实际关闭的双向HAL，再交原FocusEngine授权；准备成功不等于取得。
#include "browser_ai_endpoint_focus.hpp"
#include <algorithm>
#include <array>
#include <cJSON.h>

namespace ur::device {
namespace {
// same_prepared
// 核原严格解码请求的全部控制身份；重试不得换CAS、事件水位、期限或资源。
bool same_prepared(const FocusCommand &left, const FocusCommand &right) {
  return left.envelope.session == right.envelope.session &&
         left.envelope.token == right.envelope.token &&
         left.envelope.event_sequence == right.envelope.event_sequence &&
         left.focus_id == right.focus_id && left.operation == right.operation &&
         left.expected_revision == right.expected_revision &&
         left.priority == right.priority &&
         left.resume_policy == right.resume_policy &&
         left.expires_at == right.expires_at &&
         left.resources.size() == right.resources.size() &&
         std::is_permutation(left.resources.begin(), left.resources.end(),
                             right.resources.begin());
}
} // namespace
// 构造只连接原宿主端口，未初始化前不生成闲置事实或资源许可。
// 保留 boot/deadline/expires 构造合同；boot 标识输入启动代次，deadline 为单调截止，expires 为墙钟截止，各自沿原用途。
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
BrowserAIEndpointFocus::BrowserAIEndpointFocus(OutputGroupGatePort& output, std::uint64_t boot,
                                               std::uint64_t deadline, std::uint64_t expires,
                                               BrowserInputOwnershipGate::Source source,
                                               BrowserInputOwnershipGate::Fence fence, Scope scope,
                                               Emit emit, BrowserInputRouteKind route,
                                               Released released)
    // NOLINTEND(bugprone-easily-swappable-parameters)
    : scope_(std::move(scope)),
      released_(std::move(released)),
      route_(route),
      emit_(std::move(emit)),
      input_(
          boot, std::move(source),
          [this](const InputGateRequest& request) {
              return binding_ && request.session == binding_->session &&
                     request.route_generation == binding_->route_generation &&
                     (!request.enabled || (!cancelled_ && !failed_ && current().has_value()));
          },
          std::move(fence)),
      driver_(input_, {&output}, boot, 0),
      deadline_(deadline),
      expires_(expires) {
    failed_ = route != BrowserInputRouteKind::Udp && route != BrowserInputRouteKind::AISender;
}

// initialize 请求原输出关闭；返回受理结果，不代表双向驱动已关闭。
bool BrowserAIEndpointFocus::initialize(std::uint64_t now) {
  if (initialized_ || failed_ || cancelled_ || !scope_ || !emit_ ||
      now >= deadline_ || !expires_)
    return false;
  now_ = now;
  initialized_ = driver_.initialize(now);
  return initialized_;
}

// receive 将原输入回执交给唯一门控；未知或迟到票据不形成当前实际事实。
bool BrowserAIEndpointFocus::receive(std::string_view body, std::uint64_t now) {
  return initialized_ && !failed_ && input_.receive(body, now);
}

// observe_time 同时固定单调时钟和租约时钟水位，回拨不能延长原授权。
bool BrowserAIEndpointFocus::observe_time(std::uint64_t now,
                                          std::uint64_t wall) {
  if (!initialized_ || failed_ || now < now_ || now >= deadline_ || !wall ||
      wall < wall_ || wall >= expires_) {
    failed_ = true;
    cancel();
    return false;
  }
  now_ = now;
  wall_ = wall;
  return true;
}

// poll 有界推进原驱动并发布门控请求，时钟或发送异常永久关闭。
bool BrowserAIEndpointFocus::poll(std::uint64_t now, std::uint64_t wall) {
  if (!observe_time(now, wall))
    return false;
  if (!input_.poll(now) || !driver_.poll(now)) {
    failed_ = true;
    cancel();
    return false;
  }
  while (auto command = input_.take_command())
    if (!emit_(*command)) {
      failed_ = true;
      cancel();
      return false;
    }
  return true;
}

// initial_idle 仅认本次启动两个真实端口的首次关闭确认。
bool BrowserAIEndpointFocus::initial_idle() const {
  return initialized_ && !failed_ &&
         driver_.initial_idle_observation().has_value();
}

// matches 同步比较原候选身份、轨和媒体代次，不保存借用view。
bool BrowserAIEndpointFocus::matches(const AIEndpointFocusScope &scope) const {
  return binding_ && scope.session_id == binding_->session &&
         scope.input_track == input_track_ &&
         scope.output_track == output_track_ &&
         scope.media_revision == media_revision_ && scope.endpoint_epoch == endpoint_epoch_ &&
         scope.capability_revision == capability_revision_;
}

// current
// 从核心实际候选生成最小权限快照；活腿可释放原完整组，取消后只保留有限关闭资格。
std::optional<BrowserDuplexScope> BrowserAIEndpointFocus::current() const {
  if (!binding_ || !scope_ || (failed_ && !cancelled_))
    return {};
  const auto actual = scope_();
  const bool authorized = !cancelled_ && actual && matches(*actual);
  if (!authorized && !cancelled_)
    return {};
  BrowserDuplexScope value;
  value.binding = *binding_;
  value.authorized = value.peer_ready = authorized;
  value.acquire = authorized;
  value.closing = cancelled_;
  value.release = authorized || cancelled_;
  value.operation_token = operation_;
  return value;
}

// emit_binding 发布原实际输入路由关联，不含云端身份、密钥或采集许可。
bool BrowserAIEndpointFocus::emit_binding() {
  std::unique_ptr<cJSON, decltype(&cJSON_Delete)> value(cJSON_CreateObject(),
                                                        cJSON_Delete);
  if (!value || !binding_ ||
      !cJSON_AddStringToObject(value.get(), "kind", "inputOwnershipBind") ||
      !cJSON_AddStringToObject(value.get(), "sessionId",
                               binding_->session.c_str()) ||
      !cJSON_AddStringToObject(value.get(), "routeGeneration", "1") ||
      !cJSON_AddStringToObject(
          value.get(), "routeKind",
          route_ == BrowserInputRouteKind::AISender ? "aiSender" : "udp"))
    return false;
  std::unique_ptr<char, decltype(&cJSON_free)> bytes(
      cJSON_PrintUnformatted(value.get()), cJSON_free);
  return bytes && emit_(bytes.get());
}

// prepare 先绑定原已关闭整组，再等待两端真实票据；Ready仍不等于acquire。
EndpointFocusPreparation
BrowserAIEndpointFocus::prepare(const AIEndpointFocusScope &scope,
                                const FocusCommand &command, std::uint64_t now,
                                std::uint64_t wall) {
  const std::array<std::string, 2> resources{"microphone", "speaker"};
  if (!observe_time(now, wall) || (cancelled_ && !control_recovering_) || !scope_ ||
      scope.session_id.empty() || scope.input_track.empty() ||
      scope.output_track.empty() || scope.input_track == scope.output_track ||
      scope.media_revision == 0 ||
      command.envelope.session != scope.session_id ||
      command.operation != "acquire" || command.priority != "conversation" ||
      command.resume_policy != "explicit" || command.resources.size() != 2 ||
      !std::is_permutation(command.resources.begin(), command.resources.end(),
                           resources.begin()) ||
      !command.expires_at || *command.expires_at <= wall ||
      *command.expires_at > expires_ ||
      *command.expires_at - wall > deadline_ - now)
    return EndpointFocusPreparation::Failed;
  const auto actual = scope_();
  if (!actual || actual->session_id != scope.session_id ||
      actual->input_track != scope.input_track ||
      actual->output_track != scope.output_track ||
      actual->media_revision != scope.media_revision || actual->endpoint_epoch != scope.endpoint_epoch ||
      actual->capability_revision != scope.capability_revision)
    return EndpointFocusPreparation::Failed;
  // 恢复只能在旧整组正式released ACK已交付、原双false及下一代快照之后换独立focus。
  // 不刷新宿主/原租约期限，不重绑实际媒体route，也不清全局CAS或旧票据。
  if (control_recovering_) {
    if (!binding_ || !prepared_ || !released_ || !released_(binding_->session) ||
        !endpoint_epoch_ || endpoint_epoch_ >= 9007199254740991ULL ||
        scope.endpoint_epoch != endpoint_epoch_ + 1 ||
        scope.capability_revision != capability_revision_ ||
        command.focus_id == binding_->focus ||
        command.expected_revision != driver_.revision() ||
        !command.envelope.event_sequence || !prepared_->envelope.event_sequence ||
        *command.envelope.event_sequence <= *prepared_->envelope.event_sequence ||
        !prepared_->expires_at || *command.expires_at > *prepared_->expires_at ||
        !driver_.replace_released_focus(0, binding_->session, binding_->focus, command.focus_id, now))
      return EndpointFocusPreparation::Failed;
    binding_->focus = command.focus_id;
    endpoint_epoch_ = scope.endpoint_epoch;
    prepared_ = command;
    operation_ = command.envelope.token;
    cancelled_ = control_recovering_ = false;
    authority_ = std::make_unique<BrowserDuplexFocusAuthority>(driver_, *binding_, [this] { return current(); });
  }
  if (!binding_) {
    if (!initial_idle() || command.expected_revision != driver_.revision())
      return EndpointFocusPreparation::Failed;
    binding_ = BrowserDuplexBinding{0,
                                    std::string(scope.session_id),
                                    command.focus_id,
                                    1,
                                    1,
                                    0,
                                    deadline_,
                                    expires_,
                                    BrowserDuplexRole::AI};
    input_track_ = scope.input_track;
    output_track_ = scope.output_track;
    media_revision_ = scope.media_revision;
    endpoint_epoch_ = scope.endpoint_epoch;
    capability_revision_ = scope.capability_revision;
    operation_ = command.envelope.token;
    prepared_ = command;
    if (!input_.bind_route(binding_->session, 1, route_) || !emit_binding() ||
        !driver_.bind_scope(0, binding_->session, binding_->focus, 1, now)) {
      failed_ = true;
      cancel();
      return EndpointFocusPreparation::Failed;
    }
    authority_ = std::make_unique<BrowserDuplexFocusAuthority>(
        driver_, *binding_, [this] { return current(); });
  }
  if (!matches(scope) || !prepared_ || !same_prepared(*prepared_, command))
    return EndpointFocusPreparation::Failed;
  if (!driver_.scope_ready(0))
    return EndpointFocusPreparation::Pending;
  return authority_->admit_binding(*binding_, now, wall)
             ? EndpointFocusPreparation::Ready
             : EndpointFocusPreparation::Failed;
}

// cancel 只请求停止当前原整组，不用同步返回冒认驱动已经释放。
void BrowserAIEndpointFocus::cancel() {
  cancelled_ = true;
  if (binding_)
    driver_.stop_scope(binding_->session, binding_->focus,
                       {"microphone", "speaker"});
}

// settled 必须有原双端关闭事实；失败或未知状态不能报告释放完成。
bool BrowserAIEndpointFocus::settled() const {
  return initialized_ && !failed_ &&
         (binding_ ? driver_.quiescent_scope_ready(0) : initial_idle());
}
// control_recovery_supported缺少正式release读取装配时保持旧候选断连关闭。
bool BrowserAIEndpointFocus::control_recovery_supported() const { return bool(released_); }
// suspend_control先核原实际完整取得并保存有限关闭资格，再请求两端false；不假报已完成。
bool BrowserAIEndpointFocus::suspend_control(const AIEndpointFocusScope& original,
                                             std::uint64_t now, std::uint64_t wall) {
  if (!observe_time(now, wall) || !released_ || !authority_ || !prepared_ ||
      !matches(original) || !original.endpoint_epoch || !original.capability_revision ||
      cancelled_ || control_recovering_ || !driver_.owns(*prepared_) ||
      !authority_->authorize(*prepared_, now, wall)) return false;
  control_recovering_ = true;
  cancel(); // 原scope的实际false异步推进；正式release仍由原FocusEngine执行。
  return true;
}
// control_idle_observation原回执未交付或当前任一端未知时不给基线，绝不把被动stop当release。
std::optional<FocusIdleObservation> BrowserAIEndpointFocus::control_idle_observation() const {
  if (!control_recovering_ || !binding_ || !released_ || !released_(binding_->session)) return {};
  return driver_.released_idle_observation(0);
}

// authorize
// 只签原固定acquire或同原租约release，不接受恢复、暂停或新作用域换取权限。
std::optional<FocusPermit>
BrowserAIEndpointFocus::authorize(const FocusCommand &command,
                                  std::uint64_t now, std::uint64_t wall) {
  if (!observe_time(now, wall) || !authority_ || !binding_ ||
      command.envelope.session != binding_->session ||
      command.focus_id != binding_->focus)
    return {};
  // 平台正常release可早于sessionEnded；交原引擎等待双端false，不先cancel干扰原驱动CAS。
  if (command.operation == "acquire") {
    if (!prepared_ || !same_prepared(*prepared_, command) || cancelled_ ||
        failed_)
      return {};
  } else if (command.operation != "release")
    return {};
  const auto previous =
      operation_; // 非法资源、CAS或期限不能污染原操作关联和持续租约。
  operation_ = command.envelope.token;
  const auto permit = authority_->authorize(command, now, wall);
  if (!permit)
    operation_ = previous;
  return permit;
}

// valid 使用同一原许可、双截止和当前核心权限核对持续有效性。
bool BrowserAIEndpointFocus::valid(const FocusPermit &permit, std::uint64_t now,
                                   std::uint64_t wall) const {
    return valid_observed(permit, now, wall, nullptr);
}
// valid_observed
// 原HAL关闭或时钟拒绝不再读取authority；下一层仍只调用原valid一次。
bool BrowserAIEndpointFocus::valid_observed(const FocusPermit& permit, std::uint64_t now,
                                            std::uint64_t wall, ScopeFailure* observation) const {
    if (failed_)
        return scope_reject(observation, ScopeFailureCode::HALState);
    if (now < now_ || wall < wall_)
        return scope_reject(observation, ScopeFailureCode::HALClock);
    if (!authority_)
        return scope_reject(observation, ScopeFailureCode::HALState);
    return observation ? authority_->valid_observed(permit, now, wall, observation)
                       : authority_->valid(permit, now, wall);
}

// closed_release_valid 不扩权，仅容许已明确关闭的原腿完成实际释放。
bool BrowserAIEndpointFocus::closed_release_valid(const FocusPermit &permit,
                                                  std::uint64_t now,
                                                  std::uint64_t wall) const {
  return !failed_ && now >= now_ && wall >= wall_ && authority_ &&
         authority_->closed_release_valid(permit, now, wall);
}
} // namespace ur::device
