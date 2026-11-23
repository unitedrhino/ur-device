#!/usr/bin/env python3
"""真实device_text附件宿主回归：合成MQTT平台＋原TLS/PUT端口；不连接实际平台、OSS或模型。"""
import base64
import hashlib
import hmac
import http.server
import json
import os
import pathlib
import queue
import shutil
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest

sys.dont_write_bytecode = True  # 本测试不在维护源码目录生成import缓存。
from run_https_test import Endpoint
from text_entry_test import packet, send_packet


class ContentTextEntryTest(unittest.TestCase):
    """实际stdin命令经worker三阶段上传，再由原事件线程提交typed引用；取消不冒充回复完成。"""
    binary = None  # CTest传入当前实际产物，不借旧device_text。

    def exercise(self, kind, action=None, selected=True, configured=True):
        """action为cancel/quit时在真实PUT响应屏障撤销原意图，原线程/端点均有精确收尾。"""
        listener = socket.socket()
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        listener.settimeout(5)
        failures, lines = queue.Queue(), queue.Queue()
        stopped, put_entered, release_put = threading.Event(), threading.Event(), threading.Event()
        connections, inputs, stages = [], [], []  # 只驻留合成协议/阶段，绝不写默认日志。
        auth = []  # 实际CONNECT的已校验短期Basic，只用于同次HTTPS一致性比较。
        secret = b"public-offline-content-key"  # 合成fixture密钥，非平台凭据。
        session = "content-pipe-session"  # 合成scope，不从自由响应选择新身份。
        process = reader = tls = tls_thread = mqtt_thread = None

        class MetadataEndpoint(Endpoint):
            """仅替换原metadata合成授权/类型；PUT读取、实际hash核验、TLS仍复用原fixture。"""
            def metadata(endpoint, body):
                """核同次实际MQTT Basic与真实三步关联；此匹配不是生产平台权限模拟通过。"""
                if not auth or endpoint.headers.get("Authorization") != auth[0]:
                    failures.put(True)
                    endpoint.respond(403, b"invalid")
                    return
                try:
                    data = json.loads(body)
                    if endpoint.path.endswith("/prepare"):
                        valid = (set(data) == {"sessionId", "requestId", "name", "mimeType", "size", "sha256"}
                                 and data["sessionId"] == session and data["mimeType"] == mime
                                 and data["name"] == source.name and data["size"] == len(original)
                                 and data["sha256"] == hashlib.sha256(original).hexdigest()
                                 and len(data["requestId"]) == 32
                                 and all(c in "0123456789abcdef" for c in data["requestId"]))
                        if not valid:
                            failures.put(True)
                            endpoint.respond(400, b"invalid")
                            return
                        request_id = data["requestId"]
                        with Endpoint.lock:
                            Endpoint.uploads[request_id] = {**data, "received": False}
                        stages.append("prepare")
                        output = {"contentRef": "artifact_" + request_id, "type": kind,
                                  "mimeType": mime, "status": "uploading",
                                  "uploadUrl": f"https://localhost:{endpoint.server.server_port}/upload/{request_id}",
                                  "uploadReceipt": "synthetic-receipt-" + request_id,
                                  "expiresAt": int(time.time() * 1000) + 10000}
                    elif endpoint.path.endswith("/complete"):
                        request_id = data["contentRef"].removeprefix("artifact_")
                        with Endpoint.lock:
                            record = Endpoint.uploads.get(request_id)
                            valid = (set(data) == {"sessionId", "contentRef", "uploadReceipt"}
                                     and record is not None and record["received"]
                                     and data["sessionId"] == session
                                     and data["uploadReceipt"] == "synthetic-receipt-" + request_id)
                        if not valid:
                            failures.put(True)
                            endpoint.respond(403, b"invalid")
                            return
                        stages.append("complete")
                        output = {"contentRef": data["contentRef"], "type": kind, "mimeType": mime, "status": "ready"}
                    else:
                        failures.put(True)
                        endpoint.respond(400, b"invalid")
                        return
                    endpoint.respond(200, json.dumps({"code": 200, "data": output}).encode())
                except (KeyError, ValueError, TypeError):
                    failures.put(True)
                    endpoint.respond(400, b"invalid")

            def respond(endpoint, status, body, location=None):
                """PUT成功证明原hash/字节已过；受控屏障确保用户取消发生在Complete/提交前。"""
                if endpoint.command == "PUT" and status == 204:
                    stages.append("put")
                    put_entered.set()
                    if action and not release_put.wait(5):
                        failures.put(True)
                super().respond(status, body, location)

        class Server(http.server.ThreadingHTTPServer):
            """服务异常仅记布尔，默认traceback不得带HTTP请求或私有文件路径。"""
            daemon_threads = False

            def handle_error(server, request, address):
                failures.put(True)

        def mqtt():
            """真正读取CONNECT签名/订阅与实际上行inputSend；绝不打印认证报文或自由正文。"""
            try:
                stream, _ = listener.accept()
                connections.append(stream)
                stream.settimeout(8)
                with stream:
                    sequence = 0
                    while not stopped.is_set():
                        header, data = packet(stream)
                        wire_kind = header >> 4
                        if wire_kind == 1:
                            offset = 10  # MQTT3.1.1固定原variable header。

                            def take_string():
                                """只在内存解析固定CONNECT字段，不把异常值输出到断言。"""
                                nonlocal offset
                                length = struct.unpack("!H", data[offset:offset + 2])[0]
                                offset += 2
                                value = data[offset:offset + length]
                                offset += length
                                return value

                            client, username, password = take_string(), take_string(), take_string()
                            valid = (client == b"test&text" and username.startswith(client + b";12010126;")
                                     and hmac.compare_digest(hmac.new(secret, username, hashlib.sha256).hexdigest().encode() + b";hmacsha256", password))
                            if not valid:
                                failures.put(True)
                                return
                            auth.append("Basic " + base64.b64encode(username + b":" + password).decode())
                            send_packet(stream, 0x20, b"\0\0")
                        elif wire_kind == 8:
                            send_packet(stream, 0x90, data[:2] + b"\1")
                        elif wire_kind == 12:
                            send_packet(stream, 0xd0, b"")
                        elif wire_kind == 14:
                            return
                        elif wire_kind == 3:
                            length = struct.unpack("!H", data[:2])[0]
                            offset = length + 2
                            if (header >> 1) & 3:
                                send_packet(stream, 0x40, data[offset:offset + 2])
                                offset += 2
                            request = json.loads(data[offset:])
                            method = request["method"]
                            reply = {"msgToken": request["msgToken"], "method": "requestAck", "code": 200}
                            if "sessionId" in request:
                                reply["sessionId"] = request["sessionId"]
                            messages = []
                            if method == "capabilityReport":
                                valid = request["params"]["inputModalities"] == (["text", "image", "file"] if configured else ["text"])
                                if not valid:
                                    failures.put(True)
                                    return
                                reply["data"] = {"requestMethod": method, "result": {"acceptedRevision": 1}}
                            elif method == "sessionCreate":
                                reply.update(method="sessionCreated", sessionId=session,
                                    data={"state": "negotiating", "selected": {"transport": "none",
                                          "inputModalities": ["text", "image", "file"] if selected and configured else ["text"],
                                          "outputModalities": ["text"]}, "tracks": []})
                                messages.append((False, {"msgToken": "established", "method": "sessionEstablished", "sessionId": session,
                                    "eventSeq": 1, "data": {"state": "established", "mediaRevision": 0}}))
                            elif method == "inputSend":
                                inputs.append(True)
                                contents = request["params"]["contents"]
                                valid = (not action and stages == ["prepare", "put", "complete"]
                                         and request["sessionId"] == session and len(contents) == 1
                                         and set(contents[0]) == {"type", "contentRef", "mimeType"}
                                         and contents[0]["type"] == kind and contents[0]["mimeType"] == mime
                                         and contents[0]["contentRef"].startswith("artifact_"))
                                if not valid:
                                    failures.put(True)
                                    return
                                source_id = request["params"]["inputId"]
                                reply["data"] = {"requestMethod": method, "result": {"inputId": source_id, "state": "accepted"}}
                                for event, fields in [("respCreated", {"inputId": source_id}),
                                    ("respTextDone", {"contentIndex": 0, "text": "公开离线附件回复"}),
                                    ("respDone", {"inputId": source_id, "status": "completed", "contentCount": 1, "audioState": "none"})]:
                                    sequence += 1
                                    messages.append((True, {"msgToken": "content", "method": event, "sessionId": session,
                                        "messageSeq": sequence, "data": dict(respId="response-one", **fields)}))
                            elif method == "sessionEnd":
                                reply.update(method="sessionEnded", data={"state": "ended", "reason": "userStopped"})
                            else:
                                failures.put(True)
                                return
                            for message_route, body in [(method == "inputSend", reply)] + messages:
                                topic = ("$thing/down/message/" if message_route else "$thing/down/signaling/") + "test/text"
                                encoded = topic.encode()
                                send_packet(stream, 0x30, struct.pack("!H", len(encoded)) + encoded + json.dumps(body).encode())
            except EOFError:
                return
            except Exception:
                if not stopped.is_set():
                    failures.put(True)

        with tempfile.TemporaryDirectory(prefix="content-entry-", dir=self.binary.parent) as temporary:
            directory = pathlib.Path(temporary)
            original = (b"public-offline-file-content" if kind == "file" else base64.b64decode(
                "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+jWioAAAAASUVORK5CYII="))
            mime = "text/plain" if kind == "file" else "image/png"
            source = directory / ("note.txt" if kind == "file" else "picture.png")
            source.write_bytes(original)
            certificate, key = directory / "ca.pem", directory / "key.pem"
            subprocess.run([shutil.which("openssl"), "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                "-keyout", str(key), "-out", str(certificate), "-days", "1", "-subj", "/CN=localhost",
                "-addext", "subjectAltName=DNS:localhost"], check=True, timeout=15,
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            os.chmod(key, 0o600)
            tls = Server(("127.0.0.1", 0), MetadataEndpoint)
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            context.minimum_version = ssl.TLSVersion.TLSv1_2
            context.load_cert_chain(certificate, key)
            tls.socket = context.wrap_socket(tls.socket, server_side=True)
            tls_thread = threading.Thread(target=tls.serve_forever, daemon=False)
            mqtt_thread = threading.Thread(target=mqtt, daemon=False)
            try:
                tls_thread.start()
                mqtt_thread.start()
                environment = {k: v for k, v in os.environ.items() if not k.startswith("UR_")}
                environment.update(UR_DEVICE_PRODUCT="test", UR_DEVICE_NAME="text", UR_DEVICE_SECRET=base64.b64encode(secret).decode(),
                    UR_MQTT_HOST="127.0.0.1", UR_MQTT_PORT=str(listener.getsockname()[1]))
                if configured:
                    environment.update(UR_DEVICE_CONTENT_ORIGIN=f"https://localhost:{tls.server_port}", UR_DEVICE_CONTENT_CA=str(certificate))
                process = subprocess.Popen([str(self.binary)], env=environment, stdin=subprocess.PIPE,
                    stdout=subprocess.PIPE, stderr=subprocess.PIPE)

                def collect():
                    """真实pipe持续消费固定诊断，不因进程未退出猜测完成。"""
                    for line in process.stdout:
                        lines.put(line)

                reader = threading.Thread(target=collect, daemon=False)
                reader.start()
                self.assertTrue(json.loads(lines.get(timeout=5)) == {"sessionEstablished": True, "transport": "none"})
                process.stdin.write(("/" + kind + " " + str(source) + "\n").encode())
                process.stdin.flush()
                if action:
                    self.assertTrue(put_entered.wait(5))
                    process.stdin.write(("/" + action + "\n").encode())
                    process.stdin.flush()
                    if action == "cancel":
                        self.assertTrue(json.loads(lines.get(timeout=5)) == {"attachmentCancelRequested": True})
                    else:
                        # 原sessionEnded仍须从实际协议ACK取得，不能用进程退出当协议释放。
                        self.assertTrue(json.loads(lines.get(timeout=5)) == {"sessionEnded": True, "completedRounds": 0})
                    release_put.set()
                process.stdin.close()
                if not selected or not configured:
                    observed = [json.loads(lines.get(timeout=5)) for _ in range(2)]
                    self.assertTrue(observed == [{"attachmentRejected": True}, {"sessionEnded": True, "completedRounds": 0}])
                elif not action:
                    observed = [json.loads(lines.get(timeout=5)) for _ in range(3)]
                    self.assertTrue(observed == [{"inputAccepted": True, "round": 1}, {"replyCompleted": True, "round": 1},
                                               {"sessionEnded": True, "completedRounds": 1}])
                elif action == "cancel":
                    observed = [json.loads(lines.get(timeout=5)) for _ in range(2)]
                    self.assertTrue(observed == [{"attachmentRejected": True}, {"sessionEnded": True, "completedRounds": 0}])
                self.assertTrue(process.wait(timeout=5) == 0)
                self.assertTrue(process.stderr.read() == b"")
                self.assertTrue(len(inputs) == (0 if action or not selected or not configured else 1))
                expected_stages = [] if not selected or not configured else (["prepare", "put"] if action else ["prepare", "put", "complete"])
                self.assertTrue(stages == expected_stages)
                self.assertTrue(failures.empty())
            finally:
                release_put.set()
                stopped.set()
                listener.close()
                if process:
                    if process.poll() is None:
                        process.terminate()
                        try:
                            process.wait(timeout=3)
                        except subprocess.TimeoutExpired:
                            process.kill()
                            process.wait(timeout=3)
                    for pipe in (process.stdin, process.stdout, process.stderr):
                        if pipe and not pipe.closed:
                            pipe.close()
                for stream in connections:
                    try:
                        stream.shutdown(socket.SHUT_RDWR)
                    except OSError:
                        pass
                    stream.close()
                tls.shutdown()
                tls.server_close()
                for thread in (reader, mqtt_thread, tls_thread):
                    if thread:
                        thread.join(timeout=3)
                        self.assertFalse(thread.is_alive())

    def test_file_actual_host_three_phases_then_typed_reply(self):
        """真实/file stdin→原件TLS三步→typed输入受理→同回复Done→协议结束。"""
        self.exercise("file")

    def test_image_actual_host_three_phases_then_typed_reply(self):
        """图片路径走相同实际宿主与平台交集，不从本地格式偷偷改为文字。"""
        self.exercise("image")

    def test_upload_cancel_has_no_complete_or_late_input(self):
        """上传中/cancel撤销atomic许可，PUT迟到成功不能提交引用。"""
        self.exercise("file", "cancel")

    def test_upload_quit_has_no_complete_or_late_input(self):
        """上传中/quit仍取得原结束ACK，工作线程退出不产生迟到inputSend。"""
        self.exercise("file", "quit")

    def test_platform_text_only_rejects_before_any_http(self):
        """平台最终交集只有text时，不借本地声明上传或提交图片引用。"""
        self.exercise("image", selected=False)

    def test_missing_origin_declares_only_text_and_rejects_command(self):
        """未配置可信origin时真实capability只声明text；/file不降级成文字。"""
        self.exercise("file", configured=False)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("需要本次device_text产物")
    ContentTextEntryTest.binary = pathlib.Path(sys.argv.pop()).resolve(strict=True)
    unittest.main()
