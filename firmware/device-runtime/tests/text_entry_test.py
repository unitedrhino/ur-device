#!/usr/bin/env python3
"""真实device_text管道/原生MQTT适配回归；受控TCP协议对端不是平台、Broker或真实模型。"""
import base64
import json
import os
import pathlib
import queue
import socket
import struct
import subprocess
import sys
import threading
import unittest


def packet(stream):
    """有界读取MQTT包；不记录CONNECT认证内容或自由异常正文。"""
    def exact(length):
        result = b""
        while len(result) < length:
            chunk = stream.recv(length - len(result))
            if not chunk:
                raise EOFError()
            result += chunk
        return result
    header = exact(1)[0]
    remaining, factor = 0, 1
    for _ in range(4):
        part = exact(1)[0]
        remaining += (part & 127) * factor
        if remaining > 65536:
            raise ValueError()
        if part < 128:
            return header, exact(remaining)
        factor *= 128
    raise ValueError()


def send_packet(stream, header, payload):
    """受控对端只构造公开合成MQTT协议事实，不访问生产配置。"""
    size, prefix = len(payload), bytearray([header])
    while True:
        digit, size = size % 128, size // 128
        prefix.append(digit | (128 if size else 0))
        if not size:
            break
    stream.sendall(prefix + payload)


