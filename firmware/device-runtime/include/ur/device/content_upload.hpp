// 设备附件三阶段上传核心；不持有平台身份或MQTT状态，原件只进入流式HTTPS端口。
#pragma once
#include "ur/device/http_port.hpp"

namespace ur::device {
// ContentUploadRuntime 由宿主提供校时和原会话存活快照；不能借旧会话授权新上传。
class ContentUploadRuntime {
public:
  virtual ~ContentUploadRuntime() = default;
  // monotonic_ms 用于固定总预算，不能随Prepare、PUT或Complete重试刷新。
  virtual std::uint64_t monotonic_ms() const = 0;
  // unix_ms 用于原签名截止；设备网络可用后须先可靠校时。
  virtual std::uint64_t unix_ms() const = 0;
  // current 必须检查本次原session及连接仍有效；关闭/换连接即返回false。
  virtual bool current() const = 0;
};
// ContentUploadRequest
// 由已协商image/file的原会话生成，不接受浏览器自报设备凭据。
struct ContentUploadRequest {
  std::string api_origin; // 受控平台HTTPS origin，不含路径、查询或userinfo。
  std::string
      authorization; // 当前worker借用的短期Basic认证，不发给OSS或进入结果。
  std::string session_id; // 原已建立AI会话，上传完成不自动提交模型输入。
  std::string request_id; // 原幂等键；调用方重试须保持全部元数据相同。
  std::string name;       // 展示文件名，不是设备文件路径。
  std::string mime_type;  // 平台支持的真实原件类型，Complete仍回读核验。
  std::string sha256;     // 调用方从同一不可变原件计算的64位小写hex摘要。
  std::uint64_t size = 0; // 精确原件长度；图片20MiB、文件50MiB。
  std::uint32_t timeout_ms = 90000; // 整条业务固定总预算，1～120000ms。
};
// ContentUploadResult
// 仅返回稳定分类；失败不回显请求、签名URL、收据或服务端正文。
enum class ContentUploadResult {
  Ready,
  Invalid,
  Busy,
  Inactive,
  Cancelled,
  Timeout,
  TransportFailed,
  Denied,
  Unsupported,
  Conflict,
  ServiceFailed,
  InvalidReply
};
// ContentUploadStage 标明本次截止所在阶段，不推测失败PUT是否已写入OSS。
enum class ContentUploadStage { Prepare, Put, Complete };
// ContentUploadOutcome 只有Ready携带可提交引用，既不是URL也不表示模型已消费。
struct ContentUploadOutcome {
  ContentUploadResult result = ContentUploadResult::Invalid;
  ContentUploadStage stage = ContentUploadStage::Prepare;
  HttpResult transport_result = HttpResult::Ok; // 原HTTP端口的稳定诊断。
  std::string content_ref; // 仅Ready有效；不含对象键或租约。
  std::string type;        // image/file。
  std::string mime_type;   // Complete核验通过的MIME。
};
// ContentUploader
// 不复制原件或自动重试；宿主只在单独worker调用，并在事件线程提交结果。
class ContentUploader {
public:
  // 构造时借用HTTPS端口和原上传运行时，二者须存活到upload返回。
  ContentUploader(HttpPort &http, ContentUploadRuntime &runtime)
      : http_(http), runtime_(runtime) {}
  // upload 顺序执行Prepare→私有PUT→Complete；每阶段复核原会话和固定截止。
  ContentUploadOutcome upload(const ContentUploadRequest &request,
                              HttpBodySource &source,
                              const std::atomic_bool &cancel);

private:
  HttpPort &http_;                // 唯一平台网络端口，不创建第二套HTTP驱动。
  ContentUploadRuntime &runtime_; // 借用原会话快照，不创建上传专属授权。
  std::atomic_bool busy_{false};  // 拒绝同实例并发及同步重入。
};
} // namespace ur::device
