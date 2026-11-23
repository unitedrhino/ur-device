// 设备屏幕通讯录：轻量只读MQTT查询，不分配AI会话或通话媒体；网络状态仅由信令任务推进。
#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ur::device {
class MqttPort;
}  // namespace ur::device

// UrCallContact 只保存已授权联系人展示文字与拨号端点，不保存账号或设备凭据。
struct UrCallContact {
    std::string name;         // 联系人备注、昵称或本人设备名称。
    std::string target;       // 产品:设备名，拨号时仍由平台再次核权。
    std::string device_name;  // 同名联系人有多个设备时用于区分。
};

// UrContactsResult 携带原页面代次，退出或翻页后到达的结果不能改变新页面。
struct UrContactsResult {
    uint32_t revision = 0;                // 发起查询时的菜单页面代次。
    uint32_t offset = 0;                  // 原查询分页位置。
    bool ok = false;                      // 已收到并严格校验成功应答。
    bool more = false;                    // 平台确认仍有下一页。
    std::vector<UrCallContact> contacts;  // 单次最多四项，按值移交显示任务。
};

// UrSignalingContacts 将任意任务的查询请求收敛到现有信令线程，回复与超时均只回调一次。
class UrSignalingContacts {
public:
    // Instance 返回设备级只读查询器，空闲仅持有少量字段。
    static UrSignalingContacts& Instance();
    // SetCallback 注册结果接收者，回调在信令线程执行，接收者只能投递显示任务。
    void SetCallback(std::function<void(UrContactsResult)> callback);
    // Query 复制页面代次和分页位置，true只表示排队，不能作为联系人已加载的证据。
    bool Query(uint32_t revision, uint32_t offset);
    // Attach 由信令线程绑定原MQTT端口和设备身份，instance为本次宿主独立请求前缀。
    void Attach(ur::device::MqttPort* port, std::string identity, std::string instance);
    // Detach 在宿主关闭前取消原请求，并停止受理来自页面的新查询。
    void Detach();
    // Poll 在信令线程发送待查请求并检查十秒截止，绝不等待网络或重建媒体。
    void Poll(uint64_t now);
    // Handle 仅消费原Topic和原token的通讯录ACK，其他报文留给原AI与通话分派。
    bool Handle(std::string_view topic, std::string_view body);

private:
    // Request 是有界请求快照，页面代次与分页位置必须一同复制。
    struct Request {
        uint32_t revision;  // 原页面代次。
        uint32_t offset;    // 已浏览条目数，每页四项。
    };
    // Finish 清除原在途请求后交付结果，避免回调重入导致重复完成。
    void Finish(bool ok, std::vector<UrCallContact> contacts = {}, bool more = false);
    std::mutex mutex_;  // 只保护跨任务邮箱、就绪和回调，不在锁内发布或回调。
    std::function<void(UrContactsResult)> callback_;  // 显示任务邮箱入口。
    std::optional<Request> queued_;                   // 最新一个页面查询，翻页不堆积网络请求。
    bool ready_ = false;                              // 宿主已绑定，供页面排队检查。
    ur::device::MqttPort* port_ = nullptr;            // 以下网络字段仅信令线程访问。
    std::string identity_;                            // 原设备的产品/设备Topic片段。
    std::string instance_;                            // 宿主独立前缀，阻止重建后误接旧ACK。
    std::optional<Request> in_flight_;                // 等待原token回复的查询。
    std::string token_;                               // 当前唯一关联token。
    uint64_t sent_at_ = 0;                            // 已发送请求的单调时刻。
    uint32_t sequence_ = 0;                           // 同一宿主内递增序号。
};
