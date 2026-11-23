#!/usr/bin/env python3
"""验证正式IXWebSocket修补入口在冷构建和重复配置时都能生成唯一副本，不访问网络。"""
import pathlib
import subprocess
import sys
import tempfile


def main():
    """接收CMake程序和正式修补脚本路径；两种副本及未知匹配数量的六项断言成功返回0。"""
    if len(sys.argv) != 3:
        raise SystemExit("用法：check_socket_guard.py <cmake> <guard-script>")
    cmake = sys.argv[1]  # 调用CTest所使用的同一CMake程序。
    guard = pathlib.Path(sys.argv[2]).resolve(strict=True)  # 被测正式入口，不复制逻辑。
    original = "            ssize_t ret = _socket->recv((char*) &_readbuf[0], _readbuf.size());\n"  # 固定版源文件匹配点。
    original_server = "Socket::closeSocket(_serverFd);\n" * 7  # 只验证配置匹配，真实FD回归交实际SDK网络测试。
    with tempfile.TemporaryDirectory(prefix="ur-socket-guard-", dir=pathlib.Path.cwd()) as directory:
        root = pathlib.Path(directory)  # 本次测试独占目录，退出时精确回收。
        source = root / "source"  # 最小依赖工程不下载第三方代码。
        dependency = source / "dependency"  # 模拟第三方目标所属的独立CMake目录。
        transport = dependency / "ixwebsocket" / "IXWebSocketTransport.cpp"  # 只作为配置输入，不冒充编译或竞态测试。
        transport.parent.mkdir(parents=True)
        transport.write_text(original, encoding="utf-8")
        server = transport.with_name("IXSocketServer.cpp")  # 第二种只读配置输入，不冒认运行时实现。
        server.write_text(original_server, encoding="utf-8")
        (dependency / "CMakeLists.txt").write_text(
            "add_library(ixwebsocket ixwebsocket/IXWebSocketTransport.cpp ixwebsocket/IXSocketServer.cpp)\n", encoding="utf-8")
        (source / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.22)\nproject(socket_guard LANGUAGES CXX)\n"
            "set(ixwebsocket_SOURCE_DIR \"${CMAKE_CURRENT_SOURCE_DIR}/dependency\")\n"
            f"add_subdirectory(dependency)\ninclude(\"{guard.as_posix()}\")\n", encoding="utf-8")
        build = root / "build"  # 必须从不存在的生成文件开始验证。
        for iteration in range(2):  # 第二次配置不能依靠对原始源文件重复替换。
            result = subprocess.run([cmake, "-S", str(source), "-B", str(build)],
                                    capture_output=True, text=True, check=False, timeout=120)
            if result.returncode:
                raise RuntimeError(f"配置第{iteration + 1}轮失败:\n{result.stdout}\n{result.stderr}")
            patched = (build / "IXWebSocketTransport.cpp").read_text(encoding="utf-8")  # 实际生成副本。
            if patched.count("std::lock_guard<std::mutex> lock(_socketMutex);") != 1:
                raise RuntimeError("生成副本没有且仅有一个接收锁")
            patched_server = (build / "IXSocketServer.cpp").read_text(encoding="utf-8")
            if (patched_server.count("_serverFd = static_cast<socket_t>(-1);") != 7 or
                    patched_server.count("Socket::closeSocket(urRetiredServerFd);") != 7 or
                    "Socket::closeSocket(_serverFd);" in patched_server):
                raise RuntimeError("监听FD副本没有且仅有七处先失效再关闭")
        if transport.read_text(encoding="utf-8") != original:
            raise RuntimeError("修补入口修改了第三方原始源文件")
        if server.read_text(encoding="utf-8") != original_server:
            raise RuntimeError("修补入口修改了第三方原监听源文件")
        for count in (6, 8):  # 升级减少或新增关闭路径都必须失败，不能部分修补后假装通过。
            server.write_text("Socket::closeSocket(_serverFd);\n" * count, encoding="utf-8")
            result = subprocess.run([cmake, "-S", str(source), "-B", str(build)],
                                    capture_output=True, text=True, check=False, timeout=120)
            if result.returncode == 0 or "关闭修补点数量不匹配" not in result.stderr:
                raise RuntimeError("未知监听关闭数量没有在配置阶段明确拒绝")
    print("Socket guard configuration assertions passed: 6")


if __name__ == "__main__":
    main()
