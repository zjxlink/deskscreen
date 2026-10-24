#include "web_auth.h"

#include <stdio.h>
#include <ctype.h>
#include <string.h>

static void new_code(WebAuth &a, uint32_t now_ms, WebAuthRandom rnd) {
  snprintf(a.code, sizeof(a.code), "%06u", (unsigned)(rnd() % 1000000u));
  a.code_at = now_ms;
  a.tries = 0;
}

bool web_auth_code_active(const WebAuth &a, uint32_t now_ms) {
  return a.code[0] && a.tries < kWebAuthMaxTries && now_ms - a.code_at < kWebAuthCodeMs;
}

uint32_t web_auth_locked_ms(const WebAuth &a, uint32_t now_ms) {
  if (a.burned == 0) return 0;
  uint32_t wait = kWebAuthLockMs;
  for (int i = 1; i < a.burned && wait < kWebAuthLockMaxMs; i++) wait *= 2;
  if (wait > kWebAuthLockMaxMs) wait = kWebAuthLockMaxMs;
  uint32_t passed = now_ms - a.lock_at;
  return passed < wait ? wait - passed : 0;
}

const char *web_auth_start(WebAuth &a, uint32_t now_ms, WebAuthRandom rnd) {
  if (web_auth_code_active(a, now_ms)) return a.code;
  if (web_auth_locked_ms(a, now_ms)) return nullptr;
  new_code(a, now_ms, rnd);
  return a.code;
}

static bool valid_token(const char *t) {
  if (strlen(t) != kWebAuthTokenLen) return false;
  for (int i = 0; i < kWebAuthTokenLen; i++) {
    char c = t[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}

static void add_token(WebAuth &a, const char *t) {
  if (a.count == kWebAuthMaxTokens) {  // 满了挤掉最早的
    memmove(a.tokens[0], a.tokens[1], sizeof(a.tokens[0]) * (kWebAuthMaxTokens - 1));
    a.count--;
  }
  memcpy(a.tokens[a.count], t, kWebAuthTokenLen);
  a.tokens[a.count][kWebAuthTokenLen] = 0;
  a.count++;
}

bool web_auth_pair(WebAuth &a, const char *code, uint32_t now_ms, WebAuthRandom rnd, char *token_out) {
  if (!web_auth_code_active(a, now_ms) || !code) return false;
  if (strcmp(code, a.code) != 0) {
    if (++a.tries >= kWebAuthMaxTries) {  // 这个码作废，下一个要等
      a.code[0] = 0;
      a.burned++;
      a.lock_at = now_ms;
    }
    return false;
  }
  a.code[0] = 0;
  a.burned = 0;
  for (int i = 0; i < kWebAuthTokenLen; i += 8) snprintf(token_out + i, 9, "%08x", (unsigned)rnd());
  add_token(a, token_out);
  return true;
}

bool web_auth_check(const WebAuth &a, const char *token) {
  if (!token || !valid_token(token)) return false;
  bool found = false;
  for (int i = 0; i < a.count; i++) {
    uint8_t diff = 0;
    for (int j = 0; j < kWebAuthTokenLen; j++) diff |= (uint8_t)(a.tokens[i][j] ^ token[j]);
    if (diff == 0) found = true;
  }
  return found;
}

void web_auth_forget_all(WebAuth &a) {
  a.count = 0;
  memset(a.tokens, 0, sizeof(a.tokens));
}

void web_auth_save_tokens(const WebAuth &a, char *out, size_t n) {
  size_t len = 0;
  if (n) out[0] = 0;
  for (int i = 0; i < a.count; i++) {
    int w = snprintf(out + len, n - len, "%s%s", i ? "," : "", a.tokens[i]);
    if (w < 0 || (size_t)w >= n - len) {  // 放不下就只存前面完整的
      out[len] = 0;
      return;
    }
    len += w;
  }
}

void web_auth_load_tokens(WebAuth &a, const char *s) {
  web_auth_forget_all(a);
  if (!s) return;
  while (*s) {
    const char *end = strchr(s, ',');
    size_t len = end ? (size_t)(end - s) : strlen(s);
    if (len == kWebAuthTokenLen) {
      char t[kWebAuthTokenLen + 1];
      memcpy(t, s, len);
      t[len] = 0;
      if (valid_token(t)) add_token(a, t);
    }
    if (!end) break;
    s = end + 1;
  }
}

bool web_auth_cookie(const char *header, const char *name, char *out, size_t n, int skip) {
  if (!header || !n) return false;
  size_t name_len = strlen(name);
  const char *p = header;
  while (*p) {
    while (*p == ' ' || *p == ';') p++;
    const char *end = strchr(p, ';');
    size_t len = end ? (size_t)(end - p) : strlen(p);
    if (len > name_len && strncmp(p, name, name_len) == 0 && p[name_len] == '=' && skip-- == 0) {
      size_t vlen = len - name_len - 1;
      if (vlen >= n) return false;
      memcpy(out, p + name_len + 1, vlen);
      out[vlen] = 0;
      return true;
    }
    if (!end) break;
    p = end + 1;
  }
  return false;
}

bool web_auth_host_ok(const char *host) {
  if (!host || !host[0]) return true;  // 老的 HTTP/1.0 客户端不带 Host
  if (host[0] == '[') return true;     // IPv6 地址
  char h[64];
  size_t n = 0;
  for (; host[n] && host[n] != ':' && n < sizeof(h) - 1; n++) h[n] = (char)tolower((unsigned char)host[n]);
  h[n] = 0;
  if (!strchr(h, '.')) return true;  // 单个名字（deskscreen、路由器给的主机名）
  bool ip = true;
  for (size_t i = 0; i < n; i++) {
    if (!isdigit((unsigned char)h[i]) && h[i] != '.') ip = false;
  }
  if (ip) return true;
  static const char *const kLocal[] = {".local", ".lan", ".home", ".internal", ".home.arpa", ".localdomain"};
  for (const char *suffix : kLocal) {
    size_t m = strlen(suffix);
    if (n > m && strcmp(h + n - m, suffix) == 0) return true;
  }
  return false;
}
