#!/usr/bin/env python3
"""临时自有CA和真实TLS端点驱动正式C++ HTTP适配测试；不连接平台、OSS或真实模型。"""
import http.server
import hashlib
import json
import os
import pathlib
import shutil
import ssl
import subprocess
import sys
import tempfile
import threading
import time


class Endpoint(http.server.BaseHTTPRequestHandler):
    """有界读取真实原件并检查字节；不记录认证头、URL、自由异常正文或请求内容。"""
    redirected = False  # 只由本测试唯一进程的端点访问，读写在同一锁下。
    lock = threading.Lock()  # 迟到的取消/超时请求不影响其他端点检查。
    stop = threading.Event()  # 提前结束有界延迟，确保结束后没有后台handler。
    uploads = {}  # 仅本次合成元数据和原件摘要；不冒充平台权限或真实OSS。

    def log_message(self, *_args):
        """默认HTTP日志会包含签名查询，测试与正式驱动均禁止输出。"""

    def do_POST(self):
        """POST和PUT使用相同确定性字节判据，HTTP成功本身不能替代读取核验。"""
        self.connection.settimeout(3)
        try:
            length = int(self.headers.get("Content-Length", "-1"))
            if length < 0 or length > 50 * 1024 * 1024:
                self.respond(400, b"invalid")
                return
            body = self.rfile.read(length)
            if len(body) != length:
                return
            if self.path.startswith("/api/v1/things/device/edge/content/"):
                self.metadata(body)
            elif self.path.startswith("/upload/"):
                request_id = self.path.removeprefix("/upload/")
                with self.lock:
                    record = Endpoint.uploads.get(request_id)
                    valid = (self.command == "PUT" and self.headers.get("Authorization") is None
                             and record is not None and len(body) == record["size"]
                             and hashlib.sha256(body).hexdigest() == record["sha256"])
                    if valid:
                        record["received"] = True
                self.respond(204 if valid else 400, b"")
            elif self.path == "/bytes":
                valid = all(value == index % 251 for index, value in enumerate(body))
                self.respond(200 if valid else 400, b'{"verified":true}' if valid else b"invalid")
            elif self.path == "/limit":
                self.respond(200, b"x" * 1024)
            elif self.path == "/redirect":
                self.respond(307, b"sensitive-error-body", location="/sink")
            elif self.path == "/sink":
                with self.lock:
                    Endpoint.redirected = True
                self.respond(200, b"bad-redirect")
            elif self.path == "/check-redirect":
                with self.lock:
                    redirected = Endpoint.redirected
                self.respond(200, b'{"redirected":true}' if redirected else b'{"redirected":false}')
            elif self.path == "/error":
                self.respond(403, b"synthetic-private-receipt-and-signed-url")
            elif self.path == "/delay":
                self.stop.wait(1)
                self.respond(200, b"late")
            else:
                self.respond(404, b"unknown")
        except (OSError, ValueError):
            # 源失败或取消导致的断连是被测事实，不能输出可能包含凭据的异常正文。
            return

    do_PUT = do_POST

    def metadata(self, body):
        """明确离线业务fixture；校验三步关联和实际原件hash，不授予真实平台身份。"""
        if (self.command != "POST" or self.headers.get("Authorization") !=
                "Basic c3ludGhldGljOnByaXZhdGU="):
            self.respond(403, b"synthetic-secret-error")
            return
        try:
            data = json.loads(body)
            if self.path.endswith("/prepare"):
                if set(data) != {"sessionId", "requestId", "name", "mimeType", "size", "sha256"}:
                    self.respond(400, b"invalid")
                    return
                request_id = data["requestId"]
                with self.lock:
                    Endpoint.uploads[request_id] = {**data, "received": False}
                output = {"contentRef": "artifact_" + request_id, "type": "file",
                          "mimeType": data["mimeType"], "status": "uploading",
                          "uploadUrl": f"https://localhost:{self.server.server_port}/upload/{request_id}",
                          "uploadReceipt": "synthetic-receipt-" + request_id,
                          "expiresAt": int(time.time() * 1000) + 10000}
            elif self.path.endswith("/complete"):
                if set(data) != {"sessionId", "contentRef", "uploadReceipt"}:
                    self.respond(400, b"invalid")
                    return
                request_id = data["contentRef"].removeprefix("artifact_")
                with self.lock:
                    record = Endpoint.uploads.get(request_id)
                    valid = (record is not None and record["received"] and
                             data["sessionId"] == record["sessionId"] and
                             data["uploadReceipt"] == "synthetic-receipt-" + request_id)
                if not valid:
                    self.respond(403, b"synthetic-secret-error")
                    return
                output = {"contentRef": data["contentRef"], "type": "file",
                          "mimeType": record["mimeType"], "status": "ready"}
            else:
                self.respond(404, b"unknown")
                return
            self.respond(200, json.dumps({"code": 200, "msg": "success", "data": output}).encode())
        except (ValueError, KeyError, TypeError):
            self.respond(400, b"invalid")

    def respond(self, status, body, location=None):
        """为真实端点发送明确长度；不读取或回显平台身份。"""
        self.send_response(status)
        self.send_header("Content-Length", str(len(body)))
        if location:
            self.send_header("Location", location)
        self.end_headers()
        self.wfile.write(body)


def main():
    """临时证书仅在传入二进制所属.temp目录生成；进程/端点/证书均由本入口收尾。"""
    if len(sys.argv) != 2:
        raise SystemExit("用法：run_https_test.py <desktop_http_tests>")
    binary = pathlib.Path(sys.argv[1]).resolve(strict=True)
    openssl = shutil.which("openssl")
    if not openssl:
        raise SystemExit("缺少openssl，真实TLS适配验证未执行")
    with tempfile.TemporaryDirectory(prefix="https-contract-", dir=binary.parent) as temporary:
        certificate = pathlib.Path(temporary) / "ca.pem"
        key = pathlib.Path(temporary) / "key.pem"
        subprocess.run([openssl, "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                        "-keyout", str(key), "-out", str(certificate), "-days", "1",
                        "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost"],
                       check=True, timeout=15, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        os.chmod(key, 0o600)
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Endpoint)
        server.daemon_threads = False
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.minimum_version = ssl.TLSVersion.TLSv1_2
        context.load_cert_chain(certificate, key)
        server.socket = context.wrap_socket(server.socket, server_side=True)
        thread = threading.Thread(target=server.serve_forever, daemon=False)
        try:
            thread.start()
            environment = os.environ.copy()
            environment.update(UR_TEST_HTTPS_URL=f"https://localhost:{server.server_port}",
                               UR_TEST_HTTPS_MISMATCH_URL=f"https://127.0.0.1:{server.server_port}",
                               UR_TEST_HTTPS_CA=str(certificate))
            result = subprocess.run([str(binary)], env=environment, timeout=25, check=False)
        finally:
            Endpoint.stop.set()
            server.shutdown()
            server.server_close()
            thread.join(timeout=3)
            if thread.is_alive():
                raise RuntimeError("本任务HTTPS端点未退出")
        raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
