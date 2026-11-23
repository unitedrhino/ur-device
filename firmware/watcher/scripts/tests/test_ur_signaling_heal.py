"""HealListeningCapture 判定边界：从生产源码复算八类事实组合下自愈探针的决策。

真机哑麦缺陷（媒体恢复链闭环后，应用延迟开麦的两个单次边沿整体错过）由该低频探针
兜底：会话已建立 + 应用监听态 + 设备采集确实未开才补发，两道防误伤闸门（等待回复
窗口、播放未排空）拦下会打断回复/截断尾音的补发，2 秒节流保证重复触发安全。

本文件与既有 test_ur_signaling_recovery_contract.py 的分工：后者守护“结构、常量与
动作存在”，本文件守护“判定真值表与边界行为”。做法与 P0 契约测试同款“源码切片 +
条件复算”，但不改生产源码、不依赖固件构建：

- 从 HealListeningCapture 源码按大括号/小括号配对切出有序语句（注释先剥离）；
- 条件表达式直接取自源码文本，经最小 C++→Python 改写后用宿主事实求值；
- 解释器按源码控制流复算（return 短路、闸门释放、节流刷新、EnsureListeningAudio 动作），
  任何条件文本、语句顺序或动作变更都会让本文件变红，需人工复核后再同步。

边界项与真机事故形态对应：
  1) 等待回复中不补发（打断本轮回复）；2) 等待超时后放行（回复丢失不哑麦）；
  3) 播放中不补发（半双工抢麦）；4) 播放 idle 才放行（尾音排空后才开麦）；
  5) 未采集时补发动作是 EnsureListeningAudio（只补 audioStart 麦克风仍不出声）；
  6) 非 Established 不动作（媒体未就绪）；7) 非监听态不动作（挂断后不复活）；
  8) 2 秒节流内不重复（重复上行抢麦）。
"""

import re
import types
import unittest
from pathlib import Path

WATCHER_ROOT = Path(__file__).resolve().parents[2]
SIGNALING = (WATCHER_ROOT / "main/signaling/ur_signaling_protocol.cc").read_text(
    encoding="utf-8"
)

# 源码条件 ↔ Python 复算的最小改写：成员/枚举访问映射到同名宿主事实，不改比较语义。
UR_NAMESPACE = types.SimpleNamespace(
    device=types.SimpleNamespace(
        VoiceState=types.SimpleNamespace(
            Established="Established", Recovering="Recovering", Failed="Failed", Idle="Idle"
        )
    )
)


def _function_body(source, signature):
    """按签名切出函数体（首个左花括号到顶格右花括号之间），保留缩进供结构化解析。"""
    start = source.index(signature)
    brace = source.index("{", start)
    end = source.index("\n}\n", brace)
    return source[brace + 1 : end]


def _strip_comments(text):
    """剥离块注释与行注释，避免注释中的括号/花括号干扰语句切分。"""
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def _constexpr(source, name):
    """读取生产常量值（std::uint64_t），保证边界复算跟随源码而不是复制数字。"""
    match = re.search(rf"constexpr\s+std::uint64_t\s+{name}\s*=\s*(\d+)\s*;", source)
    if match is None:
        raise AssertionError(f"源码中未找到常量 {name}")
    return int(match.group(1))


def _normalize(text):
    """折叠空白，得到与源码排版无关但仍保持字面、运算符和顺序的指纹。"""
    return re.sub(r"\s+", " ", text).strip()


def _match(text, index, opening, closing):
    """返回与 text[index] 处开括号配对的闭括号位置；跳过字符串字面量。"""
    depth = 0
    in_string = False
    while index < len(text):
        char = text[index]
        if in_string:
            if char == "\\":
                index += 2
                continue
            if char == '"':
                in_string = False
        elif char == '"':
            in_string = True
        elif char == opening:
            depth += 1
        elif char == closing:
            depth -= 1
            if depth == 0:
                return index
        index += 1
    raise AssertionError(f"括号未闭合（{opening}...{closing}）")


class _Statement:
    """解析后的单条语句：if 块（条件 + 子语句）或到分号为止的表达式语句。"""

    __slots__ = ("kind", "condition", "body", "text")

    def __init__(self, kind, condition="", body=None, text=""):
        self.kind = kind
        self.condition = condition
        self.body = body or []
        self.text = text


