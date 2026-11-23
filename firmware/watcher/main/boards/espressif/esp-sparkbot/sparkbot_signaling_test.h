// 文件说明：仅测试固件使用的坦克 AI 会话和语料注入入口。
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "application.h"
#include "board.h"
#include "display.h"
#include "esp_console.h"
#include "esp_err.h"
#include "esp_random.h"
#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "system_info.h"

// RegisterSparkbotSignalingTestCommands 注册等效触点的对话入口和受控 PCM 语料注入。
// 这些命令仅验证设备协议及交接，不代替真实麦克风和真人听感验收。
inline void RegisterSparkbotSignalingTestCommands() {
    const esp_console_cmd_t chat = {
        .command = "sig_chat",
        .help = "toggle AI chat using the application path",
        .hint = nullptr,
        .func = [](int argc, char**) -> int {
            if (argc != 1) return 1;
            Application::GetInstance().Schedule([] { Application::GetInstance().ToggleChatState(); });
            return 0;
        },
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&chat));
    const esp_console_cmd_t speak = {
        .command = "sig_speak",
        .help = "inject the test PCM corpus into the microphone input path",
        .hint = nullptr,
        .func = [](int argc, char**) -> int {
            if (argc != 1) return 1;
            extern const uint8_t start[] asm("_binary_ur_sig_speak_corpus_wav_start");
            extern const uint8_t end[] asm("_binary_ur_sig_speak_corpus_wav_end");
            const size_t size = end - start;
            size_t offset = 0, pcm_size = 0;
            // WAV 可包含额外 chunk；只读取完整 data 段，不把文件尾部当作语音。
            for (size_t pos = 12; pos + 8 <= size;) {
                const uint32_t len = static_cast<uint32_t>(start[pos + 4]) |
                    (static_cast<uint32_t>(start[pos + 5]) << 8) |
                    (static_cast<uint32_t>(start[pos + 6]) << 16) |
                    (static_cast<uint32_t>(start[pos + 7]) << 24);
                if (len > size - pos - 8) return 1;
                if (std::memcmp(start + pos, "data", 4) == 0) { offset = pos + 8; pcm_size = len; break; }
                const size_t advance = static_cast<size_t>(len) + (len & 1U);
                if (advance > size - pos - 8) return 1;
                pos += 8 + advance;
            }
            constexpr size_t chunk_samples = 160;
            if (!offset || pcm_size < chunk_samples * 2) return 1;
            auto& audio = Application::GetInstance().GetAudioService();
            bool ready = audio.SignalingCaptureEnabled();
            for (int wait = 0; !ready && wait < 150; ++wait) {
                vTaskDelay(pdMS_TO_TICKS(100));
                ready = audio.SignalingCaptureEnabled();
            }
            if (!ready) { printf("sig_speak capture unavailable\n"); return 1; }
            unsigned injected = 0;
            for (size_t i = 0; i + chunk_samples <= pcm_size / 2; i += chunk_samples) {
                std::vector<int16_t> chunk(chunk_samples);
                std::memcpy(chunk.data(), start + offset + i * sizeof(int16_t),
                            chunk_samples * sizeof(int16_t));
                // 经 AFE 输入才能驱动 VAD；直接写采集输出队列会与真实麦克风形成双倍流量。
                if (!audio.InjectMicInput(chunk)) break;
                ++injected;
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            printf("sig_speak injected %u chunks\n", injected);
            return injected ? 0 : 1;
        },
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&speak));
    const esp_console_cmd_t perf = {
        .command = "sig_perf",
        .help = "sample local task CPU use for one second",
        .hint = nullptr,
        .func = [](int argc, char**) -> int {
            if (argc != 1) return 1;
            Application::GetInstance().GetAudioService().TraceSignalingCapture();
            return SystemInfo::PrintTaskCpuUsage(pdMS_TO_TICKS(1000)) == ESP_OK ? 0 : 1;
        },
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&perf));
    const esp_console_cmd_t screen = {
        .command = "sig_screen",
        .help = "capture a test-only JPEG of the local screen",
        .hint = nullptr,
        .func = [](int argc, char**) -> int {
            if (argc != 1) return 1;
            auto* display = Board::GetInstance().GetDisplay();
            std::string jpeg;
            if (!display || !display->SnapshotToJpeg(jpeg, 75) || jpeg.empty() || jpeg.size() > 256 * 1024) {
                printf("[UR_SCREEN] FAIL capture\n");
                return 1;
            }
            const unsigned id = esp_random();
            const unsigned crc = esp_rom_crc32_le(0, reinterpret_cast<const uint8_t*>(jpeg.data()), jpeg.size());
            printf("[UR_SCREEN] BEGIN %08x %u %08x\n", id, unsigned(jpeg.size()), crc);
            for (size_t offset = 0; offset < jpeg.size(); offset += 128) {
                char hex[257];
                const size_t count = std::min(size_t(128), jpeg.size() - offset);
                for (size_t i = 0; i < count; ++i)
                    snprintf(hex + i * 2, 3, "%02x", static_cast<uint8_t>(jpeg[offset + i]));
                printf("[UR_SCREEN] DATA %08x %u %s\n", id, unsigned(offset), hex);
            }
            printf("[UR_SCREEN] END %08x\n", id);
            return 0;
        },
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&screen));
}
