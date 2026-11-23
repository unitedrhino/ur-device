"""生成 Watcher 专用 Wi-Fi 扫描源副本，不修改依赖目录；上游锚点变化时拒绝静默构建。"""
import argparse
from pathlib import Path

# 固定锚点只适配多网络扫描范围及结果顺序，不替换 SDK 连接和重试逻辑。
ANCHOR = "    auto ssid_list = ssid_manager.GetSsidList();\n"
INSERTION = "    watcher::PrioritizeWifiScan(ap_records, ap_num);\n"
# 多网络必须扫描完整信道集合，否则排序无法优先未被扫描到的新热点。
SCAN_ANCHOR = "    if (use_saved_channels_scan_) {\n"
SCAN_REPLACEMENT = "    if (use_saved_channels_scan_ && watcher::CanUseSavedWifiChannels()) {\n"


def transform(source):
    """保留上游完整源和许可头，适配扫描范围和排序，拒绝重入或结构漂移。"""
    if source.count(ANCHOR) != 1 or source.count(SCAN_ANCHOR) != 1 or "ur_wifi_scan_order.h" in source:
        raise ValueError("Wi-Fi 依赖扫描锚点变化，请先审查适配")
    if source.index(ANCHOR) < source.index("void WifiStation::HandleScanResult()"):
        raise ValueError("Wi-Fi 扫描插入位置不正确")
    if not source.index("void WifiStation::StartScan()") < source.index(SCAN_ANCHOR) < source.index("void WifiStation::HandleScanResult()"):
        raise ValueError("Wi-Fi 信道扫描插入位置不正确")
    return '#include "ur_wifi_scan_order.h"\n' + source.replace(ANCHOR, ANCHOR + INSERTION).replace(SCAN_ANCHOR, SCAN_REPLACEMENT)


def main():
    """构建系统提供输入源与 build 内输出路径，不就地修改受管理依赖。"""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    if args.input.resolve() == args.output.resolve():
        raise ValueError("禁止覆盖依赖源文件")
    result = transform(args.input.read_text())
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(result)


if __name__ == "__main__":
    main()
