#include "text_conv.h"

#include <string.h>

#include "gbk_table.h"

// 往 out 里写一个字的 UTF-8，放不下（要给结尾的 '\0' 留一个字节）时返回 false
static bool put(uint32_t cp, char *out, size_t cap, size_t &len) {
  char b[4];
  size_t n;
  if (cp < 0x80) {
    b[0] = (char)cp;
    n = 1;
  } else if (cp < 0x800) {
    b[0] = (char)(0xC0 | cp >> 6);
    b[1] = (char)(0x80 | (cp & 0x3F));
    n = 2;
  } else if (cp < 0x10000) {
    b[0] = (char)(0xE0 | cp >> 12);
    b[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    b[2] = (char)(0x80 | (cp & 0x3F));
    n = 3;
  } else {
    b[0] = (char)(0xF0 | cp >> 18);
    b[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    b[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    b[3] = (char)(0x80 | (cp & 0x3F));
    n = 4;
  }
  if (len + n + 1 > cap) return false;
  memcpy(out + len, b, n);
  len += n;
  return true;
}

static size_t finish(char *out, size_t cap, size_t len) {
  if (cap) out[len] = '\0';
  return len;
}

// s 开头一个 UTF-8 字有几个字节，不合法时返回 0
static size_t utf8_len(const uint8_t *s, size_t n) {
  uint8_t c = s[0];
  size_t k = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
  if (k == 0 || k > n) return 0;
  if (k == 2 && c < 0xC2) return 0;  // 过长编码
  for (size_t i = 1; i < k; i++) {
    if ((s[i] & 0xC0) != 0x80) return 0;
  }
  return k;
}

bool utf8_valid(const uint8_t *s, size_t n) {
  size_t i = 0;
  while (i < n) {
    size_t k = utf8_len(s + i, n - i);
    if (k == 0) return false;
    i += k;
  }
  return true;
}

static size_t copy_utf8(const uint8_t *s, size_t n, char *out, size_t cap) {
  size_t len = 0;
  for (size_t i = 0; i < n && s[i];) {
    size_t k = utf8_len(s + i, n - i);
    if (k == 0) {
      if (!put('?', out, cap, len)) break;
      i++;
      continue;
    }
    if (len + k + 1 > cap) break;
    memcpy(out + len, s + i, k);
    len += k;
    i += k;
  }
  return finish(out, cap, len);
}

size_t text_from_latin1(const uint8_t *s, size_t n, char *out, size_t cap) {
  size_t len = 0;
  for (size_t i = 0; i < n && s[i]; i++) {
    if (!put(s[i], out, cap, len)) break;
  }
  return finish(out, cap, len);
}

// GBK 双字节字对应的 Unicode，没有时返回 0
static uint16_t gbk_lookup(uint8_t lead, uint8_t trail) {
  if (lead < 0x81 || lead > 0xFE || trail < 0x40 || trail > 0xFE || trail == 0x7F) return 0;
  int t = trail - 0x40 - (trail > 0x7F ? 1 : 0);
  return kGbkTable[(lead - 0x81) * kGbkTrails + t];
}

// 整段都能按 GBK 解开
static bool gbk_valid(const uint8_t *s, size_t n) {
  for (size_t i = 0; i < n && s[i];) {
    if (s[i] < 0x80) {
      i++;
    } else if (i + 1 < n && gbk_lookup(s[i], s[i + 1])) {
      i += 2;
    } else {
      return false;
    }
  }
  return true;
}

size_t text_from_gbk(const uint8_t *s, size_t n, char *out, size_t cap) {
  size_t len = 0;
  for (size_t i = 0; i < n && s[i];) {
    uint32_t cp = '?';
    if (s[i] < 0x80) {
      cp = s[i++];
    } else if (i + 1 < n && gbk_lookup(s[i], s[i + 1])) {
      cp = gbk_lookup(s[i], s[i + 1]);
      i += 2;
    } else {
      i++;
    }
    if (!put(cp, out, cap, len)) break;
  }
  return finish(out, cap, len);
}

size_t text_from_utf16(const uint8_t *s, size_t n, bool be, char *out, size_t cap) {
  size_t i = 0, len = 0;
  if (n >= 2 && s[0] == 0xFF && s[1] == 0xFE) {
    be = false;
    i = 2;
  } else if (n >= 2 && s[0] == 0xFE && s[1] == 0xFF) {
    be = true;
    i = 2;
  }
  auto unit = [&](size_t at) -> uint32_t { return be ? (s[at] << 8 | s[at + 1]) : (s[at + 1] << 8 | s[at]); };
  while (i + 1 < n) {
    uint32_t cp = unit(i);
    i += 2;
    if (cp == 0) break;
    if (cp >= 0xD800 && cp < 0xDC00) {
      uint32_t lo = i + 1 < n ? unit(i) : 0;
      if (lo >= 0xDC00 && lo < 0xE000) {
        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
        i += 2;
      } else {
        cp = '?';
      }
    } else if (cp >= 0xDC00 && cp < 0xE000) {
      cp = '?';
    }
    if (!put(cp, out, cap, len)) break;
  }
  return finish(out, cap, len);
}

size_t text_guess(const uint8_t *s, size_t n, char *out, size_t cap) {
  if (n >= 3 && s[0] == 0xEF && s[1] == 0xBB && s[2] == 0xBF) return copy_utf8(s + 3, n - 3, out, cap);
  if (n >= 2 && ((s[0] == 0xFF && s[1] == 0xFE) || (s[0] == 0xFE && s[1] == 0xFF)))
    return text_from_utf16(s, n, false, out, cap);
  size_t end = 0;
  while (end < n && s[end]) end++;
  if (utf8_valid(s, end)) return copy_utf8(s, end, out, cap);
  if (gbk_valid(s, end)) return text_from_gbk(s, end, out, cap);
  return text_from_latin1(s, end, out, cap);
}

void utf8_truncate(char *s, size_t max) {
  size_t n = strlen(s);
  if (n <= max) return;
  while (max > 0 && ((uint8_t)s[max] & 0xC0) == 0x80) max--;
  s[max] = '\0';
}

// s[i] 开始是空白时返回它占几个字节，否则返回 0
static size_t space_at(const char *s, size_t i) {
  uint8_t c = (uint8_t)s[i];
  if (c == ' ' || c == '\t' || c == '\r' || c == '\n') return 1;
  if (c == 0xE3 && (uint8_t)s[i + 1] == 0x80 && (uint8_t)s[i + 2] == 0x80) return 3;  // 全角空格
  return 0;
}

void text_trim(char *s) {
  size_t start = 0, k;
  while ((k = space_at(s, start)) > 0) start += k;
  size_t n = strlen(s + start);
  memmove(s, s + start, n + 1);
  for (;;) {
    if (n >= 1 && space_at(s, n - 1) == 1) {
      s[--n] = '\0';
    } else if (n >= 3 && space_at(s, n - 3) == 3) {
      n -= 3;
      s[n] = '\0';
    } else {
      break;
    }
  }
}