def _parse_statements(text):
    """把函数体切成有序语句，作为“复算按源码控制流执行”的结构来源。"""
    statements = []
    index = 0
    while index < len(text):
        while index < len(text) and text[index].isspace():
            index += 1
        if index >= len(text):
            break
        if re.match(r"if\s*\(", text[index:]):
            paren = text.index("(", index)
            condition_end = _match(text, paren, "(", ")")
            block_start = text.index("{", condition_end)
            block_end = _match(text, block_start, "{", "}")
            statements.append(
                _Statement(
                    "if",
                    condition=_normalize(text[paren + 1 : condition_end]),
                    body=_parse_statements(text[block_start + 1 : block_end]),
                )
            )
            index = block_end + 1
            continue
        cursor = index
        in_string = False
        while cursor < len(text):
            char = text[cursor]
            if in_string:
                if char == "\\":
                    cursor += 2
                    continue
                if char == '"':
                    in_string = False
            elif char == '"':
                in_string = True
            elif char == ";":
                break
            cursor += 1
        statements.append(_Statement("expr", text=_normalize(text[index:cursor])))
        index = cursor + 1
    return statements


def _to_python(condition):
    """把源码条件的最小 C++ 写法改写为等价的 Python 表达式。"""
    expression = condition.replace("->", ".").replace("::", ".")
    expression = expression.replace(".load()", "")
    expression = expression.replace("nullptr", "None")
    expression = re.sub(r"!(?!=)", "not ", expression)
    return expression.replace("&&", " and ").replace("||", " or ")


class _Atomic:
    """复算宿主里的原子值：源码的 .load() 已在改写时剥离，造型按真值参与条件。"""

    def __init__(self, value):
        self.value = value

    def __bool__(self):
        return bool(self.value)


class _Voice:
    """设备侧语音状态事实：state()/capturing()/playing() 与源码调用面一一对应。"""

    def __init__(self, state="Established", capturing=False, playing=False):
        self._state = state
        self._capturing = capturing
        self._playing = playing

    def state(self):
        return self._state

    def capturing(self):
        return self._capturing

    def playing(self):
        return self._playing


class _App:
    """应用状态事实：GetDeviceState/GetAudioService 与源码调用面一一对应。"""

    def __init__(self, device_state="kDeviceStateListening", playback_idle=True):
        self._device_state = device_state
        self._audio = types.SimpleNamespace(IsPlaybackIdle=lambda: playback_idle)

    def GetDeviceState(self):
        return self._device_state

    def GetAudioService(self):
        return self._audio


class _World:
    """复算宿主：字段名与源码一致；默认是一幅“监听态、采集确实未开、闸门全放行”的场景。"""

    def __init__(self, now=100_000):
        self.now = now
        self.session_active_ = _Atomic(True)
        self.runtime_ = types.SimpleNamespace(voice=lambda: _Voice())
        self.awaiting_reply_ = _Atomic(False)
        self.start_command_pending_ = _Atomic(False)
        self.awaiting_reply_at_ms_ = 0
        self.capture_heal_at_ms_ = 0
        self.app = _App()
        self.audio = self.app.GetAudioService()
        self.action = None
        self.capture_heal_updates = []


def _evaluate(condition, world):
    """用宿主事实求值源码条件（表达式文本即源码切片）。"""
    namespace = {
        "now": world.now,
        "kReplyWaitTimeoutMs": _constexpr(SIGNALING, "kReplyWaitTimeoutMs"),
        "kCaptureHealIntervalMs": _constexpr(SIGNALING, "kCaptureHealIntervalMs"),
        "ur": UR_NAMESPACE,
        "kDeviceStateListening": "kDeviceStateListening",
        "session_active_": world.session_active_,
        "runtime_": world.runtime_,
        "awaiting_reply_": world.awaiting_reply_,
        "start_command_pending_": world.start_command_pending_,
        "awaiting_reply_at_ms_": world.awaiting_reply_at_ms_,
        "capture_heal_at_ms_": world.capture_heal_at_ms_,
        "app": world.app,
        "audio": world.audio,
    }
    return bool(eval(_to_python(condition), {"__builtins__": {}}, namespace))


def _execute(text, world):
    """执行源码中的表达式语句；未建模的语句直接失败，防止新分支绕过复算。"""
    if text.startswith("auto&") or text.startswith("auto "):
        return
    if text.startswith("ESP_LOG"):
        return
    if text == "awaiting_reply_.store(false)":
        world.awaiting_reply_.value = False
        return
    if text == "capture_heal_at_ms_ = now":
        world.capture_heal_at_ms_ = world.now
        world.capture_heal_updates.append(world.now)
        return
    match = re.fullmatch(r"app\.(\w+)\(\)", text)
    if match:
        world.action = match.group(1)
        return
    raise AssertionError(f"复算模型未覆盖语句：{text}")


def _run(statements, world):
    """按源码语句顺序复算；返回 True 表示本次调用以 return 短路。"""
    for statement in statements:
        if statement.kind == "if":
            if _evaluate(statement.condition, world):
                if _run(statement.body, world):
                    return True
        elif statement.text == "return":
            return True
        else:
            _execute(statement.text, world)
    return False


