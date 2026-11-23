// 文件说明：仅在人际WebRTC已连接轮询期间，将当前任务的1ms select改为非阻塞。
#pragma once

namespace ur::device {
// CallPeerPollScope 标记当前人际媒体任务；握手阶段传false，作用域退出恢复原归属。
class CallPeerPollScope {
public:
    explicit CallPeerPollScope(bool connected);
    ~CallPeerPollScope();
    CallPeerPollScope(const CallPeerPollScope&) = delete;
    CallPeerPollScope& operator=(const CallPeerPollScope&) = delete;

private:
    void* previous_task_ = nullptr;  // 嵌套作用域保存的任务，不借用其他任务的轮询权限。
    bool active_ = false;            // 只有已连接的当前人际腿可以启用零等待。
};
}  // namespace ur::device
