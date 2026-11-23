#include <srtp.h> /* esp_libsrtp 头路径为 srtp.h（非上游 srtp2/），API 2.x 兼容 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <esp_heap_caps.h>

#include "peer.h"
#include "sctp.h"
#include "utils.h"

int peer_init() {
  /* srtp_init 已初始化时重入会重跑全部加密自检（每个 cipher/auth 分配上下文与
   * 测试缓冲），ESP32 堆碎片期必现 alloc 失败（真机 rc=2 实证，此时总空闲仍有
   * 27KB 但最大连续块不足）。本工程无人调用 srtp_shutdown（peer_deinit 为死代
   * 码），内核一经初始化即常驻：进程内只执行一次 srtp_init。
   * 失败同样只尝试一次（门先置位再自检）：重试无助于从堆碎片恢复，只会让每次
   * 建链都重跑全套自检反复 churn 堆，失败仅保留 ERROR 日志便于定位。 */
  static bool s_srtp_checked = false;
  if (!s_srtp_checked) {
    s_srtp_checked = true;
    const srtp_err_status_t srtp_rc = srtp_init();
    if (srtp_rc != srtp_err_status_ok) {
      LOGE("libsrtp init failed rc=%d free_heap=%u internal_largest=%u", (int)srtp_rc,
           (unsigned)esp_get_free_heap_size(),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
      return -1;
    }
  }
  sctp_usrsctp_init();
  return 0;
}

void peer_deinit() {
  srtp_shutdown();
  sctp_usrsctp_deinit();
}
