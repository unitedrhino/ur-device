"""诊断构建前后检查唯一回调及netif包装器；发现其他实现或链接接管就拒绝。"""
import argparse
from pathlib import Path
import re
import sys


def find_other_owners(project_root, idf_root):
    """检查应用、受管组件和SDK网络源码；仅允许指定适配文件及诊断链接配置。"""
    allowed = (project_root / "main/protocols/ur_udp_wifi_trace.cc").resolve()
    netif_allowed = (project_root / "main/protocols/ur_udp_netif_trace.cc").resolve()
    cmake_allowed = (project_root / "main/CMakeLists.txt").resolve()
    roots = [project_root / name for name in ("main", "components", "managed_components")]
    roots += [idf_root / "components/esp_wifi/src", idf_root / "components/esp_netif"]
    # 关键扫描范围缺失时拒绝，不将空结果误判为已完成占用核验。
    for mandatory in (roots[0], roots[-2], roots[-1]):
        if not mandatory.is_dir():
            raise ValueError(f"回调占用检查缺少源码目录：{mandatory}")
    if not allowed.is_file():
        raise ValueError("回调占用检查缺少指定诊断适配器")
    if not netif_allowed.is_file():
        raise ValueError("交接占用检查缺少指定netif诊断适配器")
    owners = []
    for root in roots:
        if not root.is_dir():
            continue
        for path in root.rglob("*"):
            if (path.suffix not in (".c", ".cc", ".cpp", ".h", ".hpp", ".cmake")
                    and path.name != "CMakeLists.txt") or not path.is_file():
                continue
            # 标识符是ASCII；按原字节检查兼容厂商非UTF8注释，不忽略内容或漏检引用。
            content = path.read_bytes()
            setter = path.resolve() != allowed and re.search(rb"\besp_wifi_set_tx_done_cb\b", content)
            wrapper = path.resolve() != netif_allowed and re.search(
                rb"\b__wrap_esp_netif_transmit(?:_wrap)?\b", content)
            linker = path.resolve() != cmake_allowed and re.search(
                rb"--wrap[=,]esp_netif_transmit(?:_wrap)?\b", content)
            if setter or wrapper or linker:
                owners.append(str(path))
    return sorted(owners)


def main():
    """只读检查，不清注册者或修改SDK；失败时构建不得继续交付。"""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project-root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--idf-root", type=Path, required=True)
    arguments = parser.parse_args()
    try:
        owners = find_other_owners(arguments.project_root, arguments.idf_root)
    except (OSError, ValueError) as error:
        print(str(error), file=sys.stderr)
        return 1
    if owners:
        print("发送诊断发现其他回调引用或netif包装器，拒绝接管：", file=sys.stderr)
        for owner in owners:
            print(owner, file=sys.stderr)
        return 1
    print("发送诊断回调/netif包装器源码占用检查通过；SDK结果仍不等同于端到端送达。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