class TextEntryTest(unittest.TestCase):
    """实际子进程无需PTY，建立事件必须在仍在线且未提交stdin时可见。"""
    binary = None  # 由中央CTest传入本次真实构建产物，不能借旧binary假绿。

    def exercise(self, tui, error_scope=None, late=False):
        """本轮错误须及时失败；前轮迟到错误不解锁或结束新轮，资源只归本用例。"""
        failures, lines = queue.Queue(), queue.Queue()
        listener = socket.socket()
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        listener.settimeout(5)
        stopped = threading.Event()
        connection = []  # 归属本用例的唯一对端，收尾可解除线程阻塞。
        input_count = []  # 只统计实际上行次数，不保留用户正文或认证信息。

        def serve():
            """严格逐阶段回应实际请求；不解析/打印CONNECT身份字段。"""
            try:
                stream, _ = listener.accept()
                connection.append(stream)
                # 大于宿主回归5秒判据，不能由测试对端先断网替代本轮scoped错误处理。
                stream.settimeout(15)
                with stream:
                    sequence = 0
                    previous = None  # 上一轮实际上行input及实际下行resp，不猜或伪造迟到关联。
                    while not stopped.is_set():
                        header, data = packet(stream)
                        kind = header >> 4
                        if kind == 1:
                            send_packet(stream, 0x20, b"\0\0")
                        elif kind == 8:
                            send_packet(stream, 0x90, data[:2] + b"\1")
                        elif kind == 12:
                            send_packet(stream, 0xd0, b"")
                        elif kind == 14:
                            return
                        elif kind == 3:
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
                                if request["params"]["transports"] != ["none"]:
                                    raise ValueError()
                                reply["data"] = {"requestMethod": method, "result": {"acceptedRevision": 1}}
                            elif method == "sessionCreate":
                                params = request["params"]
                                if params["tracks"] != [] or "audioParams" in params or "interactionMode" in params:
                                    raise ValueError()
                                reply.update(method="sessionCreated", sessionId="pipe-session",
                                    data={"state": "negotiating", "selected": {"transport": "none",
                                          "inputModalities": ["text"], "outputModalities": ["text"]}, "tracks": []})
                                messages.append((False, {"msgToken": "established", "method": "sessionEstablished",
                                    "sessionId": "pipe-session", "eventSeq": 1,
                                    "data": {"state": "established", "mediaRevision": 0}}))
                            elif method == "inputSend":
                                input_count.append(True)
                                params = request["params"]
                                if params["outputModalities"] != ["text"] or params["contents"][0]["type"] != "text":
                                    raise ValueError()
                                source, response = params["inputId"], "reply-" + str(sequence)
                                reply["data"] = {"requestMethod": method, "result": {"inputId": source, "state": "accepted"}}
                                inject = error_scope is not None and (not late or previous is not None)
                                current_fault = inject and not late
                                fault = None
                                if inject:
                                    target = (source, response) if not late else previous
                                    fault = (False, {"msgToken": "scoped-error", "method": "sessionError",
                                        "sessionId": "pipe-session", "eventSeq": 2, "code": 503,
                                        "data": {"scope": error_scope,
                                            "inputId" if error_scope == "input" else "respId":
                                                target[0] if error_scope == "input" else target[1],
                                            "mediaRevision": 0, "retryable": False, "reason": "modelUnavailable"}})
                                    if error_scope == "input":
                                        messages.append(fault)
                                events = [("respCreated", {"inputId": source}),
                                    ("respTextDelta", {"contentIndex": 0, "delta": "公开管道回复"}),
                                    ("respTextDone", {"contentIndex": 0, "text": "公开管道回复"}),
                                    ("respDone", {"inputId": source, "status": "completed", "contentCount": 1, "audioState": "none"})]
                                if current_fault:
                                    events = [] if error_scope == "input" else events[:1]
                                for event, fields in events:
                                    sequence += 1
                                    messages.append((True, {"msgToken": "content", "method": event,
                                        "sessionId": "pipe-session", "messageSeq": sequence,
                                        "data": dict(respId=response, **fields)}))
                                    if inject and error_scope == "response" and event == "respCreated":
                                        messages.append(fault)
                                previous = (source, response)
                            elif method == "sessionEnd":
                                reply.update(method="sessionEnded", data={"state": "ended", "reason": "userStopped"})
                            else:
                                raise ValueError()
                            for message_route, body in [(method == "inputSend", reply)] + messages:
                                topic = ("$thing/down/message/" if message_route else "$thing/down/signaling/") + "test/text"
                                encoded = topic.encode()
                                send_packet(stream, 0x30, struct.pack("!H", len(encoded)) + encoded +
                                            json.dumps(body, ensure_ascii=False, separators=(",", ":")).encode())
            except EOFError:
                return
            except Exception:
                if not stopped.is_set():
                    failures.put(True)  # 只固定失败布尔，禁止SQL/网络原正文进入诊断。

        server = threading.Thread(target=serve, daemon=False)
        process = reader = None
        try:
            server.start()
            environment = {key: value for key, value in os.environ.items() if not key.startswith("UR_")}
            environment.update(UR_DEVICE_PRODUCT="test", UR_DEVICE_NAME="text",
                UR_DEVICE_SECRET=base64.b64encode(b"public-offline-test-key").decode(),
                UR_MQTT_HOST="127.0.0.1", UR_MQTT_PORT=str(listener.getsockname()[1]))
            process = subprocess.Popen([str(self.binary)] + (["--tui"] if tui else []), env=environment,
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

            def collect():
                """逐行读取真实pipe；进程未退出时不得用communicate替代建立回执。"""
                for line in process.stdout:
                    lines.put(line)
            reader = threading.Thread(target=collect, daemon=False)
            reader.start()
            established = json.loads(lines.get(timeout=5))
            self.assertEqual(established, {"sessionEstablished": True, "transport": "none"})
            self.assertIsNone(process.poll())
            current_fault = error_scope is not None and not late
            process.stdin.write(("公开输入一\n" if current_fault else "公开输入一\n公开输入二\n").encode())
            process.stdin.flush()
            process.stdin.close()
            observed = []
            for _ in range(2 if current_fault else 5):
                observed.append(json.loads(lines.get(timeout=5)))
            self.assertEqual(process.wait(timeout=5), 1 if current_fault else 0)
            expected = [{"inputAccepted": True, "round": 1}, {"error": "responseFailed"}] if current_fault else [
                {"inputAccepted": True, "round": 1}, {"replyCompleted": True, "round": 1},
                {"inputAccepted": True, "round": 2}, {"replyCompleted": True, "round": 2},
                {"sessionEnded": True, "completedRounds": 2}]
            self.assertEqual(observed, expected)
            self.assertEqual(len(input_count), 1 if current_fault else 2)
            stderr = process.stderr.read().decode()
            self.assertEqual(stderr, "公开管道回复\n公开管道回复\n" if tui else "")
            self.assertTrue(failures.empty())
        finally:
            stopped.set()
            listener.close()
            if process is not None:
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=3)
                for pipe in [process.stdin, process.stdout, process.stderr]:
                    if pipe and not pipe.closed:
                        pipe.close()
            for stream in connection:
                try:
                    stream.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass
                stream.close()
            server.join(timeout=3)
            if reader:
                reader.join(timeout=3)
            self.assertFalse(server.is_alive())
            self.assertFalse(reader and reader.is_alive())

    def test_default_pipe_establishment_before_input_and_two_rounds(self):
        """默认stdout纯诊断，无正文，实际建立事件在写stdin之前收到。"""
        self.exercise(False)

    def test_explicit_tui_content_is_only_stderr(self):
        """显式TUI展示真实解码增量；终态全文不重复打印。"""
        self.exercise(True)

    def test_current_nonretryable_input_error_exits_without_reply_terminal(self):
        """原本轮input错误无respDone时，宿主必须及时退出且只提交一次意图。"""
        self.exercise(False, "input")

    def test_current_nonretryable_response_error_exits_without_reply_terminal(self):
        """原本轮resp已绑定后的错误无respDone时，不空等应用120秒期限。"""
        self.exercise(False, "response")

    def test_previous_input_error_cannot_fail_next_round(self):
        """迟到错误使用首轮实际inputId，不允许结束第二轮。"""
        self.exercise(False, "input", True)

    def test_previous_response_error_cannot_fail_next_round(self):
        """迟到错误使用首轮实际respId，不能猜成当前回复或失败。"""
        self.exercise(False, "response", True)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("需要本次device_text构建产物")
    TextEntryTest.binary = pathlib.Path(sys.argv.pop()).resolve(strict=True)
    unittest.main()
