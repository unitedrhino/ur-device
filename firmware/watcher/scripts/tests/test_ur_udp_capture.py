"""使用合成pcap验证独立端点视角；不连接平台，不将合成包冒充真机证据。"""

import importlib.util
from pathlib import Path
import struct
import unittest

SPEC = importlib.util.spec_from_file_location('ur_udp_capture', Path(__file__).parents[1] / 'ur_udp_capture.py')
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)
Reader = MODULE.UdpCaptureReader
EVIDENCE_SPEC = importlib.util.spec_from_file_location('ur_udp_evidence', Path(__file__).parents[1] / 'ur_udp_evidence.py')
EVIDENCE = importlib.util.module_from_spec(EVIDENCE_SPEC)
EVIDENCE_SPEC.loader.exec_module(EVIDENCE)


def fixture(order='<', link=1, route=b'ROUT', source_port=23456, fragment=0, invalid_type=False):
    """构造完整长度正确的合成报文；敏感哨兵仅用于证明输出不保留原始字段。"""
    offset = {1: 14, 113: 16, 276: 20}[link]
    header = struct.pack('!BBH4sII', 2 if invalid_type else 1, 0, 7, route, 600, 37)
    payload = header + b'PRIVATE'
    ip = bytearray(20)
    ip[0], ip[9] = 0x45, 17
    struct.pack_into('!HH', ip, 2, 20 + 8 + len(payload), 0)
    struct.pack_into('!H', ip, 6, fragment)
    ip[12:16], ip[16:20] = b'\xc0\x00\x02\x01', b'\xc0\x00\x02\x02'
    packet = bytes(offset) + bytes(ip) + struct.pack('!HHHH', source_port, 9000, 8 + len(payload), 0) + payload
    magic = b'\xd4\xc3\xb2\xa1' if order == '<' else b'\xa1\xb2\xc3\xd4'
    return (magic + struct.pack(order + 'HHIIII', 2, 4, 0, 0, 112, link)
            + struct.pack(order + 'IIII', 10, 500, len(packet), len(packet)) + packet)


class UdpCaptureTest(unittest.TestCase):
    """覆盖错路由、观察范围、pcap分块、链路格式与有界解析。"""

    def reader(self):
        """创建已核实源端点的独立观察器，不自动放宽到其他设备。"""
        return Reader(9000, b'ROUT', ('192.0.2.1', 23456))

    def test_wrong_route_is_retained(self):
        """端点模式保留错路由包，不能把它静默报告为链路缺包。"""
        reader = self.reader()
        reader.feed(fixture(route=b'BAD!'))
        self.assertEqual(reader.records, [dict(epochUs=10000500, seq=37, bytes=23,
                                               routeMatch=False, headerValid=True)])
        self.assertNotIn('PRIVATE', repr(reader.records))
        self.assertNotIn('BAD!', repr(reader.records))
        self.assertNotIn('192.0.2', repr(reader.records))

    def test_route_mode_learns_endpoint(self):
        """仅由匹配路由的报文学习源端点；错路由不建立设备关联。"""
        reader = Reader(9000, b'ROUT')
        reader.feed(fixture())
        self.assertEqual(reader.endpoint, (b'\xc0\x00\x02\x01', 23456))
        with self.assertRaisesRegex(ValueError, 'scope_mismatch'):
            Reader(9000, b'ROUT').feed(fixture(route=b'BAD!'))

    def test_other_endpoint_rejected(self):
        """其他设备或会话的源端口不能进入该目标证据。"""
        with self.assertRaisesRegex(ValueError, 'scope_mismatch'):
            self.reader().feed(fixture(source_port=23457))

    def test_both_orders_and_three_links_chunked(self):
        """六种封装逐字节进入，未完成记录保留为部分观察而非造假补齐。"""
        for order in ('<', '>'):
            for link in (1, 113, 276):
                reader = self.reader()
                for byte in fixture(order, link):
                    reader.feed(bytes([byte]))
                self.assertTrue(reader.records[0]['headerValid'])
                self.assertEqual(len(reader.buffer), 0)

    def test_fragment_and_invalid_header_retained(self):
        """目标端点的分片或类型错误仍计入捕获数，但不得证明有效语音送达。"""
        for kwargs in ({'fragment': 0x2000}, {'invalid_type': True}):
            reader = self.reader()
            reader.feed(fixture(**kwargs))
            self.assertFalse(reader.records[0]['headerValid'])
            self.assertEqual(len(reader.records), 1)

    def test_truncated_record_is_incomplete(self):
        """缺一个捕获字节不能产生完整包记录。"""
        reader = self.reader()
        reader.feed(fixture()[:-1])
        self.assertEqual(reader.records, [])
        self.assertGreater(len(reader.buffer), 0)

    def test_invalid_scope_and_format(self):
        """非法目标、超限块与不支持pcap格式必须拒绝，不能产生通过证据。"""
        for port, route, endpoint in ((True, b'ROUT', None), (9000, b'X', None),
                                     (9000, b'ROUT', ('::1', 23456)),
                                     (9000, b'ROUT', ('192.0.2.1', True))):
            with self.assertRaises(ValueError):
                Reader(port, route, endpoint)
        with self.assertRaises(ValueError):
            self.reader().feed(bytes(65537))
        with self.assertRaises(ValueError):
            self.reader().feed(bytes(24))

    def test_declared_length_mismatch_retained(self):
        """损坏长度的目标包不能作为正常音频，也不能从统计中消失。"""
        data = bytearray(fixture())
        struct.pack_into('!H', data, 24 + 16 + 14 + 20 + 4, 30)
        reader = self.reader()
        reader.feed(bytes(data))
        self.assertFalse(reader.records[0]['headerValid'])

    def test_header_gate_cannot_pass_wrong_route(self):
        """即使序号、大小与套接字日志一致，错路由或非法头也必须使完整证据失败。"""
        sends = [dict(seq=1, bytes=23, sent=23)]
        for field in ('routeMatch', 'headerValid'):
            packet = dict(seq=1, bytes=23, routeMatch=True, headerValid=True)
            packet[field] = False
            capture = dict(packets=[packet], captureReady=True, exitCode=0, incompleteBytes=0,
                           statistics=dict(captured=1, receivedByFilter=1, droppedByKernel=0))
            result = EVIDENCE.evaluate_delivery(sends, capture, 1)
            self.assertEqual((result['status'], result['reason']), ('failed', 'udp_header_or_route_mismatch'))

    def test_header_gate_requires_complete_flags(self):
        """部分或伪布尔头部证据只能判不确定，不给旧证据补造头部校验。"""
        for flags in ({'routeMatch': True}, {'routeMatch': True, 'headerValid': 1}):
            capture = dict(packets=[dict(seq=1, bytes=23, **flags)], captureReady=True,
                           exitCode=0, incompleteBytes=0,
                           statistics=dict(captured=1, receivedByFilter=1, droppedByKernel=0))
            result = EVIDENCE.evaluate_delivery([dict(seq=1, bytes=23, sent=23)], capture, 1)
            self.assertEqual(result['reason'], 'capture_header_evidence_incomplete')
            self.assertEqual(result['status'], 'inconclusive')


if __name__ == '__main__':
    unittest.main()
