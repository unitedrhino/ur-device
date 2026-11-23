// 从 Watcher fixed_queue.h 移植的有界队列；无 ESP 依赖，空读返回空指针。
// 来源及 MIT 许可见 SOURCES.md 和 LICENSE.watcher。
#pragma once
#include <array>
#include <cstddef>
#include <optional>
#include <utility>

namespace ur::device {
// FixedQueue 由设备事件线程独占；T 可移动，Capacity 是固定容量。
template <typename T, std::size_t Capacity>
class FixedQueue {
    static_assert(Capacity > 0);

public:
    // 构造空队列；禁止复制包含媒体所有权的队列。
    FixedQueue() = default;
    FixedQueue(const FixedQueue&) = delete;
    FixedQueue& operator=(const FixedQueue&) = delete;
    // empty/full/size/capacity 查询队列状态，不改变元素。
    bool empty() const { return size_ == 0; }
    bool full() const { return size_ == Capacity; }
    std::size_t size() const { return size_; }
    static constexpr std::size_t capacity() { return Capacity; }
    // front 空队列返回 nullptr，避免将驱动时序问题变成平台崩溃。
    T* front() { return empty() ? nullptr : &*storage_[head_]; }
    const T* front() const { return empty() ? nullptr : &*storage_[head_]; }
    // push_back 满队列返回 false 且不移动调用者元素，由调用者决定背压策略。
    bool push_back(T&& value) {
        if (full())
            return false;
        storage_[(head_ + size_) % Capacity].emplace(std::move(value));
        ++size_;
        return true;
    }
    // pop_front 释放队首，空队列返回 false。
    bool pop_front() {
        if (empty())
            return false;
        storage_[head_].reset();
        head_ = (head_ + 1) % Capacity;
        --size_;
        return true;
    }
    // clear 幂等释放所有元素，恢复初始写入位置。
    void clear() {
        while (pop_front()) {
        }
        head_ = 0;
    }

private:
    std::array<std::optional<T>, Capacity> storage_;  // 队列独占元素及其释放责任。
    std::size_t head_ = 0;                            // 环形缓冲队首。
    std::size_t size_ = 0;                            // 当前占用，不允许超过容量。
};
}  // namespace ur::device
