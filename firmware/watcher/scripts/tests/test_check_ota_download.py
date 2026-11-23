"""验证真实 HTTP 下载探针：200 截断、错摘要与超时不得误判 OTA 下载通过。"""
import hashlib
import contextlib
import io
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import sys
import threading
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from check_ota_download import download, main


class DownloadTest(unittest.TestCase):
    """用本地 HTTP 服务验证生产探针逻辑，不访问平台或设备。"""

    def test_http_cases(self):
        """相同内容覆盖成功、提前断开、声明错误、HTTP 错误、MD5 错误和总超时。"""
        payload = b'firmware-fixture' * 1024

        class Handler(BaseHTTPRequestHandler):
            """测试服务可返回缺失的响应尾部，模拟代理上游提前 EOF。"""

            def do_GET(self):
                """根据固定测试路径控制响应，禁止请求日志泄露测试签名。"""
                self.send_response(503 if self.path.startswith('/status') else 200)
                self.send_header('Content-Length', str(len(payload) + (1 if self.path.startswith('/size') else 0)))
                self.end_headers()
                try:
                    self.wfile.write(payload[:4096] if self.path.startswith('/short') else payload)
                except (BrokenPipeError, ConnectionResetError):
                    pass

            def log_message(self, *_args):
                """测试禁用包含 URL 的 HTTP 默认日志。"""

        server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        try:
            for path, md5, rate, timeout, passed, error in [
                ('ok', hashlib.md5(payload).hexdigest(), 10000000, 5, True, None),
                ('short', hashlib.md5(payload).hexdigest(), 10000000, 5, False, 'actual_size'),
                ('size', hashlib.md5(payload).hexdigest(), 10000000, 5, False, 'declared_size'),
                ('status', hashlib.md5(payload).hexdigest(), 10000000, 5, False, 'http_status'),
                ('ok', '0' * 32, 10000000, 5, False, 'md5'),
                ('ok', hashlib.md5(payload).hexdigest(), 1, 0.2, False, 'TimeoutError'),
            ]:
                with self.subTest(path=path, error=error):
                    result = download(f'http://127.0.0.1:{server.server_port}/{path}?signature=private',
                                      len(payload), md5, rate, 4096, timeout)
                    self.assertEqual(result['passed'], passed)
                    self.assertEqual(result['error'], error)
                    self.assertNotIn('private', str(result))
        finally:
            server.shutdown()
            server.server_close()
            worker.join()

    def test_invalid_inputs(self):
        """非法协议、内联账号、无效窗口和摘要在建立连接前拒绝。"""
        for url, size, md5, rate, buffer in [
            ('file:///secret', 1, '0' * 32, 1, 1),
            ('http://user:secret@localhost/', 1, '0' * 32, 1, 1),
            ('http://localhost/', 0, '0' * 32, 1, 1),
            ('http://localhost/', 1, 'bad', 1, 1),
            ('http://localhost/', 1, '0' * 32, 0, 1),
            ('http://localhost/', 1, '0' * 32, 1, 0),
        ]:
            with self.subTest(url=url):
                with self.assertRaises(ValueError):
                    download(url, size, md5, rate, buffer)

    def test_cli_evidence_and_permissions(self):
        """凭据文件过宽与证据已存在时均不发请求；成功输出文件仅当前用户可读。"""
        with tempfile.TemporaryDirectory() as directory:
            url_file = Path(directory) / 'url.txt'
            output = Path(directory) / 'result.json'
            url_file.write_text('http://localhost/?signature=private')
            args = ['probe', '--url-file', str(url_file), '--size', '1',
                    '--md5', '0' * 32, '--output', str(output)]
            with patch.object(sys, 'argv', args), patch('check_ota_download.download') as probe:
                url_file.chmod(0o644)
                with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                    main()
                probe.assert_not_called()
                url_file.chmod(0o600)
                output.write_text('previous evidence')
                with self.assertRaises(FileExistsError):
                    main()
                self.assertEqual(output.read_text(), 'previous evidence')
                probe.assert_not_called()
                output.unlink()
                probe.return_value = {'passed': True}
                with contextlib.redirect_stdout(io.StringIO()):
                    self.assertEqual(main(), 0)
                self.assertEqual(output.stat().st_mode & 0o777, 0o600)
