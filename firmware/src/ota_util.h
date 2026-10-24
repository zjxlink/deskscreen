#pragma once

#include <ArduinoJson.h>
#include <stddef.h>
#include <stdint.h>

// OTA 中不依赖硬件的部分，可在电脑上测试

struct OtaManifest {
  char version[24];  // 如 v0.5.0
  char url[192];     // 本板子固件的下载地址（已换算成完整地址）
  char md5[33];      // 固件的 MD5，32 位小写十六进制
  uint32_t size;
};

// 解析版本号 "v1.2.3" 或 "1.2.3"，成功返回 true
bool ota_parse_version(const char *s, int &major, int &minor, int &patch);

// latest 是否比 current 新。current 不是正式版本号（如开发版 "dev" 或提交号）时返回 false，
// 避免开发版被自动覆盖；需要时可以在设置页面手动升级
bool ota_is_newer(const char *current, const char *latest);

// 解析 manifest.json，取出 board 对应的固件信息；格式不对或没有这个板子返回 false
// {"version":"v0.5.0","boards":{"box3":{"url":"https://...","md5":"...","size":123}}}
// manifest_url 是 manifest 本身的地址：固件地址可以写成相对它的路径（自建服务器用，如 "v0.5.0/firmware-box3.bin"），
// 也可以是 http 地址，但 https 的 manifest 不能指向 http 的固件。不传时只接受完整的 https 地址
bool ota_parse_manifest(JsonDocument &doc, const char *board, OtaManifest &out,
                        const char *manifest_url = nullptr);

// 把 manifest 里的固件地址 ref 换算成完整地址：完整地址原样返回，"/a.bin" 接在域名后，"a.bin" 接在 manifest 所在目录后。
// 结果放不下或 base 不是 http(s) 地址时返回 false
bool ota_resolve_url(const char *base, const char *ref, char *out, size_t n);

// 设置里填的升级服务器地址是否可用：http:// 或 https:// 开头，有主机名，长度合适
bool ota_is_valid_source_url(const char *url);

// 判断上传的文件开头是不是 ESP32 程序固件（而不是带 bootloader 的单文件固件或其他文件）。
// 程序固件的第 32 字节起是 esp_app_desc_t 的魔数 0xABCD5432；需要至少 36 字节
bool ota_looks_like_app_image(const uint8_t *data, size_t len);

// 每块板子的固件里都嵌着 "DSBOARD:<板子名>"（ota.cpp 的 ds_board_tag）。网页上传固件时边收边找它，
// 收完对一下是不是本板子的，免得把别的板子的固件装上去。老固件里没有这个标记，找不到时不拦
struct OtaBoardTag {
  char name[16] = "";
  int match = 0;   // 已经对上了 "DSBOARD:" 的几个字
  int got = -1;    // 正在收板子名：已经收了几个字；-1 表示没在收
  bool found = false;
};
void ota_board_tag_feed(OtaBoardTag &t, const uint8_t *data, size_t len);
