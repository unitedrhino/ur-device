#!/usr/bin/env python3
"""Linux BlueZ 真机换网验收：先核对身份，再可选写入凭据；不发送绑定 Token。

默认仅探测，--provision-stdin 才从标准输入读取 JSON 凭据。失败不会自动重发密码，
也不会以断开或取得 IP 当作成功；串口、云端身份对照及手机验收需另行记录。
"""
import argparse
from collections import deque
from contextlib import contextmanager
import json
import ipaddress
from pathlib import Path
import re
import sys
import time


class ProbeError(RuntimeError):
    """ProbeError 仅携带固定诊断标签，不包含原始报文或访问凭据。"""


def verify_boot_log(text, ssid, product, device):
    """只核验最近启动的实际网络与平台恢复，不将 BLE 临时连接或历史在线当成成功。"""
    boundary = text.rfind("ESP-ROM:")
    if boundary < 0:
        raise ProbeError("boot_boundary_missing")
    current = text[boundary:]
    if any(marker in current for marker in ("Guru Meditation", "Backtrace", "CORRUPT HEAP", "stack overflow")):
        raise ProbeError("boot_fatal_error")
    connections = list(re.finditer(r"WifiBoard: Connected to WiFi: ([^\r\n]+)", current))
    if not connections or connections[-1].group(1) != ssid:
        raise ProbeError("boot_ssid_mismatch")
    connected = connections[-1]
    addresses = re.findall(r"WifiStation: Got IP: ([0-9.]+)", current[:connected.start()])
    if not addresses:
        raise ProbeError("boot_ip_missing")
    try:
        address = ipaddress.IPv4Address(addresses[-1])
    except ValueError:
        raise ProbeError("boot_ip_invalid") from None
    if address.is_unspecified or address.is_loopback or address.is_link_local:
        raise ProbeError("boot_ip_invalid")
    following = current[connected.end():]
    if "WifiBoard: WiFi disconnected" in following:
        raise ProbeError("boot_wifi_disconnected")
    transport = f"UnitedRhino MQTT transport connected, pid={product} dev={device}"
    matched = re.search(re.escape(transport) + r"(?=\r?\n|$)", following)
    if matched is None:
        raise ProbeError("boot_mqtt_missing")
    following = following[matched.end():]
    subscribed = following.find("UnitedRhino MQTT subscriptions ready")
    if subscribed < 0 or "received downlink method=reportReply" not in following[subscribed:]:
        raise ProbeError("boot_report_missing")
    return {"passed": True, "bootNetworkVerified": True, "mqttReportVerified": True}


class Decoder:
    """Decoder 严格重组单条通知，限制长度及分片时限，错误终止本次测试。"""

    def __init__(self):
        self.pending = None
        self.started = 0.0

    def push(self, raw, now):
        """push 返回完整 (类型, 正文)，中间片返回 None，非法包抛出固定错误。"""
        if len(raw) < 3 or ((raw[1] & 63) << 8 | raw[2]) != len(raw) - 3:
            raise ProbeError("invalid_notification_length")
        kind, flag, body = raw[0], raw[1] >> 6, raw[3:]
        if flag == 0:
            self.pending = None
        elif flag == 1:
            self.pending = (kind, body)
            self.started = now
        else:
            if not self.pending or self.pending[0] != kind or now - self.started > 10:
                self.pending = None
                raise ProbeError("invalid_fragment_sequence")
            body = self.pending[1] + body
            self.pending = (kind, body) if flag == 2 else None
        if len(body) > 4096:
            self.pending = None
            raise ProbeError("notification_too_large")
        return (kind, body) if flag in (0, 3) else None


def advertisement_matches(payload, product, mac):
    """advertisement_matches 读取 BlueZ 去掉 Company ID 后的 17 字节配网正文。"""
    return (len(payload) == 17 and payload[0] == 2 and
            payload[1:7].hex() == mac.replace(":", "").lower() and
            payload[7:].rstrip(b"\0") == product.encode())


def credentials(data):
    """credentials 校验 UTF-8 字节边界；返回内存字节串，禁止打印输入对象。"""
    if not isinstance(data, dict) or not isinstance(data.get("ssid"), str) or not isinstance(data.get("password"), str):
        raise ProbeError("invalid_credentials")
    ssid, password = data["ssid"].encode(), data["password"].encode()
    if not 0 < len(ssid) <= 32 or len(password) > 63 or b"\0" in ssid + password:
        raise ProbeError("invalid_credentials")
    return ssid, password