def _fingerprint(statements):
    """把语句序列转为可断言的指纹，任何条件/顺序/动作变更都会显形。"""
    fingerprint = []
    for statement in statements:
        if statement.kind == "if":
            fingerprint.append(("if", statement.condition, _fingerprint(statement.body)))
        else:
            fingerprint.append(("expr", statement.text))
    return fingerprint


class HealListeningCaptureTest(unittest.TestCase):
    """守护自愈探针的真值表：任一闸门被删/改向/去节流都必须变红。"""

    SIGNATURE = "void SignalingProtocol::HealListeningCapture("

    @classmethod
    def setUpClass(cls):
        cls.statements = _parse_statements(
            _strip_comments(_function_body(SIGNALING, cls.SIGNATURE))
        )

    def _heal(self, world):
        """复算一次 HealListeningCapture(now)；返回是否被 return 短路。"""
        short_circuited = _run(self.statements, world)
        return short_circuited

    def test_structure_matches_source_contract(self):
        """复算结构指纹：守卫顺序、嵌套等待超时与最终动作必须与契约逐字一致。"""
        self.assertEqual(
            _fingerprint(self.statements),
            [
                ("if", "!session_active_.load() || runtime_ == nullptr", [("expr", "return")]),
                (
                    "if",
                    "awaiting_reply_.load()",
                    [
                        (
                            "if",
                            "now - awaiting_reply_at_ms_.load() < kReplyWaitTimeoutMs",
                            [("expr", "return")],
                        ),
                        ("expr", "awaiting_reply_.store(false)"),
                        (
                            "expr",
                            'ESP_LOGW(kTag, "capture self-heal: reply wait timeout, gate released")',
                        ),
                    ],
                ),
                (
                    "if",
                    "runtime_->voice().state() != ur::device::VoiceState::Established",
                    [("expr", "return")],
                ),
                ("if", "start_command_pending_.load()", [("expr", "return")]),
                ("expr", "auto& app = Application::GetInstance()"),
                ("if", "app.GetDeviceState() != kDeviceStateListening", [("expr", "return")]),
                ("expr", "auto& audio = app.GetAudioService()"),
                (
                    "if",
                    "runtime_->voice().capturing() || runtime_->voice().playing() || !audio.IsPlaybackIdle()",
                    [("expr", "return")],
                ),
                (
                    "if",
                    "now - capture_heal_at_ms_ < kCaptureHealIntervalMs",
                    [("expr", "return")],
                ),
                ("expr", "capture_heal_at_ms_ = now"),
                (
                    "expr",
                    'ESP_LOGW(kTag, "capture self-heal: listening but capture off, resume app listening")',
                ),
                ("expr", "app.EnsureListeningAudio()"),
            ],
        )

    def test_inactive_session_or_missing_runtime_skips(self):
        """边界 0：会话未激活或运行时缺失时不动作（挂断后不得复活采集）。"""
        inactive = _World()
        inactive.session_active_.value = False
        self.assertTrue(self._heal(inactive))
        self.assertIsNone(inactive.action)
        self.assertEqual(inactive.capture_heal_updates, [])

        missing = _World()
        missing.runtime_ = None
        self.assertTrue(self._heal(missing))
        self.assertIsNone(missing.action)
        self.assertEqual(missing.capture_heal_updates, [])

    def test_waiting_reply_within_timeout_skips(self):
        """边界 1：等待回复窗口内不补发——补发会打断本轮回复。"""
        world = _World()
        world.awaiting_reply_.value = True
        world.awaiting_reply_at_ms_ = world.now - 9_999
        self.assertTrue(self._heal(world))
        self.assertIsNone(world.action)
        self.assertTrue(world.awaiting_reply_.value, "窗口内不得提前清位")
        self.assertEqual(world.capture_heal_updates, [])

    def test_queued_capture_start_skips_duplicate_heal(self):
        """真实开麦命令尚未消费时，不因采集状态暂未推进而重复发送。"""
        world = _World()
        world.start_command_pending_.value = True
        self.assertTrue(self._heal(world))
        self.assertIsNone(world.action)
        self.assertEqual(world.capture_heal_updates, [])

    def test_reply_wait_timeout_releases_gate_and_heals(self):
        """边界 2：等待超时视为回复丢失，释放闸门后继续补发（不得哑麦）。"""
        world = _World()
        world.awaiting_reply_.value = True
        world.awaiting_reply_at_ms_ = world.now - _constexpr(SIGNALING, "kReplyWaitTimeoutMs")
        self.assertFalse(self._heal(world), "超时后不得再被等待闸门短路")
        self.assertFalse(world.awaiting_reply_.value, "超时后必须释放闸门")
        self.assertEqual(world.action, "EnsureListeningAudio")
        self.assertEqual(world.capture_heal_updates, [world.now])

    def test_gate_release_persists_when_later_gate_blocks(self):
        """边界 2 附加：超时释放闸门后落到其它闸门也不回滚清位（避免下次再等一整窗）。"""
        world = _World()
        world.awaiting_reply_.value = True
        world.awaiting_reply_at_ms_ = world.now - _constexpr(SIGNALING, "kReplyWaitTimeoutMs")
        world.app = _App(device_state="kDeviceStateSpeaking")
        world.audio = world.app.GetAudioService()
        self.assertTrue(self._heal(world))
        self.assertFalse(world.awaiting_reply_.value)
        self.assertIsNone(world.action)

    def test_playing_skips_heal(self):
        """边界 3：正在播放时补发会抢麦/截断回复，必须跳过。"""
        world = _World()
        world.runtime_ = types.SimpleNamespace(voice=lambda: _Voice(playing=True))
        self.assertTrue(self._heal(world))
        self.assertIsNone(world.action)
        self.assertEqual(world.capture_heal_updates, [])

    def test_non_idle_playback_tail_skips_heal(self):
        """边界 3/4：回复尾音未排空（IsPlaybackIdle=false）时同样不补发。"""
        world = _World()
        world.app = _App(playback_idle=False)
        world.audio = world.app.GetAudioService()
        self.assertTrue(self._heal(world))
        self.assertIsNone(world.action)

    def test_capture_off_with_idle_playback_heals(self):
        """边界 4 基线：采集未开且播放已排空才放行补发。"""
        world = _World()
        self.assertFalse(self._heal(world))
        self.assertEqual(world.action, "EnsureListeningAudio")
        self.assertEqual(world.capture_heal_updates, [world.now])

    def test_action_is_ensure_listening_audio(self):
        """边界 5：补发动作固定为 EnsureListeningAudio——只补 audioStart 麦克风仍不出声。"""
        world = _World()
        self.assertFalse(self._heal(world))
        self.assertEqual(world.action, "EnsureListeningAudio")
        self.assertIn(
            ("expr", "app.EnsureListeningAudio()"),
            _fingerprint(self.statements),
            "动作必须由源码末尾的 EnsureListeningAudio 调用产生",
        )
        sources = [s.condition if s.kind == "if" else s.text for s in self.statements]
        self.assertEqual(
            [text for text in sources if "start_input" in text],
            [],
            "探针不得直接调用 start_input 绕过应用开麦路径",
        )

    def test_capturing_already_on_skips_heal(self):
        """边界 5 幂等：采集已开时不是缺口，不得重复上行。"""
        world = _World()
        world.runtime_ = types.SimpleNamespace(voice=lambda: _Voice(capturing=True))
        self.assertTrue(self._heal(world))
        self.assertIsNone(world.action)

    def test_not_established_skips_heal(self):
        """边界 6：建立/重协商/结束中媒体未就绪，补发必然失败或落到旧代次。"""
        world = _World()
        world.runtime_ = types.SimpleNamespace(voice=lambda: _Voice(state="Recovering"))
        self.assertTrue(self._heal(world))
        self.assertIsNone(world.action)
        self.assertEqual(world.capture_heal_updates, [])

    def test_non_listening_device_state_skips_heal(self):
        """边界 7：非监听态（播报/空闲/配网等）不补发，避免挂断后复活采集。"""
        world = _World()
        world.app = _App(device_state="kDeviceStateIdle")
        world.audio = world.app.GetAudioService()
        self.assertTrue(self._heal(world))
        self.assertIsNone(world.action)

    def test_throttle_window_skips_then_reenables(self):
        """边界 8：2 秒节流内不重复补发；满窗口后放行并刷新节流时刻。"""
        interval = _constexpr(SIGNALING, "kCaptureHealIntervalMs")
        throttled = _World()
        throttled.capture_heal_at_ms_ = throttled.now - (interval - 1)
        self.assertTrue(self._heal(throttled))
        self.assertIsNone(throttled.action)
        self.assertEqual(
            throttled.capture_heal_at_ms_, throttled.now - (interval - 1), "节流内不得刷新时刻"
        )

        relaxed = _World()
        relaxed.capture_heal_at_ms_ = relaxed.now - interval
        self.assertFalse(self._heal(relaxed))
        self.assertEqual(relaxed.action, "EnsureListeningAudio")
        self.assertEqual(relaxed.capture_heal_updates, [relaxed.now])


if __name__ == "__main__":
    unittest.main()
