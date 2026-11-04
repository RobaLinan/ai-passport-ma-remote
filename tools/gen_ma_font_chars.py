#!/usr/bin/env python3
"""Regenerate assets/fonts/chars.txt for the MA remote CJK font subsets."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "assets" / "fonts" / "chars.txt"

UI_STRINGS = (
    "暂无播放无法连接播放器关闭音量播放中已暂停未知曲目艺人专辑正在缓冲网络错误重试设置"
    "正在连接配置检查电脑隧道断开更新网络任务启动失败未运行脚本失效"
)
# GB2312 A1A4 decodes to U+30FB under Python's codec; the UI uses U+00B7.
PUNCTUATION = "·•"


def main() -> int:
    chars = {chr(c) for c in range(0x20, 0x7F)}
    for high in range(0xA1, 0xF8):
        for low in range(0xA1, 0xFF):
            try:
                chars.add(bytes([high, low]).decode("gb2312"))
            except UnicodeDecodeError:
                pass
    chars.update(UI_STRINGS)
    chars.update(PUNCTUATION)
    text = "".join(sorted(c for c in chars if c not in "\r\n"))
    OUT.write_text(text, encoding="utf-8")
    print(f"wrote {OUT} ({len(text)} chars)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
