#!/usr/bin/env python3
"""检查编译出的固件格式，CI 每次编译后运行。

  firmware.bin：程序固件（OTA 用），第 32 字节起应是应用描述信息的魔数 0xABCD5432
  firmware-merged.bin：单文件固件，开头是 bootloader（没有这个魔数），0x10000 处才是程序

设备上传固件时用同样的规则拒绝单文件固件（见 src/ota_util.cpp），这里保证规则和实际产物一致。
用法：python3 tools/check_image.py firmware.bin firmware-merged.bin
"""
import sys

APP_DESC_MAGIC = bytes([0x32, 0x54, 0xCD, 0xAB])
APP_OFFSET = 0x10000


def is_app(data, offset=0):
    return data[offset] == 0xE9 and data[offset + 32:offset + 36] == APP_DESC_MAGIC


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    app = open(sys.argv[1], "rb").read()
    merged = open(sys.argv[2], "rb").read()
    errors = []
    if not is_app(app):
        errors.append(f"{sys.argv[1]} 不像程序固件")
    if merged[0] != 0xE9 or is_app(merged):
        errors.append(f"{sys.argv[2]} 开头应该是 bootloader")
    if not is_app(merged, APP_OFFSET) or merged[APP_OFFSET:APP_OFFSET + len(app)] != app:
        errors.append(f"{sys.argv[2]} 的 0x{APP_OFFSET:x} 处应该是和 {sys.argv[1]} 相同的程序")
    for e in errors:
        print("错误：" + e)
    if errors:
        sys.exit(1)
    print(f"固件格式正确：程序 {len(app)} 字节，单文件 {len(merged)} 字节")


if __name__ == "__main__":
    main()
