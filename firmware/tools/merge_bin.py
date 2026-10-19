# PlatformIO 编译后脚本：把 bootloader、分区表、boot_app0 和程序合并成一个 firmware-merged.bin，
# 烧录时只需写到 0x0 地址（可用网页工具 https://espressif.github.io/esptool-js/）。
# 偏移和 flash 参数直接取自 PlatformIO 上传时用的配置，保证和 pio upload 写入的内容一致。
from os.path import join

Import("env")


def merge_bin(source, target, env):
    build_dir = env.subst("$BUILD_DIR")
    images = []
    for offset, path in env.get("FLASH_EXTRA_IMAGES", []):
        images += [offset, env.subst(path)]
    images += [env.subst("$ESP32_APP_OFFSET"), str(target[0])]

    board = env.BoardConfig()
    esptool = join(env.PioPlatform().get_package_dir("tool-esptoolpy"), "esptool.py")
    cmd = [
        '"$PYTHONEXE"', '"%s"' % esptool,
        "--chip", board.get("build.mcu"),
        "merge_bin", "-o", join(build_dir, "firmware-merged.bin"),
        "--flash_mode", "${__get_board_flash_mode(__env__)}",
        "--flash_freq", "${__get_board_f_image(__env__)}",
        "--flash_size", board.get("upload.flash_size", "4MB"),
    ] + images
    env.Execute(env.VerboseAction(" ".join(cmd), "Merging firmware-merged.bin"))


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", merge_bin)
