# PlatformIO 编译前脚本：给 BOX-3 接上乐鑫语音库 esp-sr（本地唤醒词「喵喵同学」和回声消除）。
#
# Arduino 2.0.17（ESP-IDF 4.4）自带的 SDK 里没有 esp-sr，这里从 GitHub 下载 v1.7.1（最后一个支持 IDF 4.4 的版本）
# 的预编译库、头文件和唤醒词模型，放在 .pio/esp-sr 里（不进仓库），校验 sha256 后加到编译和链接参数里。
# 模型文件转成 include/sr_model_wn9.h，编进固件，不用另外分区，在线升级也能带上。
# esp-sr 的许可（ESPRESSIF MIT License）允许在乐鑫芯片上使用。
import hashlib
import os
import urllib.request

Import("env")

COMMIT = "8ed7b711b33f963dff44dd5b619dc5673ece7955"  # 标签 v1.7.1
BASE = "https://raw.githubusercontent.com/espressif/esp-sr/%s/" % COMMIT
MODEL = "wn9_miaomiaotongxue_tts"

LIBS = {
    "libc_speech_features.a": "544acd5c4e1da0200b3b90981dd068ac8aee077809ba2217c6a94616cef20cde",
    "libdl_lib.a": "5109a41ce5fda9da1cef81b309ab08cfc4cc3c90c210cf7558fed80037ec77d5",
    "libesp_audio_front_end.a": "54dd2964c8a3ea02d9690c2ac50b61fa40ae0e03ce5883b24152d0f6a94e724c",
    "libesp_audio_processor.a": "40ea9b4f3c66a4c1ef5e5160aea2ba9d28d327f84348e90891aab9e1f5550d65",
    "libflite_g2p.a": "5fb21804baec0e53883a046cafb099b568a5a181e906ed0ee70370e036836384",
    "libfst.a": "464146db3ca1b3c6f135b757b0b08d598d6c00646ba17ef0f18fd687ad5f37fe",
    "libhufzip.a": "0930fdbccb46ff20e2e81367c2e2d6399971a3b7881413efa440ab92d308ca64",
    "libmultinet.a": "c5d8c65bee0467ad3620252c62d038364747e1a58344e6e070e9d10da8062e22",
    "libnsnet.a": "dc88b473748e2bd4eca3457ebd73d2a284941578e7f8fa3cef481de79df6dca1",
    "libwakenet.a": "def41e81c18e39ced27061e07763819a3334446e2f2ac445be2132ead38952dc",
}
MODEL_FILES = {
    "_MODEL_INFO_": "05ff7ed07b6ee8fe4338084ac7549a6e81f50b1fb94dc64f7e49eed7b0a89362",
    "wn9_data": "39b92027f150094e1b7cc3212302b3c9ae980674123cf1d061ca04f25ebe6fed",
    "wn9_index": "b463a5051d01bddc1f5723cddc8f3d5f11a6c750106694ca4aa0c8fdc235032c",
}
HEADERS = [
    "dl_lib.h", "dl_lib_coefgetter_if.h", "dl_lib_conv_queue.h", "dl_lib_convq8_queue.h", "dl_lib_convq_queue.h",
    "dl_lib_matrix.h", "dl_lib_matrixq.h", "dl_lib_matrixq8.h", "esp_aec.h", "esp_afe_config.h",
    "esp_afe_sr_iface.h", "esp_afe_sr_models.h", "esp_agc.h", "esp_mase.h", "esp_mn_iface.h", "esp_mn_models.h",
    "esp_ns.h", "esp_nsn_iface.h", "esp_nsn_models.h", "esp_vad.h", "esp_wn_iface.h", "esp_wn_models.h",
    "flite_g2p.h",
]

