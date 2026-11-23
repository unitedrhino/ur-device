#!/usr/bin/env python3
"""核对 LCOV 核心覆盖率：包含全部核心源文件和公开头文件中的可执行代码。"""
import pathlib
import sys


def main():
    """读取唯一报告参数，检查实际源文件覆盖范围以及行/分支阈值。"""
    if len(sys.argv) != 2:
        raise SystemExit("用法：check_coverage.py <core.info>")
    report = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")
    totals = {"LF": 0, "LH": 0, "BRF": 0, "BRH": 0}
    sources = set()
    for line in report.splitlines():
        name, _, value = line.partition(":")
        if name == "SF":
            sources.add(pathlib.Path(value).name)
        elif name in totals:
            totals[name] += int(value)
    # 从源码目录确定分母范围；新增核心不能因为忘记维护白名单而被漏算。
    core = pathlib.Path(__file__).resolve().parents[1] / "core"
    required = {path.name for path in core.glob("*.cpp")} | {"fixed_queue.hpp"}
    if not required.issubset(sources):
        raise SystemExit(f"核心覆盖范围缺失：{sorted(required - sources)}")
    for total, hit, threshold in [("LF", "LH", 85), ("BRF", "BRH", 75)]:
        rate = 100 * totals[hit] / totals[total] if totals[total] else 0
        print(f"{hit}/{total}: {totals[hit]}/{totals[total]} = {rate:.2f}% (要求 {threshold}%)")
        if rate < threshold:
            raise SystemExit("覆盖率未达标")


if __name__ == "__main__":
    main()
