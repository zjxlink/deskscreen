# PlatformIO 编译前脚本：把设置网页 web/index.html 压缩成 gzip，生成 include/web_index_gz.h
# 网页源码保持可读，板子上存的是压缩后的字节（约 15KB），浏览器自己解压。
import gzip
import os

Import("env")

project = env.subst("$PROJECT_DIR")
src = os.path.join(project, "web", "index.html")
dst = os.path.join(project, "include", "web_index_gz.h")

with open(src, "rb") as f:
    html = f.read()
data = gzip.compress(html, compresslevel=9, mtime=0)  # mtime=0：内容不变时生成的文件也不变

lines = []
for i in range(0, len(data), 20):
    lines.append("  " + ",".join("0x%02x" % b for b in data[i:i + 20]) + ",")
header = (
    "// 由 tools/web_gz.py 从 web/index.html 生成，不要手改\n"
    "#pragma once\n#include <stddef.h>\n#include <stdint.h>\n\n"
    "static const uint8_t WEB_INDEX_GZ[] = {\n" + "\n".join(lines) + "\n};\n"
    "static const size_t WEB_INDEX_GZ_LEN = %d;\n" % len(data)
)

old = None
if os.path.exists(dst):
    with open(dst) as f:
        old = f.read()
if old != header:
    with open(dst, "w") as f:
        f.write(header)
print("web/index.html: %d -> %d bytes gzip" % (len(html), len(data)))
