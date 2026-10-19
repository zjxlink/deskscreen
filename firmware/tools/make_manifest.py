#!/usr/bin/env python3
"""生成 OTA 用的 manifest.json，发布 Release 时由 CI 调用。

用法：
  python3 tools/make_manifest.py --version v0.5.0 \\
      --base-url https://github.com/OWNER/REPO/releases/download/v0.5.0 \\
      box3=dist/firmware-box3.bin desk_v1=dist/firmware-desk_v1.bin > manifest.json

板子名要和 include/board.h 里的 BOARD_NAME 一致，设备按它找自己的固件。
"""
import argparse
import hashlib
import json
import os
import re
import sys


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--version", required=True)
    p.add_argument("--base-url", required=True)
    p.add_argument("boards", nargs="+", metavar="BOARD=FILE")
    a = p.parse_args()

    if not re.fullmatch(r"v\d+\.\d+\.\d+", a.version):
        sys.exit(f"版本号必须是 vX.Y.Z 的形式：{a.version}")
    if not a.base_url.startswith("https://"):
        sys.exit("下载地址必须是 https")

    boards = {}
    for item in a.boards:
        name, _, path = item.partition("=")
        data = open(path, "rb").read()
        boards[name] = {
            "url": f"{a.base_url.rstrip('/')}/{os.path.basename(path)}",
            "md5": hashlib.md5(data).hexdigest(),
            "size": len(data),
        }
    json.dump({"version": a.version, "boards": boards}, sys.stdout, indent=2)
    print()


if __name__ == "__main__":
    main()
