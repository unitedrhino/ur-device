// 文件说明：原人际会话的显式prepare/ready/commit/abort控制；只更新协议事实，不触发HAL或新Provider。
#include "ur/device/endpoint_handoff_client.hpp"
#include "ur/device/json_parse.hpp"
#include <algorithm>
#include <cJSON.h>
#include <memory>
#include <utility>

namespace ur::device {
namespace {
using Json =
    std::unique_ptr<cJSON,
                    decltype(&cJSON_Delete)>; // 任一失败出口释放有限参数树。
// valid_text 使用协议同一有界ID条件；topic和设备权限另由宿主与平台负责。
bool valid_text(std::string_view value, std::size_t maximum) {
  return !value.empty() && value.size() <= maximum &&
         std::none_of(value.begin(), value.end(),
                      [](unsigned char ch) { return ch < 32 || ch == 127; });
}
} // namespace

bool endpoint_handoff_ingress_ack(std::string_view body) {
  Envelope envelope;
  if (decode_envelope(body, envelope) != DecodeResult::Ok ||
      envelope.method != "requestAck" || envelope.request ||
      envelope.session.empty() || envelope.session.size() > 64 ||
      !envelope.code || envelope.event_sequence || envelope.message_sequence)
    return false;
  std::string_view expected;
  switch (*envelope.code) {
  case 400:
    expected = "invalidRequest";
    break;
  case 403:
    expected = "requestDenied";
    break;
  case 409:
    expected = "requestConflict";
    break;
  case 422:
    expected = "unsupportedMethod";
    break;
  default:
    return false;
  }
  Json data(parse_json(envelope.payload.c_str()), cJSON_Delete);
  if (!data || !cJSON_IsObject(data.get()) ||
      cJSON_GetArraySize(data.get()) != 2)
    return false;
  const auto *method =
      cJSON_GetObjectItemCaseSensitive(data.get(), "requestMethod");
  const auto *reason = cJSON_GetObjectItemCaseSensitive(data.get(), "reason");
  return cJSON_IsString(method) && cJSON_IsString(reason) &&
         std::string_view(method->valuestring) == "endpointControl" &&
         std::string_view(reason->valuestring) == expected;
}

bool EndpointHandoffClient::receive_ingress_rejection(std::string_view body,
                                                      std::uint64_t now) {
  Envelope envelope;
  if (!endpoint_handoff_ingress_ack(body) ||
      decode_envelope(body, envelope) != DecodeResult::Ok ||
      envelope.session != receipt_.session || !envelope.code.has_value())
    return false;
  // 冻结本次已核原拒绝码，账本确认和回执投影共用该值，不依赖跨函数隐含检查。
  const auto code = *envelope.code;
  // 已保存的原拒绝可跨后续新意图重放，但不得回退新请求的状态。
  for (const auto &saved : facts_)
    if (saved.reply && saved.token == envelope.token)
      return saved.body == body;
  const auto bytes = sizeof(Fact) + body.size() + envelope.token.size();
  // receive已用can_receive唯一合同核过账本/容量/总窗；确认仍在同一事件线程，无外部回调。
  if (!requests_.acknowledge({envelope.token, receipt_.session,
                              "endpointControl", code, up_},
                             now))
    return false;
  facts_.push_back({envelope.token, std::string(body), 0, true});
  bytes_ += bytes;
  last_now_ = now;
  receipt_.code = code;
  receipt_.pending = false;
  receipt_.unknown = false;
  // prepare未被受理才允许显式新意图；commit/abort拒绝保留已有ready等事实。
  if (receipt_.operation == "prepare" && receipt_.state.empty())
    receipt_.handoff.clear();
  return true;
}

EndpointHandoffClient::EndpointHandoffClient(PublishPort &port, std::string up,
                                             std::string down,
                                             std::string instance)
    : requests_(port), up_(std::move(up)), down_(std::move(down)),
      instance_(std::move(instance)) {}

bool EndpointHandoffClient::bind(std::string_view session, std::uint64_t now) {
  if (!valid_text(session, 64) || now < last_now_)
    return false;
  if (receipt_.session == session)
    return true;
  close(now);
  receipt_.session = session;
  return true;
}

// bind_committed_candidate 仅首次原候选改绑继承已核CAS；false保留旧账本，不给新意图伪造应答。
bool EndpointHandoffClient::bind_committed_candidate(std::string_view session,
                                                     std::uint64_t revision, std::uint64_t now) {
    constexpr std::uint64_t safe_integer =
        9007199254740991ULL;  // 原跨语言整数边界，不新增协议约束。
    if (!revision || revision > safe_integer || !valid_text(session, 64) || now < last_now_)
        return false;
    if (receipt_.session == session)
        return receipt_.revision >= revision;  // 已发原请求及后续ACK账本不能被旧提交覆盖。
    if (!bind(session, now))
        return false;
    receipt_.revision = revision;  // 只继承接续CAS事实，不伪造本端prepare/commit应答或媒体权限。
    return true;
}

bool EndpointHandoffClient::terminal() const {
  return receipt_.state == "committed" || receipt_.state == "aborted" ||
         receipt_.state == "failed";
}

bool EndpointHandoffClient::prepare(std::string_view target,
                                    std::uint64_t now) {
  if (!valid_text(target, 128) || receipt_.session.empty() ||
      receipt_.pending || receipt_.unknown ||
      (!receipt_.handoff.empty() && !terminal()))
    return false;
  return submit("prepare", target, now);
}

bool EndpointHandoffClient::commit(std::uint64_t now) {
  if (receipt_.state != "ready" || receipt_.candidate.empty() ||
      receipt_.pending || receipt_.unknown || !handoff_started_ || now < *handoff_started_ ||
        now - *handoff_started_ >= 30000)
    return false;
  return submit("commit", {}, now);
}

bool EndpointHandoffClient::abort(std::uint64_t now) {
  if (receipt_.handoff.empty() || terminal() || receipt_.pending ||
      receipt_.unknown ||
        !handoff_started_ || now < *handoff_started_ || now - *handoff_started_ >= 30000)
    return false;
  return submit("abort", {}, now);
}

bool EndpointHandoffClient::submit(std::string operation,
                                   std::string_view target, std::uint64_t now) {
  if (now < last_now_ || !valid_text(instance_, 80) || facts_.size() >= 128 ||
      bytes_ >= 65536)
    return false;
  const auto token = instance_ + "-endpoint-" + std::to_string(serial_ + 1);
  const auto handoff = operation == "prepare" ? token : receipt_.handoff;
  Json params(cJSON_CreateObject(), cJSON_Delete);
  if (!params ||
      !cJSON_AddStringToObject(params.get(), "operation", operation.c_str()) ||
      !cJSON_AddNumberToObject(params.get(), "expectedRevision",
                               static_cast<double>(receipt_.revision)) ||
      !cJSON_AddStringToObject(params.get(), "handoffId", handoff.c_str()))
    return false;
  if (operation == "prepare") {
    auto *party = cJSON_AddObjectToObject(params.get(), "target");
    if (!party || !cJSON_AddStringToObject(party, "kind", "endpoint") ||
        !cJSON_AddStringToObject(party, "id", std::string(target).c_str()))
      return false;
  } else if (operation == "commit" &&
             !cJSON_AddStringToObject(params.get(), "candidateSessionId",
                                      receipt_.candidate.c_str())) {
    return false;
  }
  std::unique_ptr<char, decltype(&cJSON_free)> encoded(
      cJSON_PrintUnformatted(params.get()), cJSON_free);
  if (!encoded)
    return false;
  Envelope request;
  request.method = "endpointControl";
  request.token = token;
  request.session = receipt_.session;
  request.payload = encoded.get();
  std::string body;
  EndpointCommand checked;
  if (encode_envelope(request, body) != DecodeResult::Ok ||
      decode_endpoint_control(WireRoute::Signaling, body, checked) !=
          WireResult::Ok ||
      requests_.submit({token, receipt_.session, "endpointControl", up_, body},
                       now) != SubmitResult::Accepted)
    return false;
  ++serial_;
  last_now_ = now;
  receipt_.handoff = handoff;
  receipt_.operation = std::move(operation);
  receipt_.token = token;
  receipt_.code = 0;
  receipt_.pending = true;
  operation_revision_ = receipt_.revision;
  operation_started_ = now;
  if (receipt_.operation == "prepare") {
      handoff_started_ = now;
      receipt_.state.clear();
    receipt_.candidate.clear();
    receipt_.reason.clear();
  }
  return true;
}

// can_receive 仅核固定当前事务，不触发Requests/发布/水位副作用。
bool EndpointHandoffClient::can_receive(std::string_view topic, std::string_view body,
                                        std::uint64_t now) const {
    if (topic != down_ || now < last_now_ || receipt_.session.empty()) return false;
    if (endpoint_handoff_ingress_ack(body)) {
        Envelope envelope;
        if (decode_envelope(body, envelope) != DecodeResult::Ok ||
            envelope.session != receipt_.session)
            return false;
        for (const auto &saved : facts_)
            if (saved.reply && saved.token == envelope.token) return saved.body == body;
        const auto bytes = sizeof(Fact) + body.size() + envelope.token.size();
        return envelope.token == receipt_.token && receipt_.pending &&
               receipt_.revision == operation_revision_ && facts_.size() < 128 &&
               bytes <= 65536 - bytes_ && now >= operation_started_ &&
               now - operation_started_ < 30000 && handoff_started_ && now >= *handoff_started_ &&
               now - *handoff_started_ < 30000 &&
               requests_.state({up_, "endpointControl", envelope.token}) == RequestState::Pending;
    }
    EndpointState fact;
    if (decode_endpoint_state(WireRoute::Signaling, body, fact) != WireResult::Ok ||
        fact.envelope.session != receipt_.session || receipt_.handoff.empty() ||
        fact.handoff_id != receipt_.handoff)
        return false;
    const bool reply = fact.envelope.code.has_value();
    const auto sequence = fact.envelope.event_sequence.value_or(0);
    for (const auto &saved : facts_)
        if ((saved.reply == reply && saved.token == fact.envelope.token) ||
            (!reply && !saved.reply && saved.sequence == sequence))
            return saved.body == body;
    const bool expired =
        !handoff_started_ || now < *handoff_started_ || now - *handoff_started_ >= 30000;
    if (expired &&
        (reply || (fact.state != "committed" && fact.state != "aborted" && fact.state != "failed")))
        return false;  // 原总窗到期只保留严格原终态事实，不能接ready或新的受理续授权。
    const auto bytes = sizeof(Fact) + body.size() + fact.envelope.token.size();
    const bool historical =
        reply && fact.envelope.token == receipt_.token && fact.revision < receipt_.revision;
    if (reply && *fact.envelope.code == 200 &&
        (fact.revision != operation_revision_ + 1 ||
         (receipt_.operation == "prepare" && fact.state != "preparing") ||
         (receipt_.operation == "commit" &&
          (fact.state != "committed" || fact.candidate_session_id != receipt_.candidate)) ||
         (receipt_.operation == "abort" && fact.state != "aborted")))
        return false;
    if (facts_.size() >= 128 || bytes > 65536 - bytes_ ||
        (fact.revision < receipt_.revision && !historical) ||
        (terminal() && fact.state != receipt_.state && !historical) ||
        (fact.revision == receipt_.revision && !receipt_.state.empty() &&
         (fact.state != receipt_.state || fact.candidate_session_id != receipt_.candidate ||
          fact.reason != receipt_.reason)))
        return false;
    return !reply || (fact.envelope.token == receipt_.token && now >= operation_started_ &&
                      now - operation_started_ < 30000 &&
                      requests_.state({up_, "endpointControl", fact.envelope.token}) ==
                          RequestState::Pending);
}

bool EndpointHandoffClient::receive(std::string_view topic,
                                    std::string_view body, std::uint64_t now) {
  if (!can_receive(topic, body, now))
    return false;
  if (endpoint_handoff_ingress_ack(body))
    return receive_ingress_rejection(body, now);
  EndpointState fact;
  if (topic != down_ || now < last_now_ ||
      decode_endpoint_state(WireRoute::Signaling, body, fact) !=
          WireResult::Ok ||
      receipt_.session.empty() || fact.envelope.session != receipt_.session ||
      receipt_.handoff.empty() || fact.handoff_id != receipt_.handoff)
    return false;
  const bool reply = fact.envelope.code.has_value();
  const auto sequence = fact.envelope.event_sequence.value_or(0);
  for (const auto &saved : facts_)
    if ((saved.reply == reply && saved.token == fact.envelope.token) ||
        (!reply && !saved.reply && saved.sequence == sequence))
      return saved.body == body;
  const auto bytes = sizeof(Fact) + body.size() + fact.envelope.token.size();
  const bool historical_reply = reply &&
                                fact.envelope.token == receipt_.token &&
                                fact.revision < receipt_.revision;
    // can_receive是唯一事务/容量/期限合同；此处只提交已经验证的原事实，不维护第二套条件。
    if (reply) {
    if (fact.envelope.token != receipt_.token ||
        !requests_.acknowledge({fact.envelope.token, receipt_.session,
                                "endpointControl", *fact.envelope.code, up_},
                               now))
      return false;
  }
  facts_.push_back({fact.envelope.token, std::string(body), sequence, reply});
  bytes_ += bytes;
  last_now_ = now;
  if (!historical_reply) {
    receipt_.state = std::move(fact.state);
    receipt_.revision = fact.revision;
    receipt_.candidate = std::move(fact.candidate_session_id);
    receipt_.reason = std::move(fact.reason);
  }
  if (reply) {
    receipt_.code = *fact.envelope.code;
    receipt_.pending = false;
  }
  // 主动终态是平台事实，但不能补造当前操作ACK；未知原结果仍需按原token查证。
  if (terminal())
    receipt_.unknown = false;
  return true;
}

void EndpointHandoffClient::poll(std::uint64_t now) {
  if (now < last_now_)
    return;
  last_now_ = now;
  if (handoff_started_ && !receipt_.handoff.empty() && !terminal() && now >= *handoff_started_ &&
      now - *handoff_started_ >= 30000) {
      requests_.suspend_session(receipt_.session, now);
      receipt_.pending = false;
      receipt_.unknown = true;
  }
  requests_.tick(now);
  if (receipt_.pending &&
      requests_.state({up_, "endpointControl", receipt_.token}) ==
          RequestState::TimedOut) {
    receipt_.pending = false;
    receipt_.unknown = !terminal();
  }
}

// suspend 保留原腿、事务、幂等字节和修订；控制恢复并不证明远端操作没有执行。
void EndpointHandoffClient::suspend(std::uint64_t now) {
  if (now < last_now_) return;
  requests_.suspend_session(receipt_.session, now);
  if (receipt_.pending) {
    receipt_.pending = false;
    receipt_.unknown = !terminal();
  }
  last_now_ = now;
}

void EndpointHandoffClient::close(std::uint64_t now) {
  if (now < last_now_)
    return;
  requests_.cancel_session(receipt_.session, now);
  receipt_ = {};
  facts_.clear();
  bytes_ = 0;
  operation_revision_ = 0;
  operation_started_ = 0;
  handoff_started_.reset();
  last_now_ = now;
}
} // namespace ur::device
