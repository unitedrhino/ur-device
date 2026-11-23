"""受控 UDP 抓包元数据解析：端点视角保留错路由报文，原始包只在内存中使用。"""

import ipaddress
import struct


class UdpCaptureReader:
    """解析112字节classic pcap；routing为四字节路由，endpoint为已核实源IPv4/端口。

    路由模式仅学习目标端点；独立端点模式不以路由头过滤报文。两种捕获各自
    需要实际tcpdump统计及就绪/终态，不能拼接后假称完整观测或持久保存端点。
    """

    def __init__(self, port, routing, endpoint=None):
        """校验观察范围；端点模式必须显式传入，禁止从错路由报文猜测所属设备。"""
        if type(port) is not int or not 0 < port <= 65535 or type(routing) is not bytes or len(routing) != 4:
            raise ValueError('invalid_capture_scope')
        self.port, self.routing = port, routing
        self.endpoint_mode = endpoint is not None
        self.endpoint = self._endpoint(endpoint) if endpoint is not None else None
        self.buffer = bytearray()
        self.order = self.offset = None
        self.records = []

    @staticmethod
    def _endpoint(endpoint):
        """将明确源端点规范化，仅在内存中保存；拒绝IPv6、零端口与布尔值。"""
        if not isinstance(endpoint, tuple) or len(endpoint) != 2:
            raise ValueError('invalid_source_endpoint')
        address, port = endpoint
        try:
            address = ipaddress.IPv4Address(address).packed
        except (ValueError, TypeError):
            raise ValueError('invalid_source_endpoint') from None
        if type(port) is not int or not 0 < port <= 65535:
            raise ValueError('invalid_source_endpoint')
        return address, port

    def feed(self, data):
        """有界增量读取pcap；非法封装或越界抛错，不将解析失败的包静默算作缺包。"""
        if not isinstance(data, bytes) or len(data) > 65536:
            raise ValueError('invalid_capture_chunk')
        self.buffer.extend(data)
        if self.order is None:
            if len(self.buffer) < 24:
                return
            magic = bytes(self.buffer[:4])
            if magic not in (b'\xd4\xc3\xb2\xa1', b'\xa1\xb2\xc3\xd4'):
                raise ValueError('unsupported_pcap_format')
            self.order = '<' if magic[0] == 0xd4 else '>'
            major, minor, _, _, snaplen, link = struct.unpack_from(self.order + 'HHIIII', self.buffer, 4)
            if (major, minor) != (2, 4) or snaplen != 112 or link not in (1, 113, 276):
                raise ValueError('unsupported_pcap_header')
            self.offset = {1: 14, 113: 16, 276: 20}[link]
            del self.buffer[:24]
        while len(self.buffer) >= 16:
            seconds, micros, size, original = struct.unpack_from(self.order + 'IIII', self.buffer)
            if not 0 < size <= 112 or size > original or micros >= 1000000:
                raise ValueError('invalid_pcap_record')
            if len(self.buffer) < 16 + size:
                return
            packet = bytes(self.buffer[16:16 + size])
            del self.buffer[:16 + size]
            self._packet(packet, original, seconds * 1000000 + micros)
            if len(self.records) > 2000:
                raise ValueError('capture_packet_limit')

    def _packet(self, packet, original, epoch_us):
        """验证IPv4/UDP端点，输出序号、长度、时间和头部有效性，不输出地址或音频。"""
        start = self.offset
        if len(packet) < start + 28 or packet[start] >> 4 != 4:
            raise ValueError('invalid_ipv4_capture')
        ihl = (packet[start] & 15) * 4
        if ihl < 20 or len(packet) < start + ihl + 8 or packet[start + 9] != 17:
            raise ValueError('invalid_udp_capture')
        udp = start + ihl
        source, destination, length = struct.unpack_from('!HHH', packet, udp)
        endpoint = (packet[start + 12:start + 16], source)
        if destination != self.port or (self.endpoint_mode and endpoint != self.endpoint):
            raise ValueError('capture_scope_mismatch')
        header = packet[udp + 8:udp + 24]
        route_match = len(header) == 16 and header[4:8] == self.routing
        if not self.endpoint_mode:
            if not route_match:
                raise ValueError('capture_scope_mismatch')
            if self.endpoint is None:
                self.endpoint = endpoint
            elif self.endpoint != endpoint:
                raise ValueError('target_endpoint_changed')
        total = struct.unpack_from('!H', packet, start + 2)[0]
        fragment = struct.unpack_from('!H', packet, start + 6)[0] & 0x3fff
        valid = (not fragment and total >= ihl + 25 and length == total - ihl
                 and original >= start + total and len(header) == 16 and header[0] == 1
                 and struct.unpack_from('!H', header, 2)[0] == length - 24)
        self.records.append({'epochUs': epoch_us, 'bytes': max(0, length - 8),
                             'seq': struct.unpack_from('!I', header, 12)[0] if len(header) == 16 else None,
                             'routeMatch': route_match, 'headerValid': bool(valid)})
