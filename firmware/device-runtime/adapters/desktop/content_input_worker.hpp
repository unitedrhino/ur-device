// 桌面附件单任务工作线程：只读取不可变会话快照，以原打开文件流式哈希/上传；不调用协议运行时。
#pragma once
#include "ur/device/content_upload.hpp"
#include "ur/device/content_wire.hpp"
#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace ur::device {
// ContentInputConfig 保存可信平台origin和原MQTT
// Basic；CA由宿主交给Curl，不进入任务结果。
struct ContentInputConfig {
  std::string origin;        // 仅HTTPS origin，不接收用户命令内地址。
  std::string authorization; // 原设备短期Basic，只交平台metadata请求。
};
// ContentInputSnapshot
// 是事件线程确认的原会话/连接；worker不自行读取VoiceRuntime。
struct ContentInputSnapshot {
  std::string session;    // 原已建立AI会话。
  std::string connection; // 原进程连接实例，不从上传响应获取。
};
// ContentInputCommand
// 区分旧文字和显式附件命令；路径只留在本地任务，不进入日志或协议。
struct ContentInputCommand {
  bool attachment = false;                        // 是否为/image或/file命令。
  InputContentKind kind = InputContentKind::Text; // 明确用户所选模态。
  std::string path;                               // 本机原件路径，不作URL处理。
};
// parse_content_input
// 仅解析两个明确命令；空路径仍为附件命令，后续安全拒绝而不发送文字。
ContentInputCommand parse_content_input(const std::string &line);
// content_input_configured
// 核可信origin/Basic语法，不联网、不探测平台或宣称上传授权。
bool content_input_configured(const ContentInputConfig &config);
// content_input_basic
// 从同一MQTT用户名/密码构造Basic，不使用设备secret或自报owner。
std::string content_input_basic(const std::string &username,
                                const std::string &password);
// ContentInputCompletion
// 只携带原快照、固定结论与Ready类型引用；没有路径/正文/地址/receipt。
struct ContentInputCompletion {
  ContentInputSnapshot original; // 供事件线程重新核原session和连接。
  ContentUploadResult result =
      ContentUploadResult::Invalid; // 上传器原固定结论。
  InputContentPart part; // 只有Ready时可提交，仍须复核supports_content。
};
// content_input_current_result由事件线程传入现会话和实际平台交集；旧快照/撤销/非附件不能提交。
bool content_input_current_result(const ContentInputCompletion &result,
                                  const ContentInputSnapshot &current,
                                  bool supported);
// ContentInputWorker
// 同时最多一个任务/结果，cancel中断哈希和HTTP；析构取消后join原线程。
class ContentInputWorker {
public:
  // config不可变，http寿命须长于worker；构造本身不启动任务或创建网络资源。
  ContentInputWorker(HttpPort &http, ContentInputConfig config);
  ~ContentInputWorker();
  ContentInputWorker(const ContentInputWorker &) = delete;
  ContentInputWorker &operator=(const ContentInputWorker &) = delete;
  // start复制事件线程已核快照；忙/非法配置返回false，绝不排第二任务或自动重试。
  bool start(ContentInputCommand command, ContentInputSnapshot original);
  // cancel由事件线程在取消、断线、会话结束时调用；不可将旧任务重新标成current。
  void cancel();
  // busy包含未领取结果，防止新任务覆盖上一任务终态。
  bool busy() const { return busy_.load(); }
  // take仅由事件线程领取一次已完成结果并join，不等待正在执行的任务。
  std::optional<ContentInputCompletion> take();

private:
  // execute只使用值快照与atomic许可；所有异常收敛为固定失败，不能外泄自由异常正文。
  void execute(ContentInputCommand command,
               ContentInputSnapshot original) noexcept;
  HttpPort &http_;                  // 有界同步HTTP端口，不由协议线程复用。
  const ContentInputConfig config_; // 原平台配置，任务不修改。
  std::atomic_bool cancel_{false}, current_{false}, busy_{false},
      finished_{false}; // 单向任务撤销/终态。
  std::thread thread_;  // 仅一个工作线程，没有线程池或常驻重试。
  std::mutex mutex_;    // 仅保护完成结果；不跨文件/HTTP IO持锁。
  std::optional<ContentInputCompletion> completion_; // 单个不可变结果槽。
};
} // namespace ur::device
