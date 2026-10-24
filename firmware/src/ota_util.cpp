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
    if (!isdigit((unsigned char)*s) && !isxdigit((unsigned char)*s)) return false;
  }
  return true;
}

static bool starts_with(const char *s, const char *prefix) {
  return strncmp(s, prefix, strlen(prefix)) == 0;
}

static bool is_absolute_url(const char *s) {
  return starts_with(s, "https://") || starts_with(s, "http://");
}

bool ota_resolve_url(const char *base, const char *ref, char *out, size_t n) {
  if (!ref || !*ref || n == 0) return false;
  size_t keep;  // base 里保留的前缀长度
  if (is_absolute_url(ref)) {
    keep = 0;
  } else {
    if (!base || !is_absolute_url(base)) return false;
    const char *host = strstr(base, "://") + 3;
    const char *path = strchr(host, '/');
    if (*ref == '/') {
      keep = path ? (size_t)(path - base) : strlen(base);
    } else {
      // 去掉 ?参数 后，保留到最后一个 / 为止
      const char *end = strchr(host, '?');
      if (!end) end = base + strlen(base);
      const char *slash = nullptr;
      for (const char *c = path; c && c < end; c++) {
        if (*c == '/') slash = c;
      }
      if (!slash) {
        keep = end - base;
        if (keep + 1 + strlen(ref) >= n) return false;
        memcpy(out, base, keep);
        out[keep] = '/';
        strcpy(out + keep + 1, ref);
        return true;
      }
      keep = slash + 1 - base;
    }
  }
  if (keep + strlen(ref) >= n) return false;
  memcpy(out, base, keep);
  strcpy(out + keep, ref);
  return true;
}

bool ota_is_valid_source_url(const char *url) {
  if (!url || !is_absolute_url(url) || strlen(url) >= 160) return false;
  const char *host = strstr(url, "://") + 3;
  if (*host == 0 || *host == '/') return false;
  for (const char *c = url; *c; c++) {
    if ((unsigned char)*c <= ' ') return false;
  }
  return true;
}

bool ota_parse_manifest(JsonDocument &doc, const char *board, OtaManifest &out, const char *manifest_url) {
  const char *version = doc["version"] | "";
  JsonObject b = doc["boards"][board];
  const char *ref = b["url"] | "";
  const char *md5 = b["md5"] | "";
  uint32_t size = b["size"] | 0u;

  int x, y, z;
  if (!ota_parse_version(version, x, y, z)) return false;
  if (!is_md5_hex(md5) || size == 0) return false;
  char url[sizeof(out.url)];
  if (!manifest_url) {
    if (!starts_with(ref, "https://") || strlen(ref) >= sizeof(url)) return false;
    strcpy(url, ref);
  } else if (!ota_resolve_url(manifest_url, ref, url, sizeof(url))) {
    return false;
  }
  // https 拿到的 manifest 不能把固件下载降级到 http
  if (starts_with(manifest_url ? manifest_url : "https://", "https://") && !starts_with(url, "https://")) return false;

  strncpy(out.version, version, sizeof(out.version) - 1);
  out.version[sizeof(out.version) - 1] = 0;
  strcpy(out.url, url);
  for (int i = 0; i < 33; i++) out.md5[i] = (char)tolower((unsigned char)md5[i]);  // Update.setMD5 比较时区分大小写
  out.size = size;
  return true;
}

bool ota_looks_like_app_image(const uint8_t *data, size_t len) {
  if (len < 36 || data[0] != 0xE9) return false;  // ESP 镜像头的魔数
  // 镜像头 24 字节 + 第一个段头 8 字节之后，是应用描述信息 esp_app_desc_t，以 0xABCD5432 开头（小端）
  return data[32] == 0x32 && data[33] == 0x54 && data[34] == 0xCD && data[35] == 0xAB;
}

void ota_board_tag_feed(OtaBoardTag &t, const uint8_t *data, size_t len) {
  static const char kNeedle[] = {'D', 'S', 'B', 'O', 'A', 'R', 'D', ':'};  // 不写成字符串，免得这里也成了一个空名字的标记
  for (size_t i = 0; i < len && !t.found; i++) {
    char c = (char)data[i];
    if (t.got >= 0) {
      if (c == 0 && t.got > 0) {
        t.name[t.got] = 0;
        t.found = true;
      } else if (c > ' ' && c < 0x7F && t.got < (int)sizeof(t.name) - 1) {
        t.name[t.got++] = c;
        continue;
      } else {
        t.got = -1;  // 不是标记（名字太长或有怪字符）
      }
      t.match = 0;
      if (t.found) break;
    }
    if (c == kNeedle[t.match]) {
      if (++t.match == (int)sizeof(kNeedle)) {
        t.got = 0;
        t.match = 0;
      }
    } else {
      t.match = c == kNeedle[0] ? 1 : 0;
    }
  }
}
