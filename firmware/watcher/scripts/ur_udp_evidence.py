#!/usr/bin/env python3
"""校验受控语音会话的发送与入站元数据；闭环成功不能代替音频完整性。"""

import argparse
import json
import os
from pathlib import Path
import re
import sys

# MAX_EVIDENCE_PACKETS 限制单会话证据规模，避免异常预期值构造巨大序号列表。
MAX_EVIDENCE_PACKETS = 40000


def parse_capture_statistics(stderr):
    """从 tcpdump 的英文终态提取三项计数；缺失或重复时返回 None，不猜测零丢包。"""
    labels = {'captured': 'packets? captured',
              'receivedByFilter': 'packets? received by filter',
              'droppedByKernel': 'packets? dropped by kernel'}
    counts = {}
    for name, label in labels.items():
        values = re.findall(r'^([0-9]+) ' + label + r'\s*$', stderr, re.MULTILINE)
        if len(values) != 1:
            return None
        counts[name] = int(values[0])
    return counts


def _integer(value, minimum=0):
    """验证整数元数据，拒绝布尔值和负长度，避免宽松转换制造假通过。"""
    return type(value) is int and value >= minimum


def evaluate_delivery(sends, capture, expected_packets):
    """对照同会话实际序号/长度与采集质量，返回 passed/failed/inconclusive 及脱敏原因。"""
    result = {'status': 'inconclusive', 'reason': 'invalid_evidence'}
    if (not _integer(expected_packets, 1) or expected_packets > MAX_EVIDENCE_PACKETS or not isinstance(sends, list)
            or not isinstance(capture, dict) or not isinstance(capture.get('packets'), list)):
        return result
    packets = capture['packets']
    if any(not isinstance(row, dict) or not _integer(row.get('seq'), 1)
           or not _integer(row.get('bytes'), 17) or type(row.get('sent')) is not int for row in sends):
        return result
    if any(not isinstance(row, dict) or not _integer(row.get('seq'), 1)
           or not _integer(row.get('bytes'), 17) for row in packets):
        return result
    # 端点视角不能仅凭相同序号/长度判为通过；新增头部证据必须逐包完整，旧证据不补造。
    has_headers = any('routeMatch' in row or 'headerValid' in row for row in packets)
    if has_headers and any(type(row.get(name)) is not bool for row in packets
                           for name in ('routeMatch', 'headerValid')):
        return dict(result, reason='capture_header_evidence_incomplete')
    sent_seq = [row['seq'] for row in sends]
    arrived_seq = [row['seq'] for row in packets]
    lookup = {row['seq']: row['bytes'] for row in packets}
    missing = sorted(set(sent_seq) - set(arrived_seq))
    unexpected = sorted(set(arrived_seq) - set(sent_seq))
    mismatched = sorted({row['seq'] for row in sends
                         if row['seq'] in lookup and row['bytes'] != lookup[row['seq']]})
    socket_errors = [row['seq'] for row in sends if row['sent'] != row['bytes']]
    result.update(deviceLoggedPackets=len(sends), ingressPackets=len(packets),
                  missingSequences=missing, unexpectedSequences=unexpected,
                  lengthMismatchSequences=mismatched, socketFailureSequences=socket_errors,
                  sameSequenceOrder=sent_seq == arrived_seq)
    # 已知发送失败不依赖抓包质量，但不能把只提交成功称作送达。
    if socket_errors:
        return dict(result, status='failed', reason='socket_submission_failed')
    if (len(sends) != expected_packets or len(set(sent_seq)) != expected_packets
            or sent_seq != list(range(1, expected_packets + 1))):
        return dict(result, reason='sender_evidence_incomplete')
    statistics = capture.get('statistics')
    if (not isinstance(statistics, dict) or any(not _integer(statistics.get(name)) for name in
            ('captured', 'receivedByFilter', 'droppedByKernel'))):
        return dict(result, reason='capture_statistics_missing')
    if (capture.get('captureReady') is not True or type(capture.get('exitCode')) is not int
            or capture['exitCode'] != 0 or capture.get('incompleteBytes') != 0
            or type(capture.get('incompleteBytes')) is not int):
        return dict(result, reason='capture_incomplete')
    if statistics['droppedByKernel'] != 0 or statistics['captured'] != len(packets):
        return dict(result, reason='capture_dropped_or_unparsed_packets')
    # 第一包缺失可能发生在观测器就绪之前，不能仅凭后续包证明整段观测完整。
    if not arrived_seq or 1 not in arrived_seq:
        return dict(result, reason='capture_start_unproven')
    if has_headers and any(not row['routeMatch'] or not row['headerValid'] for row in packets):
        return dict(result, status='failed', reason='udp_header_or_route_mismatch')
    if missing or unexpected or mismatched or sent_seq != arrived_seq:
        return dict(result, status='failed', reason='udp_delivery_mismatch')
    return dict(result, status='passed', reason='udp_sequence_delivery_complete')


def session_sends(transcript, session):
    """仅提取指定短会话的数值发送日志，其他会话、正文和密钥不进入返回值。"""
    active = None
    sends = []
    seen = 0
    for line in transcript.splitlines():
        if '[UR_AI_E2E] START' in line:
            active = None
        match = re.search(r'method=sessionCreated session=([a-f0-9]{8})(?![a-f0-9])', line)
        if match:
            active = match[1]
            seen += active == session
        if active != session:
            continue
        match = re.search(r'\[UR_UDP_E2E\] SEND seq=(\d+) bytes=(\d+) sent=(-?\d+)', line)
        if match:
            sends.append(dict(zip(('seq', 'bytes', 'sent'), map(int, match.groups()))))
    if seen != 1:
        raise ValueError('session_missing_or_duplicated')
    return sends


def main(argv=None):
    """读取已有受限证据并独占保存结论；0通过、1确定失败、2证据不完整，不连接平台。"""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--device-log', type=Path, required=True)
    parser.add_argument('--ingress-metadata', type=Path, required=True)
    parser.add_argument('--session', required=True)
    parser.add_argument('--expected-packets', type=int, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args(argv)
    if not re.fullmatch('[a-f0-9]{8}', args.session) or not 0 < args.expected_packets <= MAX_EVIDENCE_PACKETS:
        parser.error('需要有效短会话ID和不超过40000的正预期包数')
    os.umask(0o077)
    args.output.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    # 排他创建先于读取，旧失败、符号链接和同名重跑都不能被覆盖。
    with args.output.open('x') as output:
        try:
            sends = session_sends(args.device_log.read_text(errors='replace'), args.session)
            data = json.loads(args.ingress_metadata.read_text())
            if not isinstance(data, dict) or not isinstance(data.get('sessions'), list):
                raise ValueError('capture_schema_invalid')
            candidates = [row for row in data['sessions'] if isinstance(row, dict) and row.get('session') == args.session]
            if len(candidates) != 1:
                raise ValueError('capture_session_missing_or_duplicated')
            result = evaluate_delivery(sends, candidates[0], args.expected_packets)
        except (KeyError, TypeError, ValueError, OSError):
            result = {'status': 'inconclusive', 'reason': 'input_evidence_invalid'}
        result['session'] = args.session
        json.dump(result, output)
    print(json.dumps(result), flush=True)
    return {'passed': 0, 'failed': 1, 'inconclusive': 2}[result['status']]


if __name__ == '__main__':
    sys.exit(main())
