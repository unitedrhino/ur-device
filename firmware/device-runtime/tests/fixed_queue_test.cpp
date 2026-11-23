// Watcher 派生队列测试：空读、满队列背压、环绕及媒体所有权释放。
#include "ur/device/fixed_queue.hpp"
#include <gtest/gtest.h>
#include <memory>

namespace ur::device {
TEST(FixedQueue, EmptyIsSafeAndClearIsIdempotent) {
    FixedQueue<int, 2> queue;
    const auto& read_only = queue;
    EXPECT_TRUE(queue.empty());
    EXPECT_EQ(queue.front(), nullptr);
    EXPECT_EQ(read_only.front(), nullptr);
    EXPECT_FALSE(queue.pop_front());
    queue.clear();
    queue.clear();
    EXPECT_EQ(queue.size(), 0U);
    EXPECT_EQ(queue.capacity(), 2U);
}
TEST(FixedQueue, WraparoundMaintainsOrderAndBounds) {
    FixedQueue<int, 2> queue;
    for (int index = 0; index < 100; index += 2) {
        ASSERT_TRUE(queue.push_back(int(index)));
        ASSERT_TRUE(queue.push_back(int(index + 1)));
        EXPECT_TRUE(queue.full());
        EXPECT_FALSE(queue.push_back(999));
        EXPECT_EQ(*queue.front(), index);
        ASSERT_TRUE(queue.pop_front());
        const auto& read_only = queue;
        EXPECT_EQ(*read_only.front(), index + 1);
        ASSERT_TRUE(queue.pop_front());
    }
}
TEST(FixedQueue, FullDoesNotStealCallerOwnershipAndClearDestroys) {
    FixedQueue<std::shared_ptr<int>, 1> queue;
    auto first = std::make_shared<int>(1);
    std::weak_ptr<int> lifetime = first;
    ASSERT_TRUE(queue.push_back(std::move(first)));
    auto second = std::make_shared<int>(2);
    EXPECT_FALSE(queue.push_back(std::move(second)));
    EXPECT_NE(second, nullptr);
    EXPECT_FALSE(lifetime.expired());
    queue.clear();
    EXPECT_TRUE(lifetime.expired());
    EXPECT_TRUE(queue.push_back(std::move(second)));
}
}  // namespace ur::device
