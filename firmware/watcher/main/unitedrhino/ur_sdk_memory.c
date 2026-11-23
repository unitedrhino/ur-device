/* 联犀 SDK 大块 CPU 缓冲优先使用 PSRAM，保留内部 RAM 给音频 DMA 与网络任务。 */
#include <stdint.h>
#include <esp_heap_caps.h>

/* __real_HAL_Malloc 是链接器保留的 SDK 原始分配入口，处理小块与 PSRAM 不足时的回退。 */
extern void *__real_HAL_Malloc(uint32_t size);

/* __wrap_HAL_Malloc 为含 MQTT 接收数组的客户端等大块分配选择外部 8-bit 内存。 */
void *__wrap_HAL_Malloc(uint32_t size)
{
    if (size >= 16 * 1024) {
        void *buffer = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (buffer != NULL) return buffer;
    }
    return __real_HAL_Malloc(size);
}
