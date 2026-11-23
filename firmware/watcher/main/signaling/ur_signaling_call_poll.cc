// 文件说明：在项目适配层包装select，不修改libpeer/SDK；其他任务、握手和长等待透传。
#include "ur_signaling_call_poll.h"
#include <sys/select.h>
#include <atomic>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {
std::atomic<void*> active_call_task{nullptr};  // 人际媒体单线程装配，其他网络任务不分配TLS。
}
namespace ur::device {
CallPeerPollScope::CallPeerPollScope(bool connected) : active_(connected) {
    if (active_)
        previous_task_ = active_call_task.exchange(xTaskGetCurrentTaskHandle());
}
CallPeerPollScope::~CallPeerPollScope() {
    if (active_)
        active_call_task.store(previous_task_);
}
}  // namespace ur::device

// __real_select 由链接器绑定原SDK实现；参数保持系统select的ABI和错误语义。
extern "C" int __real_select(int nfds, fd_set* read, fd_set* write, fd_set* except,
                             struct timeval* timeout);
// __wrap_select 仅把当前已连接人际任务的短轮询设为零等待，调用者的超时对象不改写。
extern "C" int __wrap_select(int nfds, fd_set* read, fd_set* write, fd_set* except,
                             struct timeval* timeout) {
    const auto active = active_call_task.load();  // 无活动腿时不查询其他网络任务的身份。
    if (active && active == xTaskGetCurrentTaskHandle() && timeout && timeout->tv_sec == 0 &&
        timeout->tv_usec > 0 && timeout->tv_usec <= 1000) {
        struct timeval immediate{};  // 只替换本次调用，保留原始描述符集合及超时内存。
        return __real_select(nfds, read, write, except, &immediate);
    }
    return __real_select(nfds, read, write, except, timeout);
}
