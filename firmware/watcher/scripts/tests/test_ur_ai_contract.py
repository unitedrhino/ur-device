"""联犀设备语音协议与 devicesim oracle 的静态合同回归。"""

import os
import re
import struct
import unittest
from pathlib import Path


WATCHER_ROOT = Path(__file__).resolve().parents[2]
SAAS_ROOT = Path(os.environ["UR_SAAS_ROOT"]).resolve() if os.environ.get("UR_SAAS_ROOT") else None
PROTOCOL = (WATCHER_ROOT / "main/protocols/ur_ai_protocol.cc").read_text(encoding="utf-8")
PROTOCOL_HEADER = (WATCHER_ROOT / "main/protocols/ur_ai_protocol.h").read_text(
    encoding="utf-8"
)
UDP_CHANNEL = (WATCHER_ROOT / "main/protocols/udp_audio_channel.cc").read_text(encoding="utf-8")
RUNTIME = (WATCHER_ROOT / "main/protocols/ur_ai_runtime.cc").read_text(encoding="utf-8")
DEVICE = (WATCHER_ROOT / "main/unitedrhino/ur_device.c").read_text(encoding="utf-8")
APPLICATION = (WATCHER_ROOT / "main/application.cc").read_text(encoding="utf-8")
AUDIO_SERVICE = (WATCHER_ROOT / "main/audio/audio_service.cc").read_text(encoding="utf-8")
WATCHER_CONFIG = (
    WATCHER_ROOT / "main/boards/sensecap-watcher/config.json"
).read_text(encoding="utf-8")
WATCHER_BUILDER = (WATCHER_ROOT / "build-watcher.sh").read_text(encoding="utf-8")
POWER_SAVE_TIMER = (
    WATCHER_ROOT / "main/boards/common/power_save_timer.cc"
).read_text(encoding="utf-8")
POWER_SAVE_HEADER = (
    WATCHER_ROOT / "main/boards/common/power_save_timer.h"
).read_text(encoding="utf-8")
WATCHER_BOARD = (
    WATCHER_ROOT / "main/boards/sensecap-watcher/sensecap_watcher.cc"
).read_text(encoding="utf-8")
E2E_FIXTURE = WATCHER_ROOT / "main/testdata/ur_ai_query_volume.opuspack"
E2E_FIXTURE_PARSER = (
    WATCHER_ROOT / "main/protocols/ur_ai_e2e_fixture.cc"
).read_text(encoding="utf-8")



