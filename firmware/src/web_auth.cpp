#include "web_auth.h"

#include <stdio.h>
#include <string.h>

static void new_code(WebAuth &a, uint32_t now_ms, WebAuthRandom rnd) {
  snprintf(a.code, sizeof(a.code), "%06u", (unsigned)(rnd() % 1000000u));
  a.code_at = now_ms;
  a.tries = 0;
}

bool web_auth_code_active(const WebAuth &a, uint32_t now_ms) {
  return a.code[0] && a.tries < kWebAuthMaxTries && now_ms - a.code_at < kWebAuthCodeMs;
}

const char *web_auth_start(WebAuth &a, uint32_t now_ms, WebAuthRandom rnd) {
  if (!web_auth_code_active(a, now_ms)) new_code(a, now_ms, rnd);
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
    a.tries++;
    return false;
  }
  a.code[0] = 0;
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

bool web_auth_cookie(const char *header, const char *name, char *out, size_t n) {
  if (!header || !n) return false;
  size_t name_len = strlen(name);
  const char *p = header;
  while (*p) {
    while (*p == ' ' || *p == ';') p++;
    const char *end = strchr(p, ';');
    size_t len = end ? (size_t)(end - p) : strlen(p);
    if (len > name_len && strncmp(p, name, name_len) == 0 && p[name_len] == '=') {
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
