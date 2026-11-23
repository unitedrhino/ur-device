#!/usr/bin/env python3
"""启动本任务独占的回环 Broker，执行真实 C++ 传输测试并可靠清理子进程。"""
import os
import pathlib
import shutil
import socket
import subprocess
import sys
import threading
import time


def main():
    """唯一参数为测试二进制；不连接共享 Broker 或读取生产凭据。"""
    if len(sys.argv) != 2:
        raise SystemExit("用法：run_broker_test.py <desktop_transport_tests>")
    binary = pathlib.Path(sys.argv[1]).resolve(strict=True)
    broker = shutil.which("mosquitto")
    if not broker:
        raise SystemExit("缺少 mosquitto，真实 Broker 验证未执行")
    with socket.socket() as lease:
        lease.bind(("127.0.0.1", 0))
        port = lease.getsockname()[1]
    process = None  # 建立清理范围后才启动进程，初始化失败也不能遗留 Broker。
    silent = socket.socket()  # 只握TCP不回CONNACK，验证原生异步连接截止和关闭。
    stop = threading.Event()  # 仅控制本任务的无响应端点线程。
    def accept_silent():
        """接纳测试连接但不响应MQTT，退出时关闭所有本线程拥有的连接。"""
        while not stop.is_set():
            try:
                connection, _ = silent.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            with connection:
                stop.wait()
    silent_thread = threading.Thread(target=accept_silent, daemon=False)
    try:
        silent.bind(("127.0.0.1", 0))
        silent.listen(1)
        silent.settimeout(0.1)
        silent_thread.start()
        process = subprocess.Popen([broker, "-p", str(port)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if process.poll() is not None:
                raise SystemExit("隔离 Broker 启动失败，不连接可能占用端口的其他进程")
            try:
                with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                    break
            except OSError:
                time.sleep(0.02)
        else:
            raise SystemExit("隔离 Broker 就绪超时")
        environment = os.environ.copy()
        environment["UR_TEST_MQTT_PORT"] = str(port)
        environment["UR_TEST_SILENT_MQTT_PORT"] = str(silent.getsockname()[1])
        result = subprocess.run([str(binary)], env=environment, timeout=45, check=False)
        raise SystemExit(result.returncode)
    finally:
        stop.set()
        silent.close()
        if silent_thread.ident is not None:
            silent_thread.join(timeout=3)
        if process is not None and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=3)
        if silent_thread.is_alive():
            raise RuntimeError("无响应测试端点线程没有退出")


if __name__ == "__main__":
    main()
