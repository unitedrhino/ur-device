#!/usr/bin/env python3
"""只读 OTA 下载回归：用小 TCP 窗口与慢速消费复现背压，只保存长度和摘要，不输出下载地址。"""
import argparse
import hashlib
import http.client
import json
import os
from pathlib import Path
import re
import socket
import ssl
import stat
import time
from urllib.parse import urlsplit


def download(url, expected_size, expected_md5, rate=12288, receive_buffer=4096, timeout=600):
    """按指定字节速率读取单个应用包；返回实际大小、MD5 和失败分类，不保存固件或凭据。"""
    parsed = urlsplit(url)
    if (parsed.scheme not in ('http', 'https') or not parsed.hostname
            or parsed.username or parsed.password or parsed.fragment):
        raise ValueError('invalid download URL')
    if (expected_size <= 0 or rate <= 0 or receive_buffer <= 0 or timeout <= 0
            or not re.fullmatch(r'[0-9a-fA-F]{32}', expected_md5)):
        raise ValueError('invalid verification parameters')
    expected_md5 = expected_md5.lower()
    started = time.monotonic()
    digest = hashlib.md5()
    result = dict(status=None, declared=None, received=0, md5=None,
                  expectedSize=expected_size, expectedMD5=expected_md5,
                  receiveBuffer=None, error=None, passed=False)
    connection = None
    transport = None
    try:
        port = parsed.port or (443 if parsed.scheme == 'https' else 80)
        # 必须在 connect 前设置窗口；只用 curl 限速会被内核缓冲掩盖。
        address = socket.getaddrinfo(parsed.hostname, port, type=socket.SOCK_STREAM)[0]
        transport = socket.socket(address[0], address[1], address[2])
        transport.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, receive_buffer)
        transport.settimeout(min(timeout, 30))
        transport.connect(address[4])
        result['receiveBuffer'] = transport.getsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF)
        if parsed.scheme == 'https':
            transport = ssl.create_default_context().wrap_socket(transport, server_hostname=parsed.hostname)
        connection = http.client.HTTPConnection(parsed.hostname, port)
        connection.sock = transport
        target = (parsed.path or '/') + ('?' + parsed.query if parsed.query else '')
        connection.request('GET', target, headers={'Connection': 'close'})
        response = connection.getresponse()
        result['status'] = response.status
        result['declared'] = int(response.getheader('Content-Length', '0'))
        if response.status != 200:
            result['error'] = 'http_status'
        elif result['declared'] != expected_size:
            result['error'] = 'declared_size'
        else:
            # HTTPResponse 在读完声明长度后可能关闭底层 socket；完整后不再设置其期限。
            while result['received'] < expected_size:
                remaining = timeout - (time.monotonic() - started)
                if remaining <= 0:
                    raise TimeoutError()
                transport.settimeout(min(remaining, 30))
                chunk = response.read(4096)
                if not chunk:
                    break
                digest.update(chunk)
                result['received'] += len(chunk)
                pause = max(0, started + result['received'] / rate - time.monotonic())
                if pause >= timeout - (time.monotonic() - started):
                    raise TimeoutError()
                time.sleep(pause)
            if result['received'] != expected_size:
                result['error'] = 'actual_size'
            elif digest.hexdigest() != expected_md5:
                result['error'] = 'md5'
    except Exception as error:
        # 异常正文可能包含签名 URL，不进入控制台或证据。
        result['error'] = type(error).__name__
    finally:
        if connection:
            connection.close()
        if transport:
            transport.close()
    result['md5'] = digest.hexdigest()
    result['elapsed'] = round(time.monotonic() - started, 3)
    result['passed'] = result['error'] is None
    return result


def main():
    """从受限文件读取平台返回的 URL；输出独占创建，避免覆盖旧证据。"""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--url-file', required=True, type=Path)
    parser.add_argument('--size', required=True, type=int)
    parser.add_argument('--md5', required=True)
    parser.add_argument('--rate', type=int, default=12288)
    parser.add_argument('--receive-buffer', type=int, default=4096)
    parser.add_argument('--timeout', type=float, default=600)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    info = args.url_file.lstat()
    if not stat.S_ISREG(info.st_mode) or stat.S_IMODE(info.st_mode) & 0o077:
        parser.error('URL 文件必须为受限普通文件（权限 0600）')
    # 不改变调用进程的全局 umask，同时拒绝覆盖旧证据或跟随已有链接。
    descriptor = os.open(args.output, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(descriptor, 'w') as output:
        result = download(args.url_file.read_text().strip(), args.size, args.md5,
                          args.rate, args.receive_buffer, args.timeout)
        json.dump(result, output)
    print(json.dumps(result))
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
