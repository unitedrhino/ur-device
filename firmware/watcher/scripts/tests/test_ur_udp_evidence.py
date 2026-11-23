"""用固定包头元数据验证完整性门禁；不伪造真实平台或硬件验收。"""
import copy
import importlib.util
import json
import os
from pathlib import Path
import tempfile
import unittest
import subprocess
import sys
from contextlib import redirect_stdout
import io

spec = importlib.util.spec_from_file_location('ur_udp_evidence', Path(__file__).resolve().parents[1] / 'ur_udp_evidence.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def evidence():
    """生成一组完整元数据；数字代表合成包，不表示真实套接字发送。"""
    sends = [{'seq': seq, 'bytes': 80 + seq, 'sent': 80 + seq} for seq in range(1, 90)]
    capture = {'captureReady': True, 'exitCode': 0, 'incompleteBytes': 0,
               'statistics': {'captured': 89, 'receivedByFilter': 89, 'droppedByKernel': 0},
               'packets': [{'seq': row['seq'], 'bytes': row['bytes']} for row in sends]}
    return sends, capture


class DeliveryTests(unittest.TestCase):
    """覆盖通过、已知四帧缺口、采集缺口、发送失败、乱序和非法证据。"""
    def test_complete(self):
        sends, capture = evidence()
        self.assertEqual(module.evaluate_delivery(sends, capture, 89)['status'], 'passed')

    def test_keyword_success_does_not_hide_four_missing_frames(self):
        sends, capture = evidence()
        capture['packets'] = [row for row in capture['packets'] if row['seq'] not in (42, 54, 55, 56)]
        capture['statistics']['captured'] = 85
        result = module.evaluate_delivery(sends, capture, 89)
        self.assertEqual(result['status'], 'failed')
        self.assertEqual(result['missingSequences'], [42, 54, 55, 56])
        # 即使外层识别闭环成功，该独立传输判据仍须失败。
        capture['roundtripPassed'] = True
        self.assertEqual(module.evaluate_delivery(sends, capture, 89)['status'], 'failed')

    def test_missing_statistics_never_defaults_to_zero(self):
        sends, capture = evidence()
        del capture['statistics']
        result = module.evaluate_delivery(sends, capture, 89)
        self.assertEqual((result['status'], result['reason']), ('inconclusive', 'capture_statistics_missing'))

    def test_capture_quality(self):
        sends, capture = evidence()
        for key, value in (('captureReady', False), ('exitCode', 1), ('incompleteBytes', 5)):
            modified = dict(capture, **{key: value})
            self.assertEqual(module.evaluate_delivery(sends, modified, 89)['status'], 'inconclusive')
        for key, value in (('captured', 88), ('droppedByKernel', 1)):
            modified = copy.deepcopy(capture)
            modified['statistics'][key] = value
            self.assertEqual(module.evaluate_delivery(sends, modified, 89)['status'], 'inconclusive')

    def test_socket_failed_or_short(self):
        for returned in (-1, 0, 3):
            sends, capture = evidence()
            sends[10]['sent'] = returned
            result = module.evaluate_delivery(sends, capture, 89)
            self.assertEqual(result['reason'], 'socket_submission_failed')
            self.assertEqual(result['socketFailureSequences'], [11])

    def test_early_gap_has_no_network_loss_claim(self):
        sends, capture = evidence()
        capture['packets'] = capture['packets'][2:]
        capture['statistics']['captured'] = 87
        result = module.evaluate_delivery(sends, capture, 89)
        self.assertEqual((result['status'], result['reason']), ('inconclusive', 'capture_start_unproven'))

    def test_missing_tail_requires_full_capture_quality(self):
        """尾帧89缺失必须失败；捕获丢包、缺终态或晚启动仍只判证据不足。"""
        sends, capture = evidence()
        capture['packets'] = capture['packets'][:-1]
        capture['statistics']['captured'] = 88
        # 1至88内部连续不等于覆盖计划中的全部89包，禁止从观察数缩小原预期。
        result = module.evaluate_delivery(sends, capture, 89)
        self.assertEqual((result['status'], result['reason']), ('failed', 'udp_delivery_mismatch'))
        self.assertEqual(result['missingSequences'], [89])
        for kind in ('kernel_drop', 'missing_statistics', 'nonzero_exit', 'late_start'):
            with self.subTest(kind=kind):
                modified = copy.deepcopy(capture)
                if kind == 'kernel_drop':
                    modified['statistics']['droppedByKernel'] = 1
                elif kind == 'missing_statistics':
                    del modified['statistics']
                elif kind == 'nonzero_exit':
                    modified['exitCode'] = 1
                else:
                    modified['packets'] = modified['packets'][1:]
                    modified['statistics']['captured'] = 87
                self.assertEqual(module.evaluate_delivery(sends, modified, 89)['status'], 'inconclusive')

    def test_order_duplicate_and_length(self):
        sends, capture = evidence()
        for kind in ('order', 'duplicate', 'length', 'unexpected'):
            modified = copy.deepcopy(capture)
            if kind == 'order':
                modified['packets'][4], modified['packets'][5] = modified['packets'][5], modified['packets'][4]
            elif kind == 'duplicate':
                modified['packets'].append(dict(modified['packets'][5]))
            elif kind == 'length':
                modified['packets'][5]['bytes'] += 1
            else:
                modified['packets'].append({'seq': 90, 'bytes': 90})
            modified['statistics']['captured'] = len(modified['packets'])
            self.assertEqual(module.evaluate_delivery(sends, modified, 89)['status'], 'failed')

    def test_sender_incomplete_or_duplicate(self):
        sends, capture = evidence()
        for modified in (sends[:-1], [sends[0]] + sends[2:], sends[::-1]):
            self.assertEqual(module.evaluate_delivery(modified, capture, 89)['reason'], 'sender_evidence_incomplete')

    def test_aborted_plan_keeps_failure_and_prefix_evidence_separate(self):
        """合成回放595秒失败的序号形状；已完成前缀不能代替原三轮计划。"""
        sends, capture = evidence()
        sends = sends[:60]
        capture['packets'] = [row for row in capture['packets'][:60] if row['seq'] not in (37, 47)]
        capture['statistics']['captured'] = 58
        planned = module.evaluate_delivery(sends, capture, 89)
        self.assertEqual((planned['status'], planned['reason']), ('inconclusive', 'sender_evidence_incomplete'))
        self.assertEqual(planned['missingSequences'], [37, 47])
        # 60须由两次完整INPUT_DONE及已知预热数确定，不从SEND日志长度自动猜测。
        prefix = module.evaluate_delivery(sends, capture, 60)
        self.assertEqual((prefix['status'], prefix['reason']), ('failed', 'udp_delivery_mismatch'))
        self.assertEqual(prefix['socketFailureSequences'], [])

    def test_invalid_types(self):
        sends, capture = evidence()
        for invalid in (None, {}, [None], [{'seq': True, 'bytes': 80, 'sent': 80}]):
            self.assertEqual(module.evaluate_delivery(invalid, capture, 89)['status'], 'inconclusive')
        self.assertEqual(module.evaluate_delivery(sends, capture, True)['status'], 'inconclusive')
        self.assertEqual(module.evaluate_delivery(sends, capture, 2**40)['status'], 'inconclusive')


class CaptureStatisticsTests(unittest.TestCase):
    """缺统计、重复终态、单复数均需明确判定，不输出 tcpdump 原文。"""
    def test_complete_and_singular(self):
        for noun, number in (('packets', 85), ('packet', 1)):
            raw = f'{number} {noun} captured\n{number} {noun} received by filter\n0 packets dropped by kernel\n'
            self.assertEqual(module.parse_capture_statistics(raw),
                             {'captured': number, 'receivedByFilter': number, 'droppedByKernel': 0})

    def test_missing_duplicate_or_invalid(self):
        for raw in ('listening on any', '85 packets captured\n85 packets received by filter',
                    '1 packet captured\n2 packets captured\n2 packets received by filter\n0 packets dropped by kernel',
                    '-1 packets captured\n2 packets received by filter\n0 packets dropped by kernel'):
            self.assertIsNone(module.parse_capture_statistics(raw))


class EvidenceCliTests(unittest.TestCase):
    """执行真实CLI与文件系统，验证跨会话隔离、脱敏和证据不覆盖。"""
    def test_scope_and_privacy(self):
        sends, capture = evidence()
        lines = ['private-token nonce-and-audio-content', 'method=sessionCreated session=abcdef01']
        lines.extend(f'[UR_UDP_E2E] SEND seq={row["seq"]} bytes={row["bytes"]} sent={row["sent"]}' for row in sends)
        lines += ['method=sessionCreated session=abcdef02', '[UR_UDP_E2E] SEND seq=1 bytes=90 sent=-1']
        self.assertEqual(module.session_sends('\n'.join(lines), 'abcdef01'), sends)
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            device = folder / 'device.log'
            ingress = folder / 'metadata.json'
            device.write_text('\n'.join(lines))
            ingress.write_text(json.dumps({'sessions': [dict(capture, session='abcdef01', nonce='private-nonce')]}))
            output = folder / 'private' / 'result.json'
            old_mask = os.umask(0o002)
            try:
                with redirect_stdout(io.StringIO()) as printed:
                    status = module.main(['--device-log', str(device), '--ingress-metadata', str(ingress),
                                          '--session', 'abcdef01', '--expected-packets', '89', '--output', str(output)])
                self.assertEqual(status, 0)
                self.assertEqual(output.stat().st_mode & 0o777, 0o600)
                self.assertEqual(output.parent.stat().st_mode & 0o777, 0o700)
                self.assertNotIn('private-', output.read_text() + printed.getvalue())
                original = output.read_bytes()
                with self.assertRaises(FileExistsError):
                    module.main(['--device-log', str(device), '--ingress-metadata', str(ingress),
                                 '--session', 'abcdef01', '--expected-packets', '89', '--output', str(output)])
                self.assertEqual(output.read_bytes(), original)
            finally:
                os.umask(old_mask)

    def test_duplicate_session_rejected(self):
        with self.assertRaises(ValueError):
            module.session_sends('method=sessionCreated session=abcdef01\nmethod=sessionCreated session=abcdef01', 'abcdef01')

    def test_netif_success_and_roundtrip_pass_cannot_hide_missing_frames(self):
        """合成回放真机缺34/89的形状；执行真实CLI，不冒充重新复现无线丢失。"""
        sends, capture = evidence()
        lines = ['[UR_AI_E2E] START turns=3', 'method=sessionCreated session=abcdef01',
                 '[UR_NETIF_TX] BEGIN gen=1']
        for row in sends:
            lines.append(f'[UR_UDP_E2E] SEND seq={row["seq"]} bytes={row["bytes"]} sent={row["sent"]}')
            lines.append(f'[UR_NETIF_TX] DONE gen=1 seq={row["seq"]} bytes={row["bytes"]} result=0 header=1 uptime_us=100')
        lines += ['[UR_NETIF_TX] END gen=1 calls=89 matched=89 dropped=0 ignored=0 pending=0',
                  '[UR_AI_E2E] DONE passed=1']
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            device = folder / 'device.log'
            ingress = folder / 'metadata.json'
            device.write_text('\n'.join(lines))
            for missing_sequence in (34, 89):
                with self.subTest(missing_sequence=missing_sequence):
                    modified = copy.deepcopy(capture)
                    modified['packets'] = [row for row in modified['packets'] if row['seq'] != missing_sequence]
                    modified['statistics']['captured'] = 88
                    ingress.write_text(json.dumps({'sessions': [dict(modified, session='abcdef01')]}))
                    # 独立子进程走生产命令入口，不能靠包装返回码或外层PASS覆盖入站缺口。
                    output = folder / f'missing-{missing_sequence}.json'
                    completed = subprocess.run([sys.executable, str(Path(module.__file__)),
                        '--device-log', str(device), '--ingress-metadata', str(ingress),
                        '--session', 'abcdef01', '--expected-packets', '89', '--output', str(output)],
                        capture_output=True, text=True)
                    self.assertEqual(completed.returncode, 1, completed.stderr)
                    result = json.loads(output.read_text())
                    self.assertEqual(result, json.loads(completed.stdout))
                    self.assertEqual((result['status'], result['reason']), ('failed', 'udp_delivery_mismatch'))
                    self.assertEqual(result['missingSequences'], [missing_sequence])
                    self.assertEqual(result['deviceLoggedPackets'], 89)
                    self.assertEqual(result['ingressPackets'], 88)
                    self.assertEqual(result['socketFailureSequences'], [])

    def test_process_exit_codes_and_invalid_schema(self):
        """真实子进程区分确定缺帧与统计缺失；非法文件不能让门禁误报通过。"""
        sends, capture = evidence()
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            device = folder / 'device.log'
            ingress = folder / 'metadata.json'
            device.write_text('method=sessionCreated session=abcdef01\n' + '\n'.join(
                f'[UR_UDP_E2E] SEND seq={row["seq"]} bytes={row["bytes"]} sent={row["sent"]}' for row in sends))
            missing = copy.deepcopy(capture)
            missing['packets'] = missing['packets'][:41] + missing['packets'][42:]
            missing['statistics']['captured'] = 88
            no_statistics = copy.deepcopy(capture)
            del no_statistics['statistics']
            cases = [(dict(capture, session='abcdef01'), 0, 'udp_sequence_delivery_complete'),
                     (dict(missing, session='abcdef01'), 1, 'udp_delivery_mismatch'),
                     (dict(no_statistics, session='abcdef01'), 2, 'capture_statistics_missing')]
            for index, (row, expected_exit, reason) in enumerate(cases):
                ingress.write_text(json.dumps({'sessions': [row]}))
                completed = subprocess.run([sys.executable, str(Path(module.__file__)),
                    '--device-log', str(device), '--ingress-metadata', str(ingress),
                    '--session', 'abcdef01', '--expected-packets', '89',
                    '--output', str(folder / f'result-{index}.json')], capture_output=True, text=True)
                self.assertEqual(completed.returncode, expected_exit, completed.stderr)
                self.assertEqual(json.loads(completed.stdout)['reason'], reason)
            for index, data in enumerate(([], None, {'sessions': None}, {'sessions': [None]},
                                          {'sessions': [dict(capture, session='abcdef01')] * 2})):
                ingress.write_text(json.dumps(data))
                with redirect_stdout(io.StringIO()):
                    code = module.main(['--device-log', str(device), '--ingress-metadata', str(ingress),
                        '--session', 'abcdef01', '--expected-packets', '89',
                        '--output', str(folder / f'invalid-{index}.json')])
                self.assertEqual(code, 2)

    def test_output_symlink_is_not_followed(self):
        """输出冲突必须在读取输入前失败，不修改符号链接的证据目标。"""
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            original = folder / 'old.json'
            original.write_text('original-failure')
            output = folder / 'linked.json'
            output.symlink_to(original)
            with self.assertRaises(FileExistsError):
                module.main(['--device-log', str(folder / 'nonexistent.log'),
                    '--ingress-metadata', str(folder / 'nonexistent.json'),
                    '--session', 'abcdef01', '--expected-packets', '89', '--output', str(output)])
            self.assertEqual(original.read_text(), 'original-failure')


if __name__ == '__main__':
    unittest.main()
