"""检查实际ELF的诊断隔离；只读取符号，缺工具、缺符号或正式包误链接均拒绝。"""
import argparse
from pathlib import Path
import re
import subprocess
import sys


def validate_symbols(text, enabled):
    """诊断包须有实际STA交接路径；正式包不能保留项目诊断入口或netif包装器。"""
    defined = [match.group(1) for line in text.splitlines()
               if (match := re.match(r"^[0-9a-fA-F]+ [TtWw] (.+)$", line))]
    if not defined:
        raise ValueError("无法识别实际ELF的已定义函数符号")
    prefixes = ("UrUdpNetifTrace", "UrUdpWifiTrace", "__wrap_esp_netif_transmit")
    if not enabled:
        if any(name.startswith(prefixes) for name in defined):
            raise ValueError("正式固件意外链接发送诊断")
        return
    # 固定SDK的wlanif直接走transmit_wrap；未使用的另一入口可能被链接器回收。
    # 正文函数存在不证明每包交接，仍需同轮实际匹配和返回码/质量快照。
    for prefix in ("UrUdpNetifTraceStart(", "UrUdpNetifTraceStop(", "UrUdpNetifTraceFlush(",
                   "UrUdpWifiTraceStart(", "__wrap_esp_netif_transmit_wrap"):
        if not any(name.startswith(prefix) for name in defined):
            raise ValueError("诊断固件缺少已定义符号：" + prefix)


def main():
    """调用固定SDK的nm只读校验；输出合同结论，不打印ELF完整符号或配置。"""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--elf", type=Path, required=True)
    parser.add_argument("--enabled", choices=("0", "1"), required=True)
    args = parser.parse_args()
    try:
        if not args.elf.is_file():
            raise ValueError("实际ELF不存在")
        result = subprocess.run(["xtensa-esp-elf-nm", "-C", str(args.elf)],
                                text=True, capture_output=True, check=True)
        validate_symbols(result.stdout, args.enabled == "1")
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print("发送诊断ELF门禁失败：" + str(error), file=sys.stderr)
        return 1
    print("发送诊断ELF开关与STA交接符号门禁通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
