# 可移植设备运行时

本目录提供 C++17 设备侧协议和会话核心，以及桌面和 ESP-IDF 适配。核心通过端口接口接入网络、音频、加密与设备驱动；调用方负责实际硬件、身份、连接及事件线程。主要能力包括设备认证、MQTT 信令、UDP 音频、语音会话、设备行为、文件媒体和 IPC 会话。各能力是否可用取决于宿主实际装配和平台支持，不应仅凭接口存在就向平台声明能力。

目录：

- `include/ur/device`、`core`：可移植接口、状态机和协议实现。
- `adapters/desktop`、`desktop-ipc`：桌面传输、浏览器媒体桥和 IPC 适配。
- `adapters/espidf/ur_device`：ESP-IDF 组件。
- `tests`：核心、适配和合同测试。
- `SOURCES.md`、`LICENSE.*`：来源、第三方依赖与许可。

## Linux 构建与测试

需要 CMake 3.22 以上、C++17 编译器和 Git；默认桌面适配需要 OpenSSL、libmosquitto、libopus 和 libcurl 开发包。CMake 会使用系统依赖或按固定版本获取第三方源码。于本仓库根目录执行：

```bash
cmake -S firmware/device-runtime -B .temp/device-runtime/desktop
cmake --build .temp/device-runtime/desktop -j 2
ctest --test-dir .temp/device-runtime/desktop --output-on-failure -j 2
```

纯核心构建可关闭 `UR_DESKTOP_CRYPTO`、`UR_DESKTOP_TRANSPORT`、`UR_DESKTOP_HTTP`；浏览器桥通过 `-DUR_DESKTOP_BROWSER=ON` 显式启用。构建目录和设备配置不应提交。自动化软件测试不等同于实体设备声学或摄像头验收。

ESP-IDF 全核心编译检查使用 Docker 和脚本固定的镜像，不访问或烧录硬件：

```bash
bash firmware/device-runtime/tests/check_espidf.sh
```

如需通过现有资源门禁运行，可设置 `UR_RESOURCE_GUARD` 为门禁脚本的绝对路径。实际板级驱动、网络、时钟、内存和会话事件循环须由集成方实现并单独验证。

## Windows 构建与安装包

在 Windows x64 上准备 Visual Studio 2022 C++ Build Tools、Git、Python、Node.js、Mosquitto 测试 Broker 和 Inno Setup 6，然后从仓库根目录执行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File firmware/device-runtime/scripts/build-windows.ps1
```

脚本在 `.temp/device-runtime/windows` 构建，测试成功后生成便携包及安装包。设备密钥由使用者在运行时输入并通过当前用户 DPAPI 保存；真实网络、音视频外设及 Windows 版本需单独验收。

## 与 SaaS 联调

真实平台联调需要独立的 SaaS 后端、授权设备和显式注入的配置。设备标识、密钥、MQTT 地址及 CA 通过运行环境提供，禁止提交真实凭据。后端 RPC 测试助手保留在 SaaS 的 `backend/imsvr/tools/device-media-support` 和 `backend/imsvr/tools/device-source-control`；它们是测试客户端，不属于设备运行时。对端受理请求并不证明实际媒体已播放或资源已释放，应分别核验设备实际状态。
