# 代码来源与依赖

基线：`origin/master c11781818b3b8854d8fce66dd64b919c9addf984`。

| 本项目内容 | 来源与修改 |
| --- | --- |
| `include/ur/device/fixed_queue.hpp` | 复制改编 `firmware/watcher/main/audio/fixed_queue.h`；保留固定容量环形存储，移除 ESP abort，空读改为指针、满队列不移动调用者资源；MIT 许可见 LICENSE.watcher |
| `core/session.cpp` | 新实现；参考 Watcher 的协议／板级分离，不能直接复用其互斥听说状态作为全双工状态 |
| `desktop-ipc/` | 本任务新增的浏览器IPC平台适配；复用本仓IPCClient、设备认证、MQTT和回环桥，不复制第三方状态机，不修改原Watcher；真实媒体E2E另行验收 |
| `core/udp_channel.cpp` | 根据当前后端 `aisvr/internal/server/udp/session_crypto.go` 和 `imsvr/internal/voice/udpserver/frame.go` 对齐 16 字节包头；不改变生产实现 |
| GoogleTest | v1.16.0，固定提交 `6910c9d9165801d8827d628cb72eb7ea9dd538c5`；优先 exact 版本安装包，否则 CMake 下载该提交 |
| cJSON | 与 Watcher 相同的解析库类型，桌面固定 v1.7.19 / `c859b25da02955fef659d658b8f324b5cde87be3`，解析深度限制为 32 |
| OpenSSL | 桌面密码适配使用系统 Crypto 库；核心仅依赖 AesCtrPort，不依赖 OpenSSL |
| libmosquitto | 桌面 MQTT 适配使用系统库，本轮 Linux 验证 2.0.21；测试 Broker 为同版本，非 vendored 测试框架依赖 |
| libopus | 桌面真实编解码使用系统库，本轮 Linux 验证 1.5.2；核心只依赖 AudioCodecPort |
| libcurl | 桌面流式HTTPS端口使用系统开发库，最低7.85；本轮Linux适配验证使用8.14.1。共享核心仅依赖HttpPort／HttpBodySource；Windows依赖版本由现有固定vcpkg baseline选择，新适配Windows产物尚待复验 |
| IXWebSocket | 仅可选桌面浏览器桥使用，v11.4.6 / `2efe037c9cc96fd536774f17bdb5215161ee5087`，BSD-3-Clause见LICENSE.IXWebSocket；不进入共享核心，回环端点关闭TLS与消息压缩 |
| 设备签名 | 对齐 `backend/things/share/domain/deviceAuth/loginAuth.go` 的 clientId、username、HMAC-SHA256 密码格式；仅测试公开固定数据，没有设备密钥入库 |

不是整份 ESP-IDF 工程复制：设备功能按可复用模块逐项移植，原 Watcher 源码不变。Linux桌面已接后端新信令及真实模型，并验证WebRTC AI语音和一对一基础视频；软件虚拟媒体结果不代表实体硬件验收。Linux系统 OpenSSL 为 3.5.7；Windows构建使用`vcpkg.json`锁定的独立工具链依赖，执行状态见TASK-143，系统适配库不冒充已验证的跨平台产物。

IXWebSocket固定版接收调用没有与传输层发送失败后的关闭共用锁，TSan背压用例复现了recv/close竞争。`cmake/ixwebsocket_socket_guard.cmake`对构建目录副本增加同一socket锁，保留原版权头，不修改下载源或参考仓库；匹配点不符时配置失败，升级依赖必须重新审查。回调关闭连接前先释放自有队列锁，避免库状态回调造成反向锁序。