def capability(body):
    """capability 只接受本地换网 v2 的完整合法状态，不接受云端确认旧版本。"""
    if len(body) != 12 or body[:2] != b"\x02\x01" or body[2] > 9 or body[3] > 6:
        raise ProbeError("invalid_capability")
    return body[2], body[3], body[4:]


def run_sequence(link, device_name, config=None, result_timeout=190):
    """run_sequence 执行真实 E0/E1/E2/E3；link 只抽象 BLE 收发，不抽象协议结果。"""
    identity = request = None
    def identified(kind, body):
        nonlocal identity, request
        if kind == 0x08:
            if len(body) < 4 or body[0] != 2 or body[3] != len(body) - 4 or body[4:] != device_name.encode():
                raise ProbeError("device_identity_mismatch")
            identity = True
        elif kind == 0xE5:
            phase, error, token = capability(body)
            if phase != 2 or error:
                raise ProbeError("device_not_waiting_for_credentials")
            if request is not None and request != token:
                raise ProbeError("request_changed")
            request = token
        return identity and request is not None
    link.exchange(b"\xe0", identified, 10)
    result = {"identityVerified": True, "requestId": request.hex(), "credentialsSent": False}
    if config is None:
        return result
    ssid, password = credentials(config)
    def receipt(expected):
        def accepted(kind, body):
            if kind == 0xE5:
                _, error, token = capability(body)
                if error or token != request:
                    raise ProbeError("request_changed_or_failed")
            if kind != expected:
                return False
            if body != b"\0":
                raise ProbeError("credentials_or_mode_rejected")
            return True
        return accepted
    link.exchange(b"\xe1\x01", receipt(0xE0), 10)
    size = len(ssid) + len(password)
    command = bytes([0xE2, size >> 8, size & 255, len(ssid)]) + ssid + bytes([len(password)]) + password
    link.exchange(command, receipt(0xE1), 10)
    del command, password, config
    committed = connected = False
    def completed(kind, body):
        nonlocal committed, connected
        if kind == 0xE5:
            phase, error, token = capability(body)
            if token != request or error or phase == 8:
                raise ProbeError("request_changed_or_failed")
            committed = committed or phase == 7
        elif kind == 0xE2:
            if len(body) < 4 or body[0] != 1 or len(body) != 4 + body[3]:
                raise ProbeError("invalid_wifi_result")
            if body[1] == 0:
                if body[4:] != ssid:
                    raise ProbeError("connected_ssid_mismatch")
                connected = True
        return committed and connected
    link.exchange(b"\xe3", completed, result_timeout)
    return dict(result, credentialsSent=True, committed=True, connected=True)


