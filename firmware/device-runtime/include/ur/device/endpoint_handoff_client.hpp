// 文件说明：单事件线程驱动的设备接续控制器；保持原请求身份和平台事实，不操作音视频HAL或重开原通话。
#pragma once
#include "ur/device/endpoint_wire.hpp"
#include "ur/device/requests.hpp"
#include <string>
#include <optional>
#include <string_view>
#include <vector>

namespace ur::device {
// endpoint_handoff_ingress_ack 严格识别DMS对endpointControl的负向入口回执；
// 仅供宿主分流，不校验当前会话/请求归属，也不代表事务已失败或修订已推进。
bool endpoint_handoff_ingress_ack(std::string_view body);
// EndpointHandoffReceipt
// 投影同一原会话的接续事实；unknown只表示等待结果未知，不意味着远端未提交。
struct EndpointHandoffReceipt {
  std::string session, handoff, state, candidate,
      reason;                   // 固定严格平台字段，不含SDP或自由错误正文。
  std::string operation, token; // 当前原请求身份；重试必须沿同一token及原字节。
  std::uint64_t revision = 0;   // 接续修订，不代替会话全局eventSeq。
  int code = 0;                 // 当前操作实际应答码，主动事件不得伪造ACK。
  bool pending = false, unknown = false; // 本地等待与未知状态，不授予目标媒体。
};
// EndpointHandoffClient
// 绑定宿主已确认的人际源会话；同一设备共享原MQTT连接，不自行连接或订阅。
class EndpointHandoffClient {
public:
  EndpointHandoffClient(PublishPort &port, std::string up, std::string down,
                        std::string instance);
  // bind
  // 仅由唯一宿主在原HumanCall会话更换时调用；不得用网页自报session替代当前原腿。
  bool bind(std::string_view session, std::uint64_t now);
  // prepare 声明同主人目标设备，不停止原端；权限和独立候选归属由平台正式核验。
  bool prepare(std::string_view target, std::uint64_t now);
  // commit
  // 仅在严格平台ready后发送原候选；操作ACK不代替后续原sessionEnded及HAL停止。
  bool commit(std::uint64_t now);
  // abort 只撤当前未终结事务，不能撤回已提交授权或恢复用户已停的采集。
  bool abort(std::uint64_t now);
  // receive 接纳本源会话、当前handoff的严格应答/事件；重复原字节不回退事实。
  bool receive(std::string_view topic, std::string_view body,
               std::uint64_t now);
  // can_receive 只读复核原事务/账本/应答期限，供宿主先核全腿水位后一次提交。
  bool can_receive(std::string_view topic, std::string_view body, std::uint64_t now) const;
  // poll 以原单调时钟推进Requests；到期仅锁存unknown，不换token再次切端。
  void poll(std::uint64_t now);
  // suspend 取消旧socket在途重传但保留原事务事实；未知结果不能按新token重新切端。
  void suspend(std::uint64_t now);
  // close
  // 取消本地等待并清原腿，不能声称撤销远端事务；HAL仍由HumanCall生命周期释放。
  void close(std::uint64_t now);
  // receipt 只读当前原操作和平台事实，供本地界面与验收取证。
  const EndpointHandoffReceipt &receipt() const { return receipt_; }

private:
  // Fact
  // 保存本会话有限完整原字节；相同token/序号改正文必须拒绝，不驱逐事实后二次执行。
  struct Fact {
    std::string token, body;    // 平台原关联和正文，严禁日志输出。
    std::uint64_t sequence = 0; // 0表示应答；事件水位由原HumanCall统一观察。
    bool reply = false;         // ACK和同token主动事实不能混为一个关联键。
  };
  // submit 先严格验证请求，再由原Requests提交；失败不替换当前操作或分配媒体。
  bool submit(std::string operation, std::string_view target,
              std::uint64_t now);
  // terminal 仅识别平台已确认的事务终态，不根据本地30秒等待制造failed。
  bool terminal() const;
  // receive_ingress_rejection
  // 接纳当前原请求的入口拒绝，不补造接续终态或授权变更。
  bool receive_ingress_rejection(std::string_view body, std::uint64_t now);
  Requests requests_; // 原请求关联和字节重试，不能绕过固定幂等保护。
  std::string up_, down_, instance_; // 精确同设备Topic与本进程唯一token前缀。
  EndpointHandoffReceipt receipt_;   // 原当前腿的只读投影。
  std::vector<Fact> facts_; // 至多128条、64KiB；满则拒绝，不丢幂等事实。
  std::size_t bytes_ = 0;   // 有效对象和字符串预算，不声称精确堆峰值。
  std::uint64_t serial_ = 0, last_now_ = 0; // 本进程请求号及单调时钟围栏。
  std::uint64_t operation_revision_ =
      0; // 当前操作首次发送前修订，原ACK不得被新事件改写。
  std::uint64_t operation_started_ = 0;  // 原请求首次时间；只读ACK预检不能刷新30秒期限。
  std::optional<std::uint64_t>
      handoff_started_;       // 原prepare首次时间；后继commit/abort不能续总完成窗。
  friend class VoiceRuntime;  // AI核心只继承已核候选水位，网页没有写入入口。
  friend class HumanCallClient; // 人际核心只在原邀请出处、本人接听及实际完整组成立后继承水位。
  // bind_committed_candidate 首次改绑已实际Active的公开腿时继承原提交水位；
  // 同腿后续意图保持原操作、未知结果及更高修订，不被旧committed回退或清除。
  bool bind_committed_candidate(std::string_view session, std::uint64_t revision,
                                std::uint64_t now);
};
} // namespace ur::device
