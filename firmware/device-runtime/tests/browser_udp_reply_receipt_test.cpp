// 浏览器UDP安装支持的Ready边界：只验证真实PCM对象，不把软件安装当成扬声器发声。
#include "browser_pcm.hpp"
#include "ur/device/buffered_pcm.hpp"
#include <gtest/gtest.h>

namespace ur::device {
TEST(BrowserReplyInstallReady, RequiresMatchingActualReady) {
    BrowserPcm pcm({16000, 1, 20});  // 未收到实际浏览器Ready的驱动。
    EXPECT_FALSE(pcm.reply_install_ready());
    EXPECT_FALSE(pcm.begin_playback("reply", 1));
    EXPECT_FALSE(pcm.ready({48000, 1, 20}, true));
    EXPECT_FALSE(pcm.reply_install_ready());
    ASSERT_TRUE(pcm.ready({16000, 1, 20}, false));
    EXPECT_TRUE(pcm.reply_install_ready());
    EXPECT_TRUE(pcm.begin_playback("reply", 1));
}

TEST(BrowserReplyInstallReady, DisconnectCannotRestoreSupport) {
    BrowserPcm pcm({16000, 1, 20});  // Ready后断连必须退休原实例。
    ASSERT_TRUE(pcm.ready({16000, 1, 20}, true));
    ASSERT_TRUE(pcm.reply_install_ready());
    pcm.disconnect();
    EXPECT_FALSE(pcm.reply_install_ready());
    EXPECT_FALSE(pcm.ready({16000, 1, 20}, true));
    EXPECT_FALSE(pcm.begin_playback("reply", 1));
}

TEST(BrowserReplyInstallReady, GenericBufferedPortDoesNotAdvertiseHalReady) {
    BufferedPcm pcm({16000, 1, 20});  // 通用缓冲存在不代表真实HAL可安装。
    EXPECT_FALSE(pcm.reply_install_ready());
}
}  // namespace ur::device
