"""真机 BLE 验收器回放测试：用确定性传输替身执行完整生产协议序列，不冒充硬件。"""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from run_ur_network_e2e import (Decoder, ProbeError, advertisement_matches,
                              capability, credentials, run_sequence, verify_boot_log)


def status(phase=2, token=b"12345678", error=0):
    """status 构造合法本地换网能力正文，便于覆盖请求关联。"""
    return 0xE5, bytes([2, 1, phase, error]) + token


class Link:
    """Link 仅替代 BLE 传输，协议的校验、下发和完成判定均执行生产函数。"""
    def __init__(self, batches):
        self.batches = list(batches)
        self.commands = []

    def exchange(self, command, accept, timeout):
        self.commands.append(command)
        for kind, body in self.batches.pop(0):
            if accept(kind, body):
                return
        raise ProbeError("notification_timeout")


class NetworkE2ERunnerTest(unittest.TestCase):
    """覆盖本地成功双判据、身份隔离、乱序、超时与绑定指令禁入。"""
    def test_post_boot_network_verification(self):
        """拒绝旧网在线、历史成功、新启动缺阶段及断线后的假通过。"""
        good = ("ESP-ROM:esp32s3\nWifiStation: Got IP: 192.0.2.10\n"
                "WifiBoard: Connected to WiFi: new-network\n"
                "UnitedRhino MQTT transport connected, pid=P dev=DEV\n"
                "UnitedRhino MQTT subscriptions ready\n"
                "received downlink method=reportReply\n")
        self.assertTrue(verify_boot_log(good, "new-network", "P", "DEV")["passed"])
        cases = [
            good.replace("new-network", "old-network"),
            good + "ESP-ROM:esp32s3\n",
            good.replace("ESP-ROM:", "unknown:"),
            good.replace("192.0.2.10", "0.0.0.0"),
            good.replace("192.0.2.10", "169.254.1.2"),
            good.replace("Got IP:", "no address:"),
            good.replace("pid=P", "pid=other"),
            good.replace("dev=DEV", "dev=other"),
            good.replace("dev=DEV", "dev=DEV2"),
            good.replace("reportReply", "reportInfoReply"),
            good.replace("subscriptions ready", "subscriptions waiting"),
            good + "WifiBoard: WiFi disconnected\n",
            good + "Guru Meditation\n",
            good + good.replace("new-network", "old-network"),
        ]
        for log in cases:
            with self.subTest(log=log), self.assertRaises(ProbeError):
                verify_boot_log(log, "new-network", "P", "DEV")

    def setUp(self):
        self.identity = (0x08, bytes([2, 0, 20, 3]) + b"DEV")
        self.initial = [status(), self.identity]
        self.config = {"ssid": "lab", "password": "test-only"}
        self.connected = (0xE2, bytes([1, 0, 0, 3]) + b"lab")

    def test_success_order_duplicate_and_read_only_ten_times(self):
        """成功必须同请求已保存加匹配 SSID 联网；两种终态顺序和重复均兼容。"""
        for _ in range(10):
            read_only = Link([self.initial])
            self.assertFalse(run_sequence(read_only, "DEV")["credentialsSent"])
            self.assertEqual(read_only.commands, [b"\xe0"])
            for terminal in ([status(7), status(7), self.connected],
                             [self.connected, self.connected, status(7)]):
                link = Link([self.initial, [(0xE0, b"\0")], [(0xE1, b"\0")], terminal])
                result = run_sequence(link, "DEV", self.config)
                self.assertTrue(result["committed"] and result["connected"])
                self.assertEqual([command[0] for command in link.commands], [0xE0, 0xE1, 0xE2, 0xE3])
                self.assertEqual(link.commands[2], b"\xe2\0\x0c\x03lab\x09test-only")

    def test_identity_or_capability_failure_never_sends_credentials(self):
        """设备名不符、旧版本、请求变化或错误阶段均在发送密码前拒绝。"""
        for initial in ([self.identity], [status()], [status(3), self.identity],
                        [status(), status(token=b"87654321"), self.identity],
                        [(0xE5, b"\x01\x01" + bytes(10)), self.identity],
                        [status(error=1), self.identity]):
            link = Link([initial])
            with self.assertRaises(ProbeError):
                run_sequence(link, "DEV", self.config)
            self.assertEqual(link.commands, [b"\xe0"])
        link = Link([self.initial])
        with self.assertRaisesRegex(ProbeError, "identity_mismatch"):
            run_sequence(link, "OTHER", self.config)
        self.assertEqual(link.commands, [b"\xe0"])

    def test_terminal_requires_matching_request_and_ssid(self):
        """单独已保存/联网、错误请求、失败或损坏通知不能通过成功门禁。"""
        for terminal in ([status(7)], [self.connected],
                         [status(7, token=b"87654321"), self.connected],
                         [status(8, error=2)], [status(7), (0xE2, b"\x01\0\0\x03bad")],
                         [(0xE2, b"\x01\0\0\x04lab")]):
            link = Link([self.initial, [(0xE0, b"\0")], [(0xE1, b"\0")], terminal])
            with self.assertRaises(ProbeError):
                run_sequence(link, "DEV", self.config)
            self.assertEqual([command[0] for command in link.commands], [0xE0, 0xE1, 0xE2, 0xE3])

    def test_rejected_mode_or_candidate_stops_immediately(self):
        """模式或凭据受理失败不能继续连接，也不能自动重发密码。"""
        for batches, sent in (([self.initial, [(0xE0, b"\1")]], 2),
                              ([self.initial, [(0xE0, b"\0")], [(0xE1, b"\1")]], 3)):
            link = Link(batches)
            with self.assertRaises(ProbeError):
                run_sequence(link, "DEV", self.config)
            self.assertEqual(len(link.commands), sent)

    def test_credentials_and_advertisement(self):
        """验证长度为 UTF-8 字节数，广播为 BlueZ 去 Company ID 后格式。"""
        for data in (None, {}, {"ssid": "", "password": ""}, {"ssid": "x" * 33, "password": ""},
                     {"ssid": "网络" * 6, "password": ""}, {"ssid": "x", "password": "x" * 64},
                     {"ssid": "x\0", "password": ""}):
            with self.assertRaises(ProbeError):
                credentials(data)
        raw = bytes([2, 1, 2, 3, 4, 5, 6, 65]) + bytes(9)
        self.assertTrue(advertisement_matches(raw, "A", "01:02:03:04:05:06"))
        for value in (b"\xe7\xfe" + raw, bytes([0x20]) + raw[1:], raw[:-1]):
            self.assertFalse(advertisement_matches(value, "A", "01:02:03:04:05:06"))

    def test_fragment_bounds_expiry_and_full_frame(self):
        """所有分片必须满足长度、顺序、类型和十秒窗口。"""
        decoder = Decoder()
        self.assertIsNone(decoder.push(b"\x08\x40\x02ab", 0))
        self.assertIsNone(decoder.push(b"\x08\x80\x01c", 1))
        self.assertEqual(decoder.push(b"\x08\xc0\x01d", 2), (8, b"abcd"))
        for raw in (b"", b"\x08\0\2a", b"\x08\xc0\1a"):
            with self.assertRaises(ProbeError):
                decoder.push(raw, 3)
        decoder.push(b"\x08\x40\1a", 0)
        with self.assertRaises(ProbeError):
            decoder.push(b"\x08\xc0\1b", 11)
        with self.assertRaises(ProbeError):
            decoder.push(bytes([8, 16, 1]) + b"a" * 4097, 12)
        with self.assertRaises(ProbeError):
            capability(bytes(12))
