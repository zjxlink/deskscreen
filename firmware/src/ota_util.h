#pragma once

#include <ArduinoJson.h>
#include <stddef.h>
#include <stdint.h>

// OTA 中不依赖硬件的部分，可在电脑上测试

struct OtaManifest {
  char version[24];  // 如 v0.5.0
  char url[192];     // 本板子固件的下载地址（https）
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
bool ota_parse_manifest(JsonDocument &doc, const char *board, OtaManifest &out);

// 判断上传的文件开头是不是 ESP32 程序固件（而不是带 bootloader 的单文件固件或其他文件）。
// 程序固件的第 32 字节起是 esp_app_desc_t 的魔数 0xABCD5432；需要至少 36 字节
bool ota_looks_like_app_image(const uint8_t *data, size_t len);
