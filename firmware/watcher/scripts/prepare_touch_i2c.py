"""为 Watcher 生成 SPD2010 的 IDF 6 纯读适配，不修改受管理组件。"""
import argparse
from pathlib import Path

# READ_OLD 是上游无命令读入口；IDF 6 会把命令 0 转成长度为零的发送事务。
READ_OLD = 'esp_lcd_panel_io_rx_param(tp->io, 0, data_p, len)'
# READ_NEW 显式禁止命令阶段；寄存器地址已由驱动前一笔写事务发送。
READ_NEW = 'esp_lcd_panel_io_rx_param(tp->io, -1, data_p, len)'


def transform(source):
    """transform 仅替换唯一读调用；依赖变化时失败，避免静默生成错误驱动。"""
    if source.count(READ_OLD) != 1 or READ_NEW in source:
        raise ValueError('SPD2010 读入口发生变化，请重新审查 IDF 兼容性')
    return source.replace(READ_OLD, READ_NEW)


def main():
    """main 读取组件源码并把适配副本写入构建目录。"""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    source = transform(args.input.read_text())
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(source)


if __name__ == '__main__':
    main()
