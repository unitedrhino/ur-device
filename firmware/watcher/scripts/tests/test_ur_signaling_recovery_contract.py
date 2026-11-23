"""信令恢复链静态契约：媒体断开恢复激活、采集自愈探针与 MQTT 读取预算解耦。

这些修复没有硬件在环的自动化守护（删掉修复不会有测试变红），本文件用源码切片把每处
修复的"语义"固定下来，并逐项注明它守护的真机历史问题：

- `main/signaling/ur_signaling_webrtc_media.cc`：libpeer 保活超时（15 秒无响应）回调
  CLOSED(5)、ICE 暂断回调 DISCONNECTED(3)，旧实现只处理 FAILED(4)，断开态落入 default
  被忽略，recovery_needed 永不置位、恢复链从未启动，会话静默到平台恢复窗口超时终结；
  close() 的视频调用缺 #if CONFIG_UR_AI_SIGNALING_WEBRTC_VIDEO 时纯音频形态编译失败。
- `main/signaling/ur_signaling_protocol.cc`：媒体恢复链闭环后应用延迟开麦的两个单次边沿
  （播放排空/表情释放）整体错过，设备长期"监听态但采集未开"（哑麦），必须由低频自愈
  探针幂等补发；闸门缺失则补发会打断本轮回复、截断尾音。
- `main/unitedrhino/ur_device.c`：Yield 窗口 500→50ms 后读包级瞬态失败（-604/-605/
  -1001/-110）与连接级错误必须分离，否则一次 TCP 饥饿直接把连接判死；瞬态重试的判死
  预算已由"连续 10 次 × 200ms"（真机 UDP 洪流约 2 秒即判 mqttDisconnected、会话被提前
  终结）改为 25 秒时间预算，与平台媒体恢复窗口同量级，设备不得先于平台自判失败。
- `components/unitedrhino_iot_c_sdk/sdk_src/mqtt_client_common.c`：包内读取预算跟着
  Yield 窗口缩短（~2050ms）时，UDP 洪流造成的 TCP 饥饿让 HAL_TCP_Read 返回 -604 并丢弃
  包内剩余字节，MQTT 流永久错位（错位后 0x60~0x6F 文本字节被解析成 PUBREL 假包）；PUBREL
  旧实现只打 "NOT handled" 错误日志、不回 PUBCOMP，平台按重试节奏反复投递。
"""

import re
import unittest
from pathlib import Path

WATCHER_ROOT = Path(__file__).resolve().parents[2]
SDK_ROOT = WATCHER_ROOT / "components/unitedrhino_iot_c_sdk"
VIDEO_MACRO = "CONFIG_UR_AI_SIGNALING_WEBRTC_VIDEO"

MEDIA = (WATCHER_ROOT / "main/signaling/ur_signaling_webrtc_media.cc").read_text(
    encoding="utf-8"
)
SIGNALING = (WATCHER_ROOT / "main/signaling/ur_signaling_protocol.cc").read_text(
    encoding="utf-8"
)
SIGNALING_HEADER = (WATCHER_ROOT / "main/signaling/ur_signaling_protocol.h").read_text(
    encoding="utf-8"
)
DEVICE = (WATCHER_ROOT / "main/unitedrhino/ur_device.c").read_text(encoding="utf-8")
MQTT_COMMON = (SDK_ROOT / "sdk_src/mqtt_client_common.c").read_text(encoding="utf-8")
SDK_ERRORS = (
    SDK_ROOT / "include/exports/unitedrhino_iot_export_error.h"
).read_text(encoding="utf-8")


def _function(source, signature):
    """按签名切出函数体（到顶格右花括号前），用于语义位置断言。"""
    start = source.index(signature)
    end = source.index("\n}\n", start)
    return source[start:end]