@contextmanager
def bluez_link(product, mac, adapter_name):
    """bluez_link 独占本次通知及连接；只连接明确目标，清理不重置适配器或其他设备。"""
    import dbus
    from dbus.mainloop.glib import DBusGMainLoop
    from gi.repository import GLib
    DBusGMainLoop(set_as_default=True)
    bus = dbus.SystemBus()
    context = GLib.MainContext.default()
    manager = dbus.Interface(bus.get_object("org.bluez", "/"), "org.freedesktop.DBus.ObjectManager")
    adapter_path = "/org/bluez/" + adapter_name
    adapter = dbus.Interface(bus.get_object("org.bluez", adapter_path), "org.bluez.Adapter1")
    started = False
    device = notify = match = None
    frames = deque()
    decoder = Decoder()
    failure = None
    def pump():
        while context.pending():
            context.iteration(False)
        if failure:
            raise failure
        time.sleep(0.02)
    def notified(interface, values, invalidated):
        nonlocal failure
        if interface != "org.bluez.GattCharacteristic1" or "Value" not in values:
            return
        try:
            frame = decoder.push(bytes(values["Value"]), time.monotonic())
            if frame:
                if len(frames) >= 256:
                    raise ProbeError("notification_queue_overflow")
                frames.append(frame)
        except ProbeError as error:
            failure = error  # D-Bus 回调异常不能只写 stderr 后被静默吞掉。
    try:
        adapter.SetDiscoveryFilter({"Transport": dbus.String("le")})
        adapter.StartDiscovery()
        started = True
        deadline = time.monotonic() + 30
        target = None
        while time.monotonic() < deadline and target is None:
            for path, interfaces in manager.GetManagedObjects().items():
                info = interfaces.get("org.bluez.Device1", {})
                if str(path).startswith(adapter_path + "/") and advertisement_matches(
                        bytes(info.get("ManufacturerData", {}).get(0xFEE7, [])), product, mac):
                    if info.get("Connected"):
                        raise ProbeError("target_already_connected")
                    target = str(path)
                    break
            pump()
        if target is None:
            raise ProbeError("target_not_found")
        device = dbus.Interface(bus.get_object("org.bluez", target), "org.bluez.Device1")
        device.Connect(timeout=30)
        properties = dbus.Interface(bus.get_object("org.bluez", target), "org.freedesktop.DBus.Properties")
        deadline = time.monotonic() + 20
        while not properties.Get("org.bluez.Device1", "ServicesResolved"):
            if time.monotonic() >= deadline:
                raise ProbeError("service_discovery_timeout")
            pump()
        chars = {}
        for path, interfaces in manager.GetManagedObjects().items():
            if str(path).startswith(target + "/"):
                info = interfaces.get("org.bluez.GattCharacteristic1", {})
                chars[str(info.get("UUID", "")).lower()] = str(path)
        write = dbus.Interface(bus.get_object("org.bluez", chars["0000ffe1-65d0-4e20-b56a-e493541ba4e2"]), "org.bluez.GattCharacteristic1")
        notify_path = chars["0000ffe3-65d0-4e20-b56a-e493541ba4e2"]
        match = bus.add_signal_receiver(notified, signal_name="PropertiesChanged",
                                       dbus_interface="org.freedesktop.DBus.Properties", path=notify_path)
        notify = dbus.Interface(bus.get_object("org.bluez", notify_path), "org.bluez.GattCharacteristic1")
        notify.StartNotify()
        class Link:
            """Link 保留真实 GATT 写入和通知顺序，每条命令仅发送一次。"""
            def exchange(self, command, accept, timeout):
                pump()
                frames.clear()  # 不接受前一指令已经收到的旧回执。
                write.WriteValue(dbus.Array([dbus.Byte(value) for value in command], signature="y"),
                                 {"type": dbus.String("request")})
                deadline = time.monotonic() + timeout
                while time.monotonic() < deadline:
                    pump()
                    while frames:
                        if accept(*frames.popleft()):
                            return
                    if not properties.Get("org.bluez.Device1", "Connected"):
                        raise ProbeError("disconnected_before_confirmation")
                raise ProbeError("notification_timeout")
        yield Link()
    finally:
        for resource, method in ((notify, "StopNotify"), (match, "remove"),
                                 (device, "Disconnect"), (adapter if started else None, "StopDiscovery")):
            if resource is not None:
                try:
                    getattr(resource, method)()
                except dbus.DBusException:
                    pass  # 不覆盖主失败；不再连接另一设备或重发凭据。


def main():
    """main 默认只读；目标显式传入，凭据只经 stdin，错误输出不带原始异常正文。"""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--product-id", required=True)
    parser.add_argument("--device-name", required=True)
    parser.add_argument("--mac", required=True)
    parser.add_argument("--adapter", default="hci0")
    parser.add_argument("--provision-stdin", action="store_true")
    parser.add_argument("--verify-boot-log", type=Path, help="只读核验本次串口日志，不操作蓝牙")
    parser.add_argument("--expected-ssid", help="重启后必须实际连接的网络名；不输出该值")
    args = parser.parse_args()
    try:
        if (not re.fullmatch(r"(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}", args.mac) or
                not re.fullmatch(r"hci[0-9]+", args.adapter) or not 0 < len(args.product_id.encode()) <= 10):
            raise ProbeError("invalid_target")
        if args.verify_boot_log:
            if args.provision_stdin or not args.expected_ssid:
                raise ProbeError("invalid_verification_arguments")
            print(json.dumps(verify_boot_log(args.verify_boot_log.read_text(errors="replace"),
                                            args.expected_ssid, args.product_id, args.device_name)))
            return 0
        if args.expected_ssid:
            raise ProbeError("boot_log_required")
        config = json.load(sys.stdin) if args.provision_stdin else None
        if args.provision_stdin:
            credentials(config)
        with bluez_link(args.product_id, args.mac, args.adapter) as link:
            result = run_sequence(link, args.device_name, config)
        print(json.dumps(result))
        return 0
    except Exception as error:
        print(json.dumps({"passed": False, "reason": str(error) if isinstance(error, ProbeError)
                          else "transport_or_input_error"}))
        return 1


if __name__ == "__main__":
    sys.exit(main())
