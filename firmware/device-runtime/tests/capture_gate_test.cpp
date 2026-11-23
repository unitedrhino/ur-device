// 文件说明：物理采集组预约回归；不把预约或队列排入当作实际媒体成功。
#include "ur/device/capture_gate.hpp"
#include <gtest/gtest.h>

namespace ur::device {
// 整组预约失败不留下视频部分租约，纯视频可在实际预算内并行。
TEST(CaptureGateTest, AtomicConflictAndCameraBudget) {
    CaptureGate gate(3);
    const auto talk = gate.acquire(true, true, false);
    ASSERT_TRUE(talk);
    EXPECT_FALSE(gate.acquire(true, true, true));
    EXPECT_FALSE(gate.acquire(true, false, true));
    const auto first = gate.acquire(false, false, true);
    const auto second = gate.acquire(false, false, true);
    const auto third = gate.acquire(false, false, true);
    ASSERT_TRUE(first && second && third);
    EXPECT_FALSE(gate.acquire(false, false, true));
    EXPECT_TRUE(gate.release(*talk));
    EXPECT_TRUE(gate.acquire(true, true, false));
}
// 原租约不重复使用，迟到释放不能影响新的麦克风所有者。
TEST(CaptureGateTest, OldReleaseCannotFreeNewOwner) {
    CaptureGate gate(1);
    const auto old = gate.acquire(true, true, true);
    ASSERT_TRUE(old);
    ASSERT_TRUE(gate.release(*old));
    const auto current = gate.acquire(true, true, true);
    ASSERT_TRUE(current);
    EXPECT_NE(old, current);
    EXPECT_FALSE(gate.release(*old));
    EXPECT_FALSE(gate.available(true, true, true));
    EXPECT_TRUE(gate.release(*current));
    EXPECT_TRUE(gate.available(true, true, true));
}
// 非法配置和无采集意义的组不能获得任何资源。
TEST(CaptureGateTest, InvalidGroupFailsClosed) {
    CaptureGate zero(0), excessive(5), valid(4);
    EXPECT_FALSE(zero.acquire(true, true, true));
    EXPECT_FALSE(excessive.acquire(false, false, true));
    EXPECT_FALSE(valid.acquire(false, true, true));
    EXPECT_FALSE(valid.acquire(false, false, false));
    EXPECT_FALSE(valid.release(0));
    EXPECT_TRUE(valid.available(true, true, true));
}
}  // namespace ur::device
