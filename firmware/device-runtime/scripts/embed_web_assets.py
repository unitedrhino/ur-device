"""构建期将界面 UTF-8 字节嵌入 C++，分段避免 MSVC 单字面量限制。"""
import argparse
from pathlib import Path


def expression(text):
    """返回保持原文本字节的分段 string 表达式；拒绝无法安全追加的 NUL。"""
    if "\0" in text:
        raise ValueError("界面资产不得包含 NUL")
    # 每段最多 8192 UTF-8 字节，不把 Unicode 字符拆成非法编码。
    result = "std::string{}"
    for offset in range(0, len(text), 2048):
        escaped = []
        for char in text[offset:offset + 2048]:
            if char in ('"', '\\'):
                escaped.append('\\' + char)
            elif ord(char) < 32 or ord(char) == 127:
                escaped.append('\\%03o' % ord(char))
            else:
                escaped.append(char)
        result += '\n.append("' + ''.join(escaped) + '")'
    return result


def main():
    """从指定源码目录读取资产，仅向构建目录生成头文件。"""
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--test-fixtures", action="store_true")
    args = parser.parse_args()
    names = {"UR_WEB_HTML": "index.html", "UR_WEB_CSS": "app.css",
             "UR_WEB_JS": "app.js", "UR_WEB_WORKLET": "audio-worklet.js",
             "UR_INPUT_OWNERSHIP": "input-ownership.js",
             "UR_HUMAN_VOICE_MEDIA": "human-voice-media.js",
             "UR_CLIENT_THEME": "client-theme.js",
             "UR_VIDEO_HTML": "video.html", "UR_VIDEO_JS": "video.js",
             "UR_CALL_WORKLET": "call-worklet.js",
             "UR_FILE_PLAYER": "file-player.js", "UR_FILE_WORKLET": "file-worklet.js",
             "UR_FILE_LANE": "file-lane.js", "UR_FILE_HOST": "file-host.js",
             "UR_LIVE_PROBE": "live-probe.js", "UR_LIVE_PLAYER": "live-player.js",
             "UR_LIVE_HOST": "live-host.js", "UR_LIVE_WORKLET": "live-worklet.js",
             "UR_LIVE_TRANSFORM": "live-transform-worker.js"}
    for header in ("web_assets", "video_assets", "file_assets", "live_assets"):
        content = (args.source / "adapters/desktop" / (header + ".hpp.in")).read_bytes().decode("utf-8")
        for name, filename in names.items():
            placeholder = "@" + name + "@"
            if placeholder in content:
                asset = (args.source / "adapters/desktop/web" / filename).read_bytes().decode("utf-8")
                content = content.replace(placeholder, expression(asset))
        (args.output / (header + ".hpp")).write_bytes(content.encode("utf-8"))
    if args.test_fixtures:
        content = "// 文件说明：验证超长资产及转义的构建期测试语料。\n#pragma once\n#include <string>\nnamespace ur::device {\n"
        for name, text in (("large_ascii", 'abc"\r\n\t\\' * 18000),
                           ("large_utf8", '中文🎤\r\n"\\' * 5000), ("empty_asset", "")):
            content += "// 返回独立构建语料，供 C++ 字节级回归比较。\ninline std::string " + name + "() { return " + expression(text) + "; }\n"
        content += "}\n"
        (args.output / "embedded_assets_test.hpp").write_bytes(content.encode("utf-8"))


if __name__ == "__main__":
    main()
