#include <string.h>

#include "bing.h"

void bing_build_filter(JsonDocument &filter) {
  JsonObject img = filter["images"][0].to<JsonObject>();
  img["startdate"] = true;
  img["urlbase"] = true;
  img["title"] = true;
  img["copyright"] = true;
}

bool bing_is_date(const char *s) {
  if (!s || strlen(s) != 8) return false;
  for (int i = 0; i < 8; i++) {
    if (s[i] < '0' || s[i] > '9') return false;
  }
  return s[0] == '2';  // 20xx 年
}

bool bing_is_file(const char *name) {
  if (!name || strlen(name) != 12 || strcmp(name + 8, ".jpg") != 0) return false;
  char date[9];
  memcpy(date, name, 8);
  date[8] = '\0';
  return bing_is_date(date);
}

// 复制最多 n 个字节，不切断 UTF-8 字符，结果总以 0 结尾
static void utf8_append(char *out, size_t size, const char *s, size_t n) {
  size_t len = strlen(out);
  if (len + 1 >= size) return;
  if (n > size - 1 - len) {
    n = size - 1 - len;
    while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;  // s[n] 是后续字节说明会切开
  }
  memcpy(out + len, s, n);
  out[len + n] = '\0';
}

static void trim_span(const char *&s, size_t &n) {
  while (n > 0 && (*s == ' ' || *s == '\t')) s++, n--;
  while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) n--;
}

void bing_caption(const char *title, const char *copyright, char *out, size_t size) {
  if (!size) return;
  out[0] = '\0';
  const char *t = title ? title : "";
  size_t tn = strlen(t);
  trim_span(t, tn);
  // 地点：去掉最后的「(© ...)」或全角的「（© ...）」
  const char *c = copyright ? copyright : "";
  size_t cn = strlen(c);
  const char *cut = nullptr;
  for (const char *p = c; (p = strstr(p, "\xC2\xA9")) != nullptr; p++) cut = p;  // ©
  if (cut) {
    const char *q = cut;
    while (q > c && q[-1] == ' ') q--;
    if (q > c && q[-1] == '(') {
      q--;
    } else if (q - c >= 3 && memcmp(q - 3, "\xEF\xBC\x88", 3) == 0) {  // （
      q -= 3;
    }
    cn = q - c;
  }
  trim_span(c, cn);
  if (tn > 0) utf8_append(out, size, t, tn);
  bool same = tn == cn && memcmp(t, c, tn) == 0;
  if (cn > 0 && !same) {
    if (tn > 0) utf8_append(out, size, " \xC2\xB7 ", 4);  // ·
    utf8_append(out, size, c, cn);
  }
}

// 地址只认 /th?id= 开头、后面只有字母数字和 ._- 的，再拼上 _320x240.jpg
static bool make_path(const char *urlbase, char *out, size_t size) {
  static const char kPrefix[] = "/th?id=";
  static const char kSuffix[] = "_320x240.jpg";
  if (!urlbase || strncmp(urlbase, kPrefix, sizeof(kPrefix) - 1) != 0) return false;
  size_t n = strlen(urlbase);
  if (n <= sizeof(kPrefix) - 1 || n + sizeof(kSuffix) > size) return false;
  for (size_t i = sizeof(kPrefix) - 1; i < n; i++) {
    char ch = urlbase[i];
    bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '.' ||
              ch == '_' || ch == '-';
    if (!ok) return false;
  }
  memcpy(out, urlbase, n);
  memcpy(out + n, kSuffix, sizeof(kSuffix));
  return true;
}

int bing_parse(JsonDocument &doc, BingImage *out, int max) {
  int n = 0;
  for (JsonObject img : doc["images"].as<JsonArray>()) {
    if (n >= max) break;
    BingImage &b = out[n];
    const char *date = img["startdate"] | "";
    if (!bing_is_date(date) || !make_path(img["urlbase"] | "", b.path, sizeof(b.path))) continue;
    bool dup = false;
    for (int i = 0; i < n; i++) dup = dup || strcmp(out[i].date, date) == 0;
    if (dup) continue;
    memcpy(b.date, date, sizeof(b.date));
    bing_caption(img["title"] | "", img["copyright"] | "", b.caption, sizeof(b.caption));
    n++;
  }
  return n;
}

int album_next_source(int last, bool has_local, bool has_bing) {
  if (has_local && has_bing) return last == 0 ? 1 : 0;
  if (has_local) return 0;
  if (has_bing) return 1;
  return -1;
}
