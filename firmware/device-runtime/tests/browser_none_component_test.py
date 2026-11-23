#!/usr/bin/env python3
"""正式C++无媒体宿主＋真实Chromium＋合成MQTT对端组件验收；不是正式后端／模型E2E。"""
import base64
import json
import os
import pathlib
import queue
import re
import socket
import struct
import subprocess
import sys
import threading
import time
import unittest

sys.dont_write_bytecode = True
from text_entry_test import packet, send_packet


class BrowserNoneComponentTest(unittest.TestCase):
    """实际双轮绘制／唯一提交及取消后迟到执行隔离，失败也精确回收自身资源。"""
    binary = None  # 必须由调用者显式指定当前构建的device_browser，不能借旧二进制假绿。

    def test_conflicting_native_media_configuration_rejected_before_network(self):
        """直接执行原宿主配置校验；失败原因必须明确，不能借缺身份造成偶然拒绝。"""
        base = {key: value for key, value in os.environ.items() if not key.startswith("UR_")}
        cases = [({"UR_OUTPUT_FOCUS": value}, "invalid_output_focus") for value in ["voice", "media", "live"]]
        cases.append(({"UR_AI_VISION_CAMERA": '{"cameraId":"front","videoTrack":"vision-1","deviceId":"synthetic"}'},
                      "invalid_vision_configuration"))
        for settings, reason in cases:
            with self.subTest(reason=reason):
                result = subprocess.run([str(self.binary), "39871"],
                    env={**base, "UR_MEDIA_TRANSPORT": "none", **settings},
                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=5, check=False)
                self.assertEqual(result.returncode, 2)
                self.assertEqual(result.stdout, b"")
                self.assertEqual(result.stderr.decode().strip(), reason)

    def test_actual_display_and_late_execute_without_media(self):
        """同一原会话完成深／浅两次真实绘制，再取消第三轮并隔离迟到Execute。"""
        listener = socket.socket()
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        listener.settimeout(0.25)  # 浏览器启动失败时允许accept线程及时观察本用例关闭，不遗留15秒等待。
        stopped = threading.Event()
        failures, lines, host_failures = queue.Queue(), queue.Queue(), queue.Queue()
        connections, submits = [], []  # 仅保存本合成对端；不解析或输出CONNECT认证正文。
        session = "none-browser-component"
        environment = {key: value for key, value in os.environ.items() if not key.startswith("UR_")}
        environment.update(UR_MEDIA_TRANSPORT="none", UR_DEVICE_PRODUCT="test", UR_DEVICE_NAME="text",
            UR_DEVICE_MESSAGE_DIAGNOSTICS="1",
            UR_DEVICE_SECRET=base64.b64encode(b"public-offline-test-key").decode(),
            UR_MQTT_HOST="127.0.0.1", UR_MQTT_PORT=str(listener.getsockname()[1]))
        browser_session = "task143-none-component-" + str(os.getpid())

        def browser(*args):
            """只操作本用例具名浏览器，命令失败不回显可能包含本地令牌的自由错误。"""
            result = subprocess.run(["agent-browser", "--session", browser_session, *args],
                env={**environment, "AGENT_BROWSER_ARGS": "--no-sandbox"},
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=25, check=False)
            self.assertEqual(result.returncode, 0, "owned_browser_command_failed")
            return result.stdout.decode().strip()

        def evaluate(script):
            """仅JSON读回固定计数和实际DOM，不通过eval改变主题、协议或页面能力。"""
            value = json.loads(browser("eval", script))
            return json.loads(value) if isinstance(value, str) else value

        def serve():
            """公开合成信封经原生MQTT适配；原token／两通道水位及续答ACK必须逐项关联。"""
            phase = "accept"  # 仅固定本夹具阶段，错误不回显认证或报文正文。
            round_number = 0  # accept本身失败也有固定计数，不能让诊断异常覆盖原失败。
            try:
                while not stopped.is_set():
                    try:
                        stream, _ = listener.accept()
                        break
                    except socket.timeout:
                        continue
                else:
                    return
                connections.append(stream)
                stream.settimeout(30)
                sequence = 0
                seen_inputs = set()  # 仅指纹判断原token重投，失败诊断不输出身份／正文。
                current_input = current_response = ""

                def emit(method, data, request=None, message=False, code=None):
                    """合成对端只模拟平台响应；不借实际网页观察来替设备提交工具结果。"""
                    nonlocal sequence
                    body = {"msgToken": request["msgToken"] if request else "event", "method": method,
                            "sessionId": session, "data": data}
                    if request and method == "requestAck" and "sessionId" not in request:
                        del body["sessionId"]  # 能力ACK仍是连接级，不能借夹具预授会话身份。
                    if code is not None:
                        body["code"] = code
                    elif message:
                        sequence += 1
                        body["messageSeq"] = sequence
                    else:
                        body["eventSeq"] = 1
                    topic = ("$thing/down/message/" if message else "$thing/down/signaling/") + "test/text"
                    encoded = topic.encode()
                    send_packet(stream, 0x30, struct.pack("!H", len(encoded)) + encoded +
                                json.dumps(body, ensure_ascii=False, separators=(",", ":")).encode())

                def execute():
                    """只有审核固定主题工具，不允许脚本／任意参数或模拟客户端显示完成。"""
                    emit("toolCallExecute", {"respId": current_response, "toolCallId": "call-" + str(round_number),
                        "name": "set_local_display_theme", "arguments": {"theme": "dark" if round_number != 2 else "light"},
                        "isFrontend": True, "interactionType": "action", "executionToken": "synthetic-execution",
                        "expiresAt": int(time.time() * 1000) + 10000}, message=True)

                with stream:
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
                            method, params = request["method"], request.get("params", {})
                            phase = method if method in {"capabilityReport", "sessionCreate", "inputSend", "toolCallSubmit", "respCancel", "sessionEnd"} else "unexpectedMethod"
                            if method == "capabilityReport":
                                self.assertEqual(params["transports"], ["none"])
                                self.assertNotIn("audioParams", params)
                                self.assertEqual(len(params["frontendTools"]), 1)
                                emit("requestAck", {"requestMethod": method, "result": {"acceptedRevision": 1}}, request, code=200)
                            elif method == "sessionCreate":
                                self.assertEqual(params["tracks"], [])
                                self.assertNotIn("interactionMode", params)
                                self.assertNotIn("audioParams", params)
                                emit("sessionCreated", {"state": "negotiating", "selected": {"transport": "none",
                                    "inputModalities": ["text"], "outputModalities": ["text"]}, "tracks": []}, request, code=200)
                                emit("sessionEstablished", {"state": "established", "mediaRevision": 0})
                            elif method == "inputSend":
                                phase = "inputSend_duplicate" if request["msgToken"] in seen_inputs else "inputSend_new"
                                seen_inputs.add(request["msgToken"])
                                round_number += 1
                                self.assertLessEqual(round_number, 3)
                                phase = "inputSend_modalities"
                                self.assertEqual(params["outputModalities"], ["text"])
                                phase = "inputSend_emit"
                                current_input, current_response = params["inputId"], "reply-" + str(round_number)
                                emit("requestAck", {"requestMethod": method, "result": {"inputId": current_input,
                                    "state": "accepted"}}, request, message=True, code=200)
                                emit("respCreated", {"respId": current_response, "inputId": current_input}, message=True)
                                emit("toolCallStart", {"respId": current_response, "toolCallId": "call-" + str(round_number),
                                    "name": "set_local_display_theme", "arguments": {}}, message=True)
                                if round_number < 3:
                                    execute()
                            elif method == "toolCallSubmit":
                                self.assertLess(round_number, 3, "cancelled_tool_must_not_submit")
                                self.assertEqual(params["respId"], current_response)
                                self.assertEqual(params["toolCallId"], "call-" + str(round_number))
                                self.assertEqual(params["status"], "succeeded")
                                self.assertNotIn(current_response, submits, "duplicate_local_effect")
                                submits.append(current_response)
                                emit("requestAck", {"requestMethod": method, "result": {"respId": current_response, "toolCallId": params["toolCallId"],
                                    "state": "accepted", "continuation": "accepted"}}, request, message=True, code=200)
                                emit("toolCallResult", {"respId": current_response, "toolCallId": params["toolCallId"],
                                    "state": "succeeded", "result": {}}, message=True)
                                emit("respTextDelta", {"respId": current_response, "contentIndex": 0, "delta": "主题已确认"}, message=True)
                                emit("respTextDone", {"respId": current_response, "contentIndex": 0, "text": "主题已确认"}, message=True)
                                emit("respDone", {"respId": current_response, "inputId": current_input,
                                    "status": "completed", "contentCount": 1, "audioState": "none"}, message=True)
                            elif method == "respCancel":
                                self.assertEqual(round_number, 3)
                                self.assertEqual(params["respId"], current_response)
                                emit("requestAck", {"requestMethod": method, "result": {"respId": current_response,
                                    "state": "cancelled"}}, request, code=200)
                                execute()  # 已有合法Start，取消ACK后即不能执行，不借缺Start或Done才偶然拒绝。
                                emit("respDone", {"respId": current_response, "inputId": current_input,
                                    "status": "cancelled", "reason": "userCancelled", "contentCount": 0, "audioState": "none"}, message=True)
                                execute()  # 同一原回复终态后的迟到请求，不能复活副作用或提交。
                            elif method == "sessionEnd":
                                emit("sessionEnded", {"state": "ended", "reason": "userStopped"}, request, code=200)
                            else:
                                raise AssertionError("unexpected_media_or_control_request")
            except EOFError:
                return
            except Exception:
                if not stopped.is_set():
                    failures.put({"phase": phase, "round": round_number})

        process = reader = stderr_reader = None
        server = threading.Thread(target=serve, daemon=False)
        try:
            server.start()
            with socket.socket() as probe:
                probe.bind(("127.0.0.1", 0))
                browser_port = probe.getsockname()[1]  # 原宿主只接受显式非特权端口；不修改生产参数校验。
            process = subprocess.Popen([str(self.binary), str(browser_port)], env=environment,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE)

            def collect():
                """真实stdout配对地址仅存内存，不归档本地令牌或自由正文。"""
                for line in process.stdout:
                    text = line.decode().strip()
                    if text.startswith("host_failure="):
                        value = json.loads(text[len("host_failure="):])
                        known = {"mqttPoll", "mqttDisconnected", "messageToolScope", "messageContent", "messageAck", "signalClientInvalid", "clientTick", "toolPoll"}
                        if value.get("runtime") in known:
                            host_failures.put({"runtime": value["runtime"]})
                    else:
                        lines.put(text)
            reader = threading.Thread(target=collect, daemon=False)
            reader.start()
            def collect_failure():
                """只保留原宿主固定失败枚举，不归档自由错误、身份或网络报文。"""
                for line in process.stderr:
                    text = line.decode().strip()
                    if re.fullmatch(r"runtime_failed[ a-z_0-9=]+", text):
                        host_failures.put(text)
            stderr_reader = threading.Thread(target=collect_failure, daemon=False)
            stderr_reader.start()
            url = lines.get(timeout=10)
            self.assertTrue(url.startswith("http://127.0.0.1:"))
            browser("open", url)
            browser("snapshot", "-i")
            browser("click", "#activate")
            try:
                browser("wait", "--fn", '!document.getElementById("sendText").disabled')
            except Exception:
                diagnostic = evaluate("JSON.stringify({textOnly:textOnlyDevice,terminated,"
                    "mediaAbsent:typeof context==='undefined'&&typeof stream==='undefined'&&typeof rtcPeer==='undefined',"
                    "themeReady:!!clientTheme&&!clientTheme.closed})")
                self.fail(json.dumps({"fixtureFailureStages": list(failures.queue), "deviceExit": process.poll(), **diagnostic}))
            for index, theme in enumerate(["dark", "light"], 1):
                browser("fill", "#textInput", "公开合成主题输入")
                browser("click", "#sendText")
                browser("wait", "--fn", "metrics.completedResponses === " + str(index))
                actual = evaluate("JSON.stringify({theme:document.body.dataset.clientTheme,background:getComputedStyle(document.body).backgroundColor,"
                    "records:clientTheme.records.size,receipts:metrics.clientToolReceiptIndex,"
                    "mediaAbsent:typeof context==='undefined'&&typeof stream==='undefined'&&typeof rtcPeer==='undefined',"
                    "audioDisabled:document.getElementById('textAudio').disabled})")
                self.assertEqual(actual, {"theme": theme, "background": "rgb(15, 23, 42)" if index == 1 else "rgb(248, 250, 252)",
                    "records": index, "receipts": index, "mediaAbsent": True, "audioDisabled": True})
            browser("fill", "#textInput", "公开合成取消输入")
            browser("click", "#sendText")
            browser("wait", "--fn", 'controlResponseId === "reply-3"')
            self.assertEqual(evaluate("JSON.stringify({third:controlResponseId==='reply-3',disabled:element('cancel').disabled})"),
                             {"third": True, "disabled": False}, "original_cancel_target_unavailable")
            evaluate("(() => {globalThis.noneCancelObserved={called:0,sent:0,clicked:0};"
                "element('cancel').addEventListener('click',()=>++noneCancelObserved.clicked);const originalSend=send;"
                "send=function(value){const result=originalSend.apply(this,arguments);if(value?.kind==='cancel'){"
                "++noneCancelObserved.called;if(result===true)++noneCancelObserved.sent;}return result;};return JSON.stringify(true);})()")
            browser("scrollintoview", "#cancel")  # 原生浏览器CLI不保证滚动到视口，禁止离屏点击被误计为取消。
            cancel_refs = re.findall(r'button "打断当前回复" \[ref=(e[0-9]+)\]', browser("snapshot", "-i"))
            self.assertEqual(len(cancel_refs), 1, "actual_cancel_button_missing")
            browser("click", "@" + cancel_refs[0])
            self.assertEqual(evaluate("JSON.stringify(noneCancelObserved)"), {"called": 1, "sent": 1, "clicked": 1},
                             "original_browser_cancel_not_sent")
            try:
                browser("wait", "--fn", 'metrics.cancelledResponses === 1 && controlResponseId === ""')
            except Exception:
                diagnostic = evaluate("JSON.stringify({cancelled:metrics.cancelledResponses,"
                    "controlStillThird:controlResponseId==='reply-3',terminated,records:clientTheme.records.size})")
                self.fail(json.dumps({"fixtureFailureStages": list(failures.queue),
                    "hostFailures": list(host_failures.queue), "deviceExit": process.poll(), **diagnostic}))
            browser("click", "#end")
            self.assertEqual(process.wait(timeout=10), 0)
            self.assertEqual(submits, ["reply-1", "reply-2"])
            self.assertTrue(failures.empty(), "mqtt_fixture_failed")
            final = evaluate("JSON.stringify({theme:document.body.dataset.clientTheme,records:clientTheme.records.size,"
                "completed:metrics.completedResponses,cancelled:metrics.cancelledResponses,terminated})")
            self.assertEqual(final, {"theme": "light", "records": 2, "completed": 2, "cancelled": 1, "terminated": True})
        finally:
            stopped.set()
            browser_closed = False  # 浏览器清理失败不能跳过原C++／socket／线程的收尾。
            try:
                browser("close")
                browser_closed = True
            except Exception:
                pass
            if process:
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=3)
                process.stdout.close()
                process.stderr.close()
            listener.close()
            for stream in connections:
                try:
                    stream.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass
                stream.close()
            server.join(timeout=3)
            if reader:
                reader.join(timeout=3)
            if stderr_reader:
                stderr_reader.join(timeout=3)
            self.assertFalse(server.is_alive())
            self.assertFalse(reader and reader.is_alive())
            self.assertFalse(stderr_reader and stderr_reader.is_alive())
            self.assertTrue(browser_closed, "owned_browser_cleanup_failed")


if __name__ == "__main__":
    BrowserNoneComponentTest.binary = pathlib.Path(sys.argv.pop(1)).resolve(strict=True)
    unittest.main()
