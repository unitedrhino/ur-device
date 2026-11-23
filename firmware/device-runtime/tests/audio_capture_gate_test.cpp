// 文件说明：采集吞吐窗口区分实际过载、正常50fps及本人静音，防止静音被误当故障。
#include "ur/device/udp_av.hpp"
#include <gtest/gtest.h>

namespace ur::device {
// 正常批量交付允许轻微计时抖动，预热后仍不触发停显。
TEST(AudioCaptureGate, HealthyAndSlowWindows) {
    AudioCaptureGate gate;
    EXPECT_FALSE(gate.overloaded(0,0));
    EXPECT_FALSE(gate.overloaded(1000,50));
    EXPECT_FALSE(gate.overloaded(2000,100));
    EXPECT_FALSE(gate.overloaded(3000,150));
    EXPECT_TRUE(gate.overloaded(4000,175));
}
// 预热不以短窗口判失败；静音结束后的恢复窗口也独立重新预热。
TEST(AudioCaptureGate, WarmupAndMutedResume) {
    AudioCaptureGate gate;
    EXPECT_FALSE(gate.overloaded(100,0));
    EXPECT_FALSE(gate.overloaded(1100,5));
    EXPECT_FALSE(gate.overloaded(2100,10));
    EXPECT_FALSE(gate.overloaded(3100,10,true));
    EXPECT_FALSE(gate.overloaded(10000,10,true));
    EXPECT_FALSE(gate.overloaded(11000,10));
    EXPECT_FALSE(gate.overloaded(12000,10));
    EXPECT_TRUE(gate.overloaded(13000,10));
}

// 首个非零批次从采集真正启动时重新预热，不能继承媒体腿建立前的空窗。
TEST(AudioCaptureGate, FirstAudioBatchStartsWarmup) {
    AudioCaptureGate gate;
    EXPECT_FALSE(gate.overloaded(0, 0));
    EXPECT_FALSE(gate.overloaded(3000, 101));
    EXPECT_FALSE(gate.overloaded(4000, 151));
    EXPECT_FALSE(gate.overloaded(5000, 201));
    EXPECT_FALSE(gate.overloaded(6000, 251));
}
// 时钟和计数重新起始不能把旧会话的时间差投影到新采集。
TEST(AudioCaptureGate, ResetFactsAndBoundary) {
    AudioCaptureGate gate;
    EXPECT_FALSE(gate.overloaded(5000,500));
    EXPECT_FALSE(gate.overloaded(4000,400));
    EXPECT_FALSE(gate.overloaded(5000,450));
    EXPECT_FALSE(gate.overloaded(6000,500));
    EXPECT_FALSE(gate.overloaded(7000,535));
    EXPECT_TRUE(gate.overloaded(8000,569));
}
} // namespace ur::device