def setup():
    project = env.subst("$PROJECT_DIR")
    root = os.path.join(env.subst("$PROJECT_BUILD_DIR"), "..", "esp-sr", COMMIT[:12])
    root = os.path.normpath(root)


    def sha256(path):
        h = hashlib.sha256()
        with open(path, "rb") as f:
            for block in iter(lambda: f.read(1 << 16), b""):
                h.update(block)
        return h.hexdigest()


    def fetch(rel, dst, digest=None):
        if os.path.exists(dst) and (digest is None or sha256(dst) == digest):
            return
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        print("esp-sr: downloading %s" % rel)
        last = None
        for _ in range(4):  # GitHub 偶尔超时，多试几次
            try:
                with urllib.request.urlopen(BASE + rel, timeout=60) as r:
                    data = r.read()
                break
            except Exception as e:  # noqa: BLE001
                last = e
        else:
            raise RuntimeError("esp-sr: download %s failed: %s" % (rel, last))
        tmp = dst + ".tmp"
        with open(tmp, "wb") as f:
            f.write(data)
        if digest is not None and sha256(tmp) != digest:
            os.remove(tmp)
            raise RuntimeError("esp-sr: %s checksum mismatch" % rel)
        os.replace(tmp, dst)


    lib_dir = os.path.join(root, "lib")
    inc_dir = os.path.join(root, "include")
    for name, digest in LIBS.items():
        fetch("lib/esp32s3/" + name, os.path.join(lib_dir, name), digest)
    for name in HEADERS:
        fetch("include/esp32s3/" + name, os.path.join(inc_dir, name))
    fetch("src/include/model_path.h", os.path.join(inc_dir, "model_path.h"))
    model_dir = os.path.join(root, MODEL)
    for name, digest in MODEL_FILES.items():
        fetch("model/wakenet_model/%s/%s" % (MODEL, name), os.path.join(model_dir, name), digest)

    # 模型文件转成数组，sr_model.cpp 用它们拼出 esp-sr 要的模型列表
    out = os.path.join(project, "include", "sr_model_wn9.h")
    parts = [
        "// 由 tools/esp_sr.py 从 esp-sr 的 %s 模型生成，不要手改\n#pragma once\n#include <stddef.h>\n#include <stdint.h>\n\n" % MODEL,
        'static const char SR_MODEL_NAME[] = "%s";\n' % MODEL,
    ]
    for i, name in enumerate(sorted(MODEL_FILES)):
        with open(os.path.join(model_dir, name), "rb") as f:
            data = f.read()
        rows = []
        for k in range(0, len(data), 24):
            rows.append("  " + ",".join("0x%02x" % b for b in data[k:k + 24]) + ",")
        # 4 字节对齐：wn9_data 里是按 int16/float 读的系数
        parts.append("static const uint8_t SR_FILE_%d[] __attribute__((aligned(16))) = {\n%s\n};\n" % (i, "\n".join(rows)))
    names = sorted(MODEL_FILES)
    parts.append("static const char *const SR_FILE_NAMES[] = {%s};\n" % ", ".join('"%s"' % n for n in names))
    parts.append("static const uint8_t *const SR_FILE_DATA[] = {%s};\n" % ", ".join("SR_FILE_%d" % i for i in range(len(names))))
    parts.append("static const int SR_FILE_SIZES[] = {%s};\n" % ", ".join(
        str(os.path.getsize(os.path.join(model_dir, n))) for n in names))
    parts.append("static const int SR_FILE_COUNT = %d;\n" % len(names))
    text = "".join(parts)
    old = None
    if os.path.exists(out):
        with open(out) as f:
            old = f.read()
    if old != text:
        with open(out, "w") as f:
            f.write(text)

    env.Append(CPPPATH=[inc_dir], CPPDEFINES=["HAS_SR"])
    # 这几个库互相引用，重复一遍代替 --start-group；放在最前面，后面 SDK 里的 esp-dsp、cJSON、libc 能补上它们缺的符号
    sr_libs = ["wakenet", "esp_audio_front_end", "esp_audio_processor", "nsnet", "multinet", "dl_lib",
               "c_speech_features", "hufzip", "fst", "flite_g2p"]
    env.Prepend(LIBPATH=[lib_dir], LIBS=sr_libs + sr_libs)
    print("esp-sr: %s ready" % MODEL)


if env.subst("$PIOENV") == "box3":  # 只有 BOX-3 有麦克风
    setup()
