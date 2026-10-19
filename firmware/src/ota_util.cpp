#include "ota_util.h"

#include <ctype.h>
#include <string.h>

bool ota_parse_version(const char *s, int &major, int &minor, int &patch) {
  if (!s) return false;
  if (*s == 'v' || *s == 'V') s++;
  int parts[3];
  for (int i = 0; i < 3; i++) {
    if (!isdigit((unsigned char)*s)) return false;
    int n = 0;
    while (isdigit((unsigned char)*s)) {
      n = n * 10 + (*s++ - '0');
      if (n > 99999) return false;
    }
    parts[i] = n;
    if (i < 2 && *s++ != '.') return false;
  }
  // 预发布版本（如 v1.0.0-rc1）不算正式版本，不自动升级
  if (*s != 0) return false;
  major = parts[0];
  minor = parts[1];
  patch = parts[2];
  return true;
}

bool ota_is_newer(const char *current, const char *latest) {
  int a[3], b[3];
  if (!ota_parse_version(current, a[0], a[1], a[2])) return false;
  if (!ota_parse_version(latest, b[0], b[1], b[2])) return false;
  for (int i = 0; i < 3; i++) {
    if (b[i] != a[i]) return b[i] > a[i];
  }
  return false;
}

static bool is_md5_hex(const char *s) {
  if (!s || strlen(s) != 32) return false;
  for (; *s; s++) {
    if (!isdigit((unsigned char)*s) && !(*s >= 'a' && *s <= 'f')) return false;
  }
  return true;
}

bool ota_parse_manifest(JsonDocument &doc, const char *board, OtaManifest &out) {
  const char *version = doc["version"] | "";
  JsonObject b = doc["boards"][board];
  const char *url = b["url"] | "";
  const char *md5 = b["md5"] | "";
  uint32_t size = b["size"] | 0u;

  int x, y, z;
  if (!ota_parse_version(version, x, y, z)) return false;
  if (strncmp(url, "https://", 8) != 0 || strlen(url) >= sizeof(out.url)) return false;
  if (!is_md5_hex(md5) || size == 0) return false;

  strncpy(out.version, version, sizeof(out.version) - 1);
  out.version[sizeof(out.version) - 1] = 0;
  strcpy(out.url, url);
  strcpy(out.md5, md5);
  out.size = size;
  return true;
}

bool ota_looks_like_app_image(const uint8_t *data, size_t len) {
  if (len < 36 || data[0] != 0xE9) return false;  // ESP 镜像头的魔数
  // 镜像头 24 字节 + 第一个段头 8 字节之后，是应用描述信息 esp_app_desc_t，以 0xABCD5432 开头（小端）
  return data[32] == 0x32 && data[33] == 0x54 && data[34] == 0xCD && data[35] == 0xAB;
}