def _ifdef_regions(source, macro):
    """返回源码中 #if <macro> ... #endif 的 [起, 止) 区间（按嵌套深度配对）。"""
    regions = []
    depth = 0
    opened = None
    for match in re.finditer(r"^[ \t]*#\s*(if|ifdef|ifndef|endif)\b[^\n]*$", source, re.M):
        if match.group(1) == "endif":
            depth -= 1
            if depth == 0 and opened is not None:
                regions.append((opened, match.start()))
                opened = None
        else:
            if depth == 0 and macro in match.group(0):
                opened = match.end()
            depth += 1
    return regions


def _error_value(name):
    """把 SDK 错误符号解析回数值，避免断言只认名字不认语义。"""
    match = re.search(rf"\b{name}\s*=\s*(-?\d+)", SDK_ERRORS)
    if match is None:
        raise AssertionError(f"SDK 未定义错误码 {name}")
    return int(match.group(1))


class UrSignalingRecoveryContractTest(unittest.TestCase):
    """守护 ESP/固件侧"防静默回退"：删除任意一处恢复语义都必须让断言变红。"""

    def test_onice_disconnect_states_activate_recovery(self):
        """守护：媒体断开回调 CLOSED(5)/DISCONNECTED(3) 曾被 default 忽略、恢复链不启动。"""
        body = _function(MEDIA, "void WebRtcVoiceMedia::OnIceState(")
        for state in (
            "PEER_CONNECTION_DISCONNECTED",
            "PEER_CONNECTION_FAILED",
            "PEER_CONNECTION_CLOSED",
        ):
            self.assertIn(f"case {state}:", body, f"{state} 必须是显式处理分支")
        first = body.index("case PEER_CONNECTION_DISCONNECTED:")
        handler = body[first : body.index("break;", first)]
        for state in (
            "PEER_CONNECTION_DISCONNECTED",
            "PEER_CONNECTION_FAILED",
            "PEER_CONNECTION_CLOSED",
        ):
            self.assertIn(f"case {state}:", handler, f"{state} 必须与断开态共用同一处理分支")
        # 断开态必须真被处理：分支位置在 default 之前，不能落进兜底空实现。
        self.assertLess(first, body.index("default:"))

    def test_recovery_is_gated_by_connected_once(self):
        """守护：连接建立期失败不得当成"断开重协商"，否则首次失败会话永不收敛。"""
        body = _function(MEDIA, "void WebRtcVoiceMedia::OnIceState(")
        first = body.index("case PEER_CONNECTION_DISCONNECTED:")
        handler = body[first : body.index("break;", first)]
        gate = handler.index("if (self->connected_once_)")
        self.assertLess(gate, handler.index("recovery_needed_.store(true)"))
        # 置位必须在 connected_once_ 门控内，未连接过只允许落到 Failed。
        self.assertLess(handler.index("recovery_needed_.store(true)"), handler.index("else"))
        self.assertIn("PeerState::Failed", handler[handler.index("else") :])

    def test_connected_event_and_close_reset_recovery_state(self):
        """守护：恢复标志必须在连接成功与会话关闭时复位，避免旧会话残留触发重协商。"""
        body = _function(MEDIA, "void WebRtcVoiceMedia::OnIceState(")
        connected = body[
            body.index("case PEER_CONNECTION_CONNECTED:") :
            body.index("case PEER_CONNECTION_DISCONNECTED:")
        ]
        self.assertIn("PeerState::Connected", connected)
        self.assertIn("connected_once_ = true", connected)
        self.assertIn("recovery_needed_.store(false)", connected)

        close = _function(MEDIA, "void WebRtcVoiceMedia::close()")
        self.assertIn("recovery_needed_.store(false)", close)
        self.assertIn("connected_once_ = false", close)

    def test_close_stops_video_only_under_video_kconfig(self):
        """守护：纯音频形态只有 UrSignalingVideo 前向声明，close() 视频调用必须带开关。"""
        start = MEDIA.index("void WebRtcVoiceMedia::close()")
        end = MEDIA.index("\n}\n", start)
        body = MEDIA[start:end]
        regions = _ifdef_regions(body, VIDEO_MACRO)
        self.assertTrue(regions, "close() 缺少 #if CONFIG_UR_AI_SIGNALING_WEBRTC_VIDEO")
        self.assertIn("video_->Stop()", body, "close() 必须停止视频取流")
        for match in re.finditer(r"video_->", body):
            self.assertTrue(
                any(low <= match.start() < high for low, high in regions),
                "close() 中的视频调用必须处于视频开关内",
            )

    def test_taskloop_scans_capture_heal_periodically(self):
        """守护：恢复后哑麦——自愈探针必须挂在 TaskLoop 周期节拍上才会被调用。"""
        self.assertIn("HealListeningCapture", SIGNALING_HEADER, "自愈入口必须对外声明")
        loop = _function(SIGNALING, "void SignalingProtocol::TaskLoop()")
        self.assertIn("kCaptureHealScanMs", loop)
        self.assertIn("capture_heal_scan_ms_ = now_ms;", loop)
        self.assertIn("HealListeningCapture(now_ms);", loop, "TaskLoop 必须周期调用自愈探针")
        self.assertLess(loop.index("kCaptureHealScanMs"), loop.index("HealListeningCapture(now_ms);"))

    def test_heal_requires_facts_and_throttles_resend(self):
        """守护：哑麦补发必须按设备事实判定并节流，否则重复上行打断会话。"""
        heal = _function(SIGNALING, "void SignalingProtocol::HealListeningCapture(")
        # 三个同时成立的事实：会话已建立 + 应用监听态 + 设备侧采集确实未开。
        self.assertIn("VoiceState::Established", heal)
        self.assertIn("GetDeviceState() != kDeviceStateListening", heal)
        self.assertIn("capturing()", heal)
        # 防误伤闸门：播放中或回复尾音未排空不补发（半双工抢麦会截断回复）。
        self.assertIn("playing()", heal)
        self.assertIn("IsPlaybackIdle()", heal)
        # 节流先于动作：2 秒最小间隔保证重复触发安全。
        self.assertIn("kCaptureHealIntervalMs", heal)
        self.assertIn("capture_heal_at_ms_ = now;", heal, "补发后必须记录节流时刻")
        self.assertLess(heal.index("kCaptureHealIntervalMs"), heal.index("EnsureListeningAudio"))
        self.assertLess(heal.index("capture_heal_at_ms_ = now;"), heal.index("EnsureListeningAudio"))

    def test_heal_action_restores_full_listening_audio(self):
        """守护：只补 audioStart 不够——AFE 仅在语音处理开启时回调输出帧，麦克风仍无声。"""
        heal = _function(SIGNALING, "void SignalingProtocol::HealListeningCapture(")
        self.assertIn("app.EnsureListeningAudio();", heal)

    def test_reply_wait_gate_releases_by_timeout_and_recovery(self):
        """守护：等待回复闸门无超时会哑麦、不释放则回复丢失后自愈永不生效。"""
        heal = _function(SIGNALING, "void SignalingProtocol::HealListeningCapture(")
        self.assertIn("if (awaiting_reply_.load())", heal, "等待回复闸门必须存在")
        self.assertIn("VoiceState::Established", heal)
        gate = heal[heal.index("if (awaiting_reply_.load())") : heal.index("VoiceState::Established")]
        self.assertIn("kReplyWaitTimeoutMs", gate)
        self.assertIn("awaiting_reply_at_ms_", gate)
        self.assertIn("awaiting_reply_.store(false)", gate, "等待超时必须释放闸门")

        loop = _function(SIGNALING, "void SignalingProtocol::TaskLoop()")
        recovering = loop[
            loop.index("VoiceState::Recovering") : loop.index("VoiceState::Established")
        ]
        self.assertIn("awaiting_reply_.store(false)", recovering, "媒体重协商作废本轮回复")

    def test_vision_session_keeps_mic_closed(self):
        """守护：视觉会话自动开麦会让静音触发 VAD 收尾结束会话（consent 排查定位）。"""
        start = SIGNALING.index("void SignalingProtocol::SendStartListening(")
        end = SIGNALING.index("\n}\n", start)
        body = SIGNALING[start:end]
        guards = [
            region
            for region in _ifdef_regions(body, VIDEO_MACRO)
            if "vision_session()" in body[region[0] : region[1]]
        ]
        self.assertTrue(guards, "SendStartListening 缺少 aiVision 会话守卫")
        low, high = guards[0]
        self.assertIn("return;", body[low:high])
        self.assertLess(high, body.index("PostCommand(Command::kStartInput)"))

    def test_mqtt_yield_window_at_most_50ms_and_used_in_loop(self):
        """现有窗口已为20ms；守护不超过50ms，避免退回500ms量化延迟。"""
        window = re.search(r"#define\s+UR_MQTT_YIELD_MS\s+(\d+)", DEVICE)
        self.assertIsNotNone(window)
        self.assertGreater(int(window.group(1)), 0)
        self.assertLessEqual(int(window.group(1)), 50)
        loop = _function(DEVICE, "static void ur_device_task(")
        self.assertIn("IOT_MQTT_Yield(s_mqtt_client, UR_MQTT_YIELD_MS)", loop)

    def test_transient_read_failures_retry_before_rebuild(self):
        """守护：-604/-605/-1001/-110 是 UDP 洪流下的可自愈瞬断，不能当连接级错误判死。

        历史：旧实现按"连续 10 次 × 200ms 退避"计数，真机 flood 场景约 2 秒即判
        mqttDisconnected、会话被提前终结；而同一场景平台给媒体恢复的固定窗口是 25 秒，
        设备先于平台自判失败会白丢会话。现改为 25 秒时间预算（与恢复窗口同量级）。
        本用例守护的语义不变：预算内必须继续 200ms 退避重试，超预算才升级为连接级失败。
        """
        self.assertIn("#define UR_MQTT_TRANSIENT_BUDGET_MS 25000", DEVICE, "瞬态重试预算必须是 25 秒时间制")
        self.assertNotIn("UR_MQTT_TRANSIENT_LIMIT", DEVICE, "计数制上限不得复活（约 2 秒即判死连接）")
        judge = _function(DEVICE, "static bool ur_mqtt_yield_is_transient(")
        transient = {
            _error_value(name)
            for name in re.findall(r"result == (UNITEDRHINO_\w+)", judge)
        }
        self.assertEqual(transient, {-604, -605, -1001, -110})
        for name in (
            "UNITEDRHINO_ERR_MQTT_NO_CONN",
            "UNITEDRHINO_ERR_TCP_READ_FAIL",
            "UNITEDRHINO_ERR_TCP_WRITE_FAIL",
            "UNITEDRHINO_ERR_TCP_PEER_SHUTDOWN",
        ):
            self.assertNotIn(name, judge, "连接级错误不得进入瞬态集合")

        loop = _function(DEVICE, "static void ur_device_task(")
        self.assertIn("uint32_t now_ms = HAL_GetTimeMs();", loop, "预算起算必须用单调毫秒时钟")
        started = loop.index("transient_since_ms = now_ms;")
        budget = loop.index("now_ms - transient_since_ms < UR_MQTT_TRANSIENT_BUDGET_MS")
        self.assertLess(started, budget, "必须先记录首次瞬态时刻，再按剩余预算判定")
        window = loop[budget : budget + 500]
        self.assertIn("vTaskDelay(pdMS_TO_TICKS(200));", window, "预算内保持 200ms 短退避")
        self.assertIn("continue;", window, "预算内必须继续重试而不是升级连接级失败")
        escalation = loop.index("result != UNITEDRHINO_RET_SUCCESS")
        self.assertGreater(escalation, budget, "超预算后才走既有连接级重建")

        success = loop[escalation : loop.index("if (s_resubscribe_pending", escalation)]
        self.assertIn("transient_tcp_errors = 0;", success, "恢复成功后必须清零重试计数")
        self.assertIn("transient_since_ms = 0;", success, "恢复成功后必须清零预算起算时刻")
        reconnect = loop.index("if (result == UNITEDRHINO_ERR_MQTT_ATTEMPTING_RECONNECT)")
        reconnect_branch = loop[reconnect : loop.index("continue;", reconnect)]
        self.assertIn("transient_since_ms = 0;", reconnect_branch, "SDK 重建连接必须清零预算，不累计重建前的饥饿")

    def test_packet_read_budget_decoupled_from_yield_window(self):
        """守护：包内读取预算跟着 Yield 窗口缩到 ~2050ms 时 TCP 饥饿致 MQTT 流永久错位。"""
        match = re.search(
            r"#define\s+UNITEDRHINO_IOT_MQTT_MIN_PACKET_READ_WINDOW_MS\s+\((\d+)\)",
            MQTT_COMMON,
        )
        self.assertIsNotNone(match, "必须定义包内读取的最小窗口预算")
        self.assertGreaterEqual(int(match.group(1)), 500, "最小窗口不得小于旧 500ms 量级")

        # 预算抬升集中在 _packet_read_window_ms：包内每个读取点都必须经它取值，
        # 只有固定头首字节读仍按 Yield 窗口（快速返回，不阻塞上行）。
        helper = _function(MQTT_COMMON, "static uint32_t _packet_read_window_ms(")
        self.assertIn("UNITEDRHINO_IOT_MQTT_MIN_PACKET_READ_WINDOW_MS", helper)
        self.assertIn("UNITEDRHINO_IOT_MQTT_MAX_REMAIN_WAIT_MS", helper)

        body = _function(MQTT_COMMON, "static int _read_mqtt_packet(")
        header_read = [line for line in body.splitlines() if "network_stack.read(" in line]
        self.assertEqual(len(header_read), 1, "读包主流程只保留固定头首字节一次网络读")
        self.assertIn("timer_left_ms", header_read[0], "首字节读仍走 Yield 窗口")
        self.assertEqual(
            body.count("_packet_read_window_ms(timer)"),
            2,
            "剩余长度字段与负载读取都必须走包内预算（≥最小窗口 + 剩余等待）",
        )
        for signature in ("static int _read_rem_len_field(", "static int _read_payload_field("):
            reader = _function(MQTT_COMMON, signature)
            self.assertIn("network_stack.read(", reader, f"{signature} 必须自行读字节")
            self.assertIn("timeout_ms", reader, f"{signature} 的读取预算必须由调用方传入")

    def test_pubrel_is_routed_to_pubcomp_reply(self):
        """守护：PUBREL 旧实现只打 'currently NOT handled' 日志，平台按重试反复投递。"""
        self.assertNotIn("NOT handled", MQTT_COMMON)
        body = _function(MQTT_COMMON, "int cycle_for_read(")
        self.assertIn("case PUBREL:", body, "cycle_for_read 必须显式处理 PUBREL")
        self.assertIn("case PUBCOMP:", body)
        case = body[body.index("case PUBREL:") : body.index("case PUBCOMP:")]
        self.assertIn("_handle_pubrel_packet(pClient, timer)", case)

    def test_pubrel_ack_never_escalates_to_connection_failure(self):
        """守护：收尾确认不得把连接判死——解析/发送失败只告警，出口恒为成功。"""
        body = _function(MQTT_COMMON, "static int _handle_pubrel_packet(")
        exits = re.findall(r"IOT_FUNC_EXIT_RC\(([^()]*)\)", body)
        self.assertGreaterEqual(len(exits), 2, "PUBREL 处理器至少要有解析失败与成功出口")
        self.assertEqual(set(exits), {"UNITEDRHINO_RET_SUCCESS"})
        self.assertIn("PUBCOMP", body, "合法 PUBREL 必须回 PUBCOMP")
        self.assertIn("send_mqtt_packet", body)


if __name__ == "__main__":
    unittest.main()