class UrAiContractTest(unittest.TestCase):
    """防止固件协议在后续改动中偏离 devicesim 已验证合同。"""

    def saas_source(self, path):
        """跨仓合同需要显式提供 SaaS 源码；缺失时单独标记跳过。"""
        if SAAS_ROOT is None:
            self.skipTest("跨仓合同需要设置 UR_SAAS_ROOT")
        source = SAAS_ROOT / path
        if not source.is_file():
            self.fail(f"UR_SAAS_ROOT 缺少合同源文件: {source}")
        return source.read_text(encoding="utf-8")

    def test_watcher_release_default_excludes_test_commands(self):
        """Watcher 正式入口默认新版本并关闭串口测试；实际镜像还须单独核验。"""
        self.assertIn('UR_FIRMWARE_VERSION="${UR_FIRMWARE_VERSION:-2.8.3}"', WATCHER_BUILDER)
        self.assertIn('ENABLE_UR_AI_E2E_TEST="${ENABLE_UR_AI_E2E_TEST:-0}"', WATCHER_BUILDER)

    def test_emotion_map_matches_devicesim(self):
        """固件 21 项映射必须逐项等于 devicesim，并保留 neutral 兜底。"""
        oracle_block = re.search(
            r"var emojiWhitelist = map\[string\]string\{(.*?)\n\}",
            self.saas_source("backend/things/tools/devicesim/emotion_test.go"),
            re.S,
        )
        self.assertIsNotNone(oracle_block)
        oracle = dict(re.findall(r'"([^"]+)":\s*"([^"]+)",', oracle_block.group(1)))

        firmware_block = re.search(
            r"constexpr std::array<EmotionPair, 21> kEmotionMap = \{\{(.*?)\}\};",
            RUNTIME,
            re.S,
        )
        self.assertIsNotNone(firmware_block)
        firmware = dict(re.findall(r'\{"([^"]+)",\s*"([^"]+)"\}', firmware_block.group(1)))
        self.assertEqual(len(oracle), 21)
        self.assertEqual(firmware, oracle)
        self.assertIn('return "neutral";', RUNTIME)
        self.assertIn("UrAiNormalizeEmotion", PROTOCOL)

        watcher_assets = {
            path.stem
            for path in (
                WATCHER_ROOT
                / "managed_components/78__xiaozhi-fonts/png/noto-color-emoji_128"
            ).glob("*.png")
        }
        self.assertEqual(set(firmware.values()), watcher_assets)

    def test_emotion_lifecycle_waits_for_playback_drain(self):
        """表情到达时退出预览并唤醒，Listening 不提前覆盖，播放排空后才恢复。"""
        llm_block = APPLICATION.split('strcmp(type->valuestring, "llm")', 1)[1].split(
            'strcmp(type->valuestring, "mcp")', 1
        )[0]
        self.assertIn("SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE)", llm_block)
        self.assertIn("display->SetPreviewImage(nullptr)", llm_block)
        self.assertIn("ur_emotion_hold_ = true", llm_block)
        self.assertIn("ur_emotion_release_pending_ = true", APPLICATION)
        self.assertIn("ReleaseUrEmotionIfReady();", APPLICATION)
        listening_block = APPLICATION.split("case kDeviceStateListening:", 1)[1].split(
            "case kDeviceStateSpeaking:", 1
        )[0]
        self.assertIn("if (!ur_emotion_hold_)", listening_block)
        release_block = APPLICATION.split("void Application::ReleaseUrEmotionIfReady", 1)[
            1
        ].split("#endif", 1)[0]
        self.assertIn("audio_service_.IsPlaybackIdle()", release_block)
        self.assertIn("kMinimumEmotionVisibleUs", release_block)
        self.assertIn("pending_listening_start_", release_block)
        playback_block = APPLICATION.split("if (bits & MAIN_EVENT_PLAYBACK_DRAINED)", 1)[
            1
        ].split("if (bits & MAIN_EVENT_TOGGLE_CHAT)", 1)[0]
        self.assertIn("!ur_emotion_hold_", playback_block)

    def test_session_and_turn_correlation_contract(self):
        """关键请求按 token、回复按 session/respId 关联并缓存乱序终态。"""
        for method in (
            "sessionClose",
            "sessionCreate",
            "sessionCreated",
            "audioStart",
            "audioStarted",
            "audioStop",
            "respCancel",
            "respCreated",
            "respSttDone",
            "respTextDone",
            "respAudioStart",
            "respAudioDone",
            "respEmotion",
        ):
            self.assertIn(f'"{method}"', PROTOCOL)
        self.assertIn("token == expected_session_token_", PROTOCOL)
        self.assertIn("token == expected_audio_token_", PROTOCOL)
        self.assertIn("token == expected_close_token_", PROTOCOL)
        self.assertIn("MatchesCurrentSession(root, data)", PROTOCOL)
        self.assertIn("UrAiClassifyResponse", PROTOCOL)
        self.assertIn("deferred_response_messages_.push_back", PROTOCOL)
        self.assertIn("(!is_stt && strncmp(method, \"resp\", 4) == 0)", PROTOCOL)
        self.assertNotIn('cJSON_AddStringToObject(params, "version"', PROTOCOL)
        self.assertNotIn("cJSON_AddNullToObject(root, \"params\")", PROTOCOL)

    def test_udp_preheat_matches_devicesim_timing(self):
        """两帧静音按真实帧间隔发送，并给服务端完整的路由预热窗口。"""
        oracle = self.saas_source("backend/things/tools/devicesim/udp.go")
        self.assertIn("time.Sleep(time.Duration(frameDurationMs) * time.Millisecond)", oracle)
        client_oracle = self.saas_source("backend/things/tools/devicesim/client.go")
        self.assertIn("time.Sleep(1 * time.Second)", client_oracle)
        self.assertIn("vTaskDelay(pdMS_TO_TICKS(60));", PROTOCOL)
        self.assertIn("vTaskDelay(pdMS_TO_TICKS(1000));", PROTOCOL)

    def test_queue_only_drops_text_delta(self):
        """MQTT 回调只复制；队列拥塞时仅允许丢弃 respTextDelta。"""
        self.assertIn('find("\\\"method\\\":\\\"respTextDelta\\\"")', PROTOCOL)
        self.assertIn("terminal_overflow_.push_back", PROTOCOL)
        self.assertIn("callback((const char *)message->payload", DEVICE)
        self.assertIn("xQueueSend(s_ai_publish_queue", DEVICE)
        callback_block = re.search(
            r"static void ur_ai_message_cb\(.*?\n\}", DEVICE, re.S
        )
        self.assertIsNotNone(callback_block)
        self.assertNotIn("cJSON_Parse", callback_block.group(0))

    def test_out_of_order_audio_is_buffered_and_drained(self):
        """audio start 乱序和 MQTT/UDP 尾帧抖动不能截掉回复首尾。"""
        self.assertIn("pending_incoming_audio_frames_.push_back", PROTOCOL)
        self.assertIn("deferred_audio_done_message_ = payload", PROTOCOL)
        self.assertIn("FlushPendingIncomingAudio();", PROTOCOL)
        self.assertIn("vTaskDelay(pdMS_TO_TICKS(180));", PROTOCOL)
        self.assertIn("(!response_pending_ && current_resp_id_.empty())", PROTOCOL)

    def test_first_audio_resets_decoder_before_speaking(self):
        """联犀首帧必须先重置解码器，状态回调不得再次清空已排队音频。"""
        tts_start = re.search(
            r'if \(strcmp\(state->valuestring, "start"\) == 0\) \{(.*?)'
            r'\} else if \(strcmp\(state->valuestring, "stop"\) == 0\)',
            APPLICATION,
            re.S,
        )
        self.assertIsNotNone(tts_start)
        block = tts_start.group(1)
        self.assertLess(block.index("audio_service_.ResetDecoder()"), block.index("SetDeviceState"))

        speaking = re.search(
            r"case kDeviceStateSpeaking:(.*?)\n\s*break;", APPLICATION, re.S
        )
        self.assertIsNotNone(speaking)
        self.assertIn("#if !CONFIG_UR_AI", speaking.group(1))
        self.assertIn("audio_service_.ResetDecoder()", speaking.group(1))

    def test_watcher_uses_multiline_transcript(self):
        """Watcher 的完整 STT/助手文本必须换行展示，不能只显示单行片段。"""
        self.assertIn('"CONFIG_USE_MULTILINE_CHAT_MESSAGE=y"', WATCHER_CONFIG)
        self.assertGreaterEqual(
            WATCHER_BUILDER.count('CONFIG_USE_MULTILINE_CHAT_MESSAGE=y'), 2
        )

    def test_platform_ota_releases_audio_stacks_and_recovers_after_failure(self):
        """语音版 OTA 必须先释放音频任务栈，并在 worker/下载失败后恢复语音。"""
        bridge = (WATCHER_ROOT / "main/unitedrhino/ur_bridge.cc").read_text(
            encoding="utf-8"
        )
        self.assertIn("audio_service_.Stop();", APPLICATION)
        self.assertIn("void Application::RecoverFromPlatformOta()", APPLICATION)
        self.assertIn("audio_service_.Start();", APPLICATION)
        self.assertIn("audio_service_.EnableWakeWordDetection(true);", APPLICATION)
        self.assertIn("vTaskDelay(pdMS_TO_TICKS(250));", bridge)
        self.assertGreaterEqual(
            bridge.count("Application::GetInstance().RecoverFromPlatformOta();"), 3
        )

    def test_turn_timeouts_end_stuck_rounds(self):
        """audioStart 与回复终态缺失时必须结束本轮，不能永久卡在监听状态。"""
        self.assertIn("kAudioStartTimeoutUs", PROTOCOL)
        self.assertIn("kResponseTimeoutUs", PROTOCOL)
        self.assertIn("CheckTurnTimeouts();", PROTOCOL)
        self.assertIn("SetError(Lang::Strings::SERVER_TIMEOUT)", PROTOCOL)

    def test_udp_header_matches_devicesim_nonce_layout(self):
        """联犀 UDP 保留 nonce 中段，只覆盖长度和尾部序号。"""
        oracle = self.saas_source("backend/things/tools/devicesim/udp.go")
        self.assertIn("c.aesNonce[:4]", oracle)
        self.assertIn("c.aesNonce[8:24]", oracle)
        self.assertIn("fmt.Sprintf(\"%08x\", c.seq)", oracle)
        self.assertIn("UrAiBuildUdpHeader", UDP_CHANNEL)
        self.assertIn("WriteUint16(output + 2", RUNTIME)
        self.assertIn("WriteUint32(output + 12", RUNTIME)
        self.assertIn("if (!preserve_nonce_timestamp)", RUNTIME)
        self.assertIn("PSA_ALG_CTR", UDP_CHANNEL)

    def test_watcher_declares_actual_encoder_format(self):
        """联犀会话必须声明设备实际编码的 16 kHz、单声道、60 ms。"""
        audio_service = (WATCHER_ROOT / "main/audio/audio_service.h").read_text(encoding="utf-8")
        self.assertIn("#define OPUS_FRAME_DURATION_MS 60", audio_service)
        self.assertIn(".sample_rate = ESP_AUDIO_SAMPLE_RATE_16K", audio_service)
        self.assertIn("constexpr int kUrAiSampleRate = 16000", PROTOCOL)
        self.assertIn('cJSON_AddNumberToObject(audio_params, "sampleRate", kUrAiSampleRate)', PROTOCOL)
        self.assertIn("server_sample_rate_ = kUrAiSampleRate", PROTOCOL)
        self.assertIn(".sample_rate = kUrAiSampleRate", PROTOCOL)
        self.assertIn('cJSON_AddNumberToObject(audio_params, "channels", 1)', PROTOCOL)
        self.assertIn('cJSON_AddNumberToObject(audio_params, "frameDuration", 60)', PROTOCOL)

    def test_16k_opus_downlink_decodes_at_native_hardware_rate(self):
        """16 kHz Opus 必须直接解码为 Watcher 24 kHz，禁止再走已知不安全的重采样。"""
        self.assertIn("bool IsNativeOpusSampleRate(int sample_rate)", AUDIO_SERVICE)
        self.assertIn("IsNativeOpusSampleRate(output_sample_rate)", AUDIO_SERVICE)
        self.assertIn("OPUS_DEC_CFG(decode_sample_rate, frame_duration)", AUDIO_SERVICE)
        self.assertIn("esp_ae_rate_cvt_get_max_out_sample_num", AUDIO_SERVICE)
        self.assertIn("if (size_ret != ESP_AE_ERR_OK", AUDIO_SERVICE)
        self.assertIn("if (resample_ret != ESP_AE_ERR_OK", AUDIO_SERVICE)
        self.assertIn("actual_output > output_capacity", AUDIO_SERVICE)

    def test_embedded_audio_fixture_matches_devicesim_encoder_contract(self):
        """内置 E2E 音频必须由 devicesim 编码器生成并保持 16 kHz/60 ms 帧边界。"""
        generator = self.saas_source("backend/things/tools/devicesim/cmd/opusfixture/main.go")
        self.assertIn("devicesim.LoadMP3AsOpusFrames", generator)
        self.assertIn("devicesim.EncodeOpusSilenceFrames", generator)
        data = E2E_FIXTURE.read_bytes()
        magic, version, sample_rate, frame_ms, frame_count = struct.unpack(
            ">4sHHHH", data[:12]
        )
        self.assertEqual(magic, b"URAF")
        self.assertEqual(version, 1)
        self.assertEqual((sample_rate, frame_ms), (16000, 60))
        self.assertGreaterEqual(frame_count, 20)

        cursor = 12
        for _ in range(frame_count):
            (frame_size,) = struct.unpack(">H", data[cursor : cursor + 2])
            cursor += 2
            self.assertGreater(frame_size, 0)
            self.assertLessEqual(frame_size, 4096)
            cursor += frame_size
            self.assertLessEqual(cursor, len(data))
        self.assertEqual(cursor, len(data))
        self.assertIn('kFixtureMagic[] = {\'U\', \'R\', \'A\', \'F\'}', E2E_FIXTURE_PARSER)
        self.assertIn("constexpr uint16_t kFixtureSampleRate = 16000", E2E_FIXTURE_PARSER)
        self.assertIn("sample_rate_ != kFixtureSampleRate", E2E_FIXTURE_PARSER)

    def test_embedded_audio_e2e_uses_real_protocol_and_is_test_only(self):
        """串口 E2E 只替代麦克风，必须等待真实 audioStarted 与完整 STT/TTS 终态。"""
        self.assertIn("CONFIG_UR_AI_E2E_TEST", WATCHER_BUILDER)
        self.assertIn("ENABLE_UR_AI_E2E_TEST", WATCHER_BUILDER)
        self.assertIn('"query_volume"', APPLICATION)
        self.assertIn('"take_photo"', APPLICATION)
        self.assertIn('"control_volume"', APPLICATION)
        self.assertIn("protocol->WaitForAudioUploadReady", APPLICATION)
        self.assertIn("protocol->SendAudio", APPLICATION)
        self.assertIn("protocol->OnLocalVadState(false)", APPLICATION)
        self.assertIn("protocol->WaitForE2eResult", APPLICATION)
        self.assertIn("result.stt_nonempty", APPLICATION)
        self.assertIn("result.stt_matches_fixture", APPLICATION)
        self.assertIn('stt_text_.find(expected_stt)', PROTOCOL)
        self.assertIn("result.text_nonempty", APPLICATION)
        self.assertIn('"ur_ai_e2e"', WATCHER_BOARD)

    def test_single_mqtt_client_topics(self):
        """语音复用 ur_device MQTT，AI Topic 仅通过队列桥接。"""
        self.assertIn('"$thing/down/ai"', DEVICE)
        self.assertIn('"$thing/up/ai"', DEVICE)
        self.assertEqual(DEVICE.count("IOT_MQTT_Construct"), 1)
        self.assertNotIn("IOT_MQTT_Construct", PROTOCOL)

    def test_ai_uplink_payloads_use_psram(self):
        """AI 上行队列不得预占大块内部 RAM，断线时必须释放指针。"""
        self.assertIn("char *payload;", DEVICE)
        self.assertIn("MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT", DEVICE)
        self.assertIn("heap_caps_free(payload_copy);", DEVICE)
        self.assertIn("ur_clear_ai_publish_queue();", DEVICE)
        self.assertNotIn("char payload[UR_AI_PAYLOAD_MAX]", DEVICE)
        self.assertNotIn("xQueueReset(s_ai_publish_queue)", DEVICE)

    def test_reconnect_restores_clean_session_subscriptions(self):
        """SDK clean session 重连后必须先重订阅，随后才能向 AI 层宣布连接。"""
        self.assertIn("result == UNITEDRHINO_RET_MQTT_RECONNECTED", DEVICE)
        reconnect_block = re.search(
            r"if \(s_resubscribe_pending \|\| result == UNITEDRHINO_RET_MQTT_RECONNECTED\) \{(.*?)\n            \}",
            DEVICE,
            re.S,
        )
        self.assertIsNotNone(reconnect_block)
        block = reconnect_block.group(1)
        self.assertLess(block.index("ur_subscribe_down_topics"), block.index("s_connected = true"))
        self.assertLess(block.index("s_connected = true"), block.index("ur_notify_ai_connection(true)"))

    def test_cold_boot_starts_sntp_without_blocking_mqtt_for_two_minutes(self):
        """冷启动必须主动同步时间，NTP 故障时也不得固定离线 120 秒。"""
        self.assertIn('#include "esp_netif_sntp.h"', DEVICE)
        self.assertIn("ESP_NETIF_SNTP_DEFAULT_CONFIG(server)", DEVICE)
        self.assertIn('"pool.ntp.org"', DEVICE)
        self.assertIn('"ntp.aliyun.com"', DEVICE)
        self.assertIn("esp_netif_sntp_init(&config)", DEVICE)
        self.assertIn("#define UR_TIME_SYNC_TIMEOUT_SECONDS 15", DEVICE)
        self.assertIn("continue connection", DEVICE)
        self.assertNotIn("seconds < 120 && time(NULL)", DEVICE)

    def test_voice_session_preserves_internal_dma_memory(self):
        """AI worker 使用 PSRAM，并在内部 DMA 不足时安全拒绝会话。"""
        self.assertIn("kWorkerTaskStackSize = 8192", PROTOCOL_HEADER)
        self.assertIn("kMinSessionDmaLargestBlockBytes = 4096", PROTOCOL_HEADER)
        self.assertIn("MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT", PROTOCOL)
        self.assertIn("xTaskCreateStatic(WorkerEntry", PROTOCOL)
        self.assertIn("MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA", PROTOCOL)
        self.assertIn("insufficient internal DMA memory for voice session", PROTOCOL)
        self.assertNotIn("CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y", WATCHER_BUILDER)

    def test_button_interrupt_restarts_listening_without_audio_done(self):
        """单击取消播报必须清尾音并恢复监听，不能依赖服务端补发旧轮 audioDone。"""
        toggle = APPLICATION.split("void Application::HandleToggleChatEvent()", 1)[1].split(
            "void Application::ContinueOpenAudioChannel", 1
        )[0]
        speaking = toggle.split("state == kDeviceStateSpeaking", 1)[1].split(
            "state == kDeviceStateListening", 1
        )[0]
        for operation in ("AbortSpeaking(kAbortReasonNone)", "audio_service_.ResetDecoder()",
                          "BeginUrAiTurn()", "SetListeningMode(GetDefaultListeningMode())"):
            self.assertIn(operation, speaking)
        self.assertLess(speaking.index("AbortSpeaking"), speaking.index("SetListeningMode"))
        cancel = PROTOCOL.split("void UrAiProtocol::SendAbortSpeaking", 1)[1].split(
            "void UrAiProtocol::SendMcpMessage", 1
        )[0]
        self.assertIn("response_pending_ = false", cancel)
        self.assertIn("pending_incoming_audio_frames_.clear()", cancel)

    def test_vad_waits_for_stable_silence_in_worker(self):
        """VAD 回调不得把短暂静音直接发成 audioStop，ACK 也不得提前收尾。"""
        vad = PROTOCOL.split("void UrAiProtocol::OnLocalVadState", 1)[1].split(
            "void UrAiProtocol::SendAbortSpeaking", 1
        )[0]
        self.assertNotIn("SendStopListening()", vad)
        self.assertIn("vad_endpoint_.Update", vad)
        self.assertIn("vad_endpoint_.Begin(turn_started_us_)", PROTOCOL)
        self.assertIn("vad_endpoint_.ShouldStop(now_us)", PROTOCOL)
        ack = PROTOCOL.split('strcmp(method, "audioStarted")', 1)[1].split(
            "if (!MatchesCurrentSession", 1
        )[0]
        self.assertNotIn("SendStopListening()", ack)

    def test_cold_boot_wake_waits_for_ai_subscription(self):
        """冷启动唤醒必须等待 AI 主题订阅完成，不能立即误报网络错误。"""
        self.assertIn("kMqttConnectedEvent = 1 << 3", PROTOCOL_HEADER)
        self.assertIn("kMqttReadyWait = pdMS_TO_TICKS(10000)", PROTOCOL)
        callback_update = PROTOCOL.index(
            "xEventGroupSetBits(protocol->event_group_, kMqttConnectedEvent)"
        )
        callback_schedule = PROTOCOL.index("Application::GetInstance().Schedule", callback_update)
        self.assertLess(callback_update, callback_schedule)
        self.assertIn(
            "xEventGroupWaitBits(event_group_, kMqttConnectedEvent", PROTOCOL
        )

    def test_charging_watcher_does_not_dim_and_shutdown_is_one_shot(self):
        """USB 供电不启动省电计时，关机阈值回调每轮只允许触发一次。"""
        self.assertIn("power_save_timer_->SetEnabled(!is_charging);", WATCHER_BOARD)
        constructor = re.search(
            r"SensecapWatcher\(\) \{(.*?)\n\s*\}", WATCHER_BOARD, re.S
        )
        self.assertIsNotNone(constructor)
        self.assertLess(
            constructor.group(1).index("InitializeExpander();"),
            constructor.group(1).index("InitializePowerSaveTimer();"),
        )
        self.assertIn("bool shutdown_requested_ = false;", POWER_SAVE_HEADER)
        self.assertIn("!shutdown_requested_ && on_shutdown_request_", POWER_SAVE_TIMER)
        self.assertIn("shutdown_requested_ = true;", POWER_SAVE_TIMER)
        self.assertGreaterEqual(
            POWER_SAVE_TIMER.count("shutdown_requested_ = false;"), 2
        )


if __name__ == "__main__":
    unittest.main()
