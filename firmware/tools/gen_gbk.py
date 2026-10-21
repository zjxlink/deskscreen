"""生成 src/gbk_table.h：GBK 双字节编码到 Unicode 的对照表。

很多中文 MP3 的标签和 .lrc 歌词是 GBK 编码（老的下载站、Windows 记事本存的），
板子上把它们转成 UTF-8 才能显示。表按 首字节 0x81~0xFE、尾字节 0x40~0xFE（不含 0x7F）排，
转不出来的位置是 0。用法：python tools/gen_gbk.py
"""

import os

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "src", "gbk_table.h")

codes = []
for lead in range(0x81, 0xFF):
    for trail in range(0x40, 0xFF):
        if trail == 0x7F:
            continue
        try:
            ch = bytes([lead, trail]).decode("gbk")
            codes.append(ord(ch) if len(ch) == 1 and ord(ch) <= 0xFFFF else 0)
        except UnicodeDecodeError:
            codes.append(0)

with open(OUT, "w", encoding="utf-8", newline="\n") as f:
    f.write("// 由 tools/gen_gbk.py 生成，不要手改。GBK 双字节编码 -> Unicode，见 text_conv.cpp\n")
    f.write("#pragma once\n\n#include <stdint.h>\n\n")
    f.write("static constexpr int kGbkTrails = 190;  // 尾字节 0x40~0xFE，不含 0x7F\n")
    f.write("static const uint16_t kGbkTable[126 * kGbkTrails] = {\n")
    for i in range(0, len(codes), 16):
        f.write("    " + ", ".join("0x%04X" % c for c in codes[i:i + 16]) + ",\n")
    f.write("};\n")
print("wrote", OUT, len(codes), "entries")
