# 联犀设备端

本仓库包含设备端固件和可移植设备运行时：

- `firmware/watcher/`：基于 ESP-IDF 的小智固件及板级适配，参见其 README 和构建脚本。
- `firmware/device-runtime/`：C++ 设备运行时、桌面和 ESP-IDF 适配及测试，参见其 README 和 SOURCES.md。

设备核心的桌面测试可在安装 CMake、C++17 编译器和相应依赖后执行：

```sh
cmake -S firmware/device-runtime -B .temp/device-runtime/build
cmake --build .temp/device-runtime/build -j 2
ctest --test-dir .temp/device-runtime/build --output-on-failure
```

ESP-IDF 编译检查执行 `bash firmware/device-runtime/tests/check_espidf.sh`；可通过 `UR_RESOURCE_GUARD` 指定兼容的资源门禁脚本。该检查不烧录设备。

Watcher 默认使用两个各 6 MiB 的 OTA 分区，资源分区位于 `0xE00000`；这与小智上游默认的 4 MiB 分区布局不同。普通 OTA 不会更新分区表，旧布局设备不能直接使用此固件升级，必须先核实实际分区并制定保留设备身份与数据的迁移方案。

Watcher 主机合同测试执行 `python3 -m unittest discover -s firmware/watcher/scripts/tests -p test_ur_ai_contract.py`。与 SaaS 模拟器比较的跨仓用例需要设置 `UR_SAAS_ROOT`；未设置时这些用例明确标记跳过。表情资源检查还需要 ESP-IDF 组件管理器下载已锁定的字体组件。

源码由 SaaS 仓库 `41ba8eeb38cb0347d60dfd1249dacde63f072ea6` 的设备侧快照迁入，保留各目录中原有版权和第三方许可文件；依赖及来源见各子项目说明。桌面测试和编译不能替代实体设备验收。
