#!/usr/bin/env python3
"""生成 LVGL 中文字体（Noto Sans SC，SIL OFL 协议，可商用）。

LVGL 自带的 simsun_16_cjk 以繁体和日文为主，缺很多简体字，所以自己生成：
  font_cn_16：ASCII + GB2312 全部汉字（6763 个）+ 源码里用到的字，用于普通文字、城市名和股票名
  font_cn_24：ASCII + 源码里用到的字，用于标题和天气描述

用法（需要 Node.js）：
  npm install lv_font_conv@1.5.2 @expo-google-fonts/noto-sans-sc@0.4.4
  python3 tools/gen_fonts.py node_modules/@expo-google-fonts/noto-sans-sc/400Regular/NotoSansSC_400Regular.ttf

改了界面文字后重新运行一次即可。
"""

import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SRC = ROOT / "src"
OUT = SRC / "fonts"

# 中文标点和常用符号
EXTRA = "，。：；！？、（）“”‘’《》…—·°℃％"


def gb2312_hanzi():
    chars = []
    for hi in range(0xB0, 0xF8):  # 0xB0~0xD7 一级汉字，0xD8~0xF7 二级汉字
        for lo in range(0xA1, 0xFF):
            try:
                chars.append(bytes([hi, lo]).decode("gb2312"))
            except UnicodeDecodeError:
                pass  # 一级汉字区末尾的几个空位
    return chars


# 依次匹配注释和字符串字面量，避免把 "https://" 里的 // 当成注释
TOKEN = re.compile(r'//[^\n]*|/\*.*?\*/|"((?:[^"\\\n]|\\.)*)"', re.S)


def source_chars():
    chars = set()
    for f in SRC.glob("*.cpp"):
        if f.name == "web_config.cpp":
            continue  # 网页里的文字由浏览器显示，不需要进字库
        for m in TOKEN.finditer(f.read_text(encoding="utf-8")):
            if m.group(1):
                chars.update(c for c in m.group(1) if ord(c) > 0x7F)
    return chars


def build(ttf, name, size, symbols):
    out = OUT / f"{name}.c"
    subprocess.run(
        [
            "npx", "lv_font_conv",
            "--font", ttf,
            "--size", str(size),
            "--bpp", "4",
            "--format", "lvgl",
            "--no-compress",
            "--lv-include", "lvgl.h",
            "-r", "0x20-0x7E",
            "--symbols", "".join(sorted(symbols)),
            "-o", str(out),
        ],
        check=True,
    )
    print(f"{out.relative_to(ROOT)}: {len(symbols)} 个符号")


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    ttf = sys.argv[1]
    OUT.mkdir(exist_ok=True)
    used = source_chars() | set(EXTRA)
    build(ttf, "font_cn_16", 16, used | set(gb2312_hanzi()))
    build(ttf, "font_cn_24", 24, used)


if __name__ == "__main__":
    main()
