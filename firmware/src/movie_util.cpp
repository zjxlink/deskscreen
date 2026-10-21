#include "movie_util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool movie_is_file(const char *name) {
  if (!name || name[0] == '.' || name[0] == '\0') return false;
  size_t n = strlen(name);
  if (n < 5) return false;
  const char *ext = name + n - 4;
  return ext[0] == '.' && tolower((unsigned char)ext[1]) == 'a' && tolower((unsigned char)ext[2]) == 'v' &&
         tolower((unsigned char)ext[3]) == 'i';
}

// 一行一行地找：line 指向行首，len 不含换行
template <typename F>
static void each_line(const char *text, F fn) {
  for (const char *p = text; p && *p;) {
    const char *nl = strchr(p, '\n');
    size_t len = nl ? (size_t)(nl - p) : strlen(p);
    if (len && p[len - 1] == '\r') len--;
    fn(p, len);
    p = nl ? nl + 1 : nullptr;
  }
}

static bool line_is(const char *line, size_t len, const char *name, const char **tab) {
  const char *t = (const char *)memchr(line, '\t', len);
  if (!t || (size_t)(t - line) != strlen(name) || memcmp(line, name, t - line) != 0) return false;
  *tab = t;
  return true;
}

uint32_t movie_progress_find(const char *text, const char *name) {
  uint32_t found = 0;
  each_line(text, [&](const char *line, size_t len) {
    const char *tab;
    if (!line_is(line, len, name, &tab)) return;
    char num[12];
    size_t k = len - (tab + 1 - line);
    if (k >= sizeof(num)) return;
    memcpy(num, tab + 1, k);
    num[k] = '\0';
    found = (uint32_t)strtoul(num, nullptr, 10);
  });
  return found;
}

size_t movie_progress_set(const char *text, const char *name, uint32_t seconds, char *out, size_t out_size) {
  size_t n = 0;
  if (out_size == 0) return 0;
  auto add = [&](const char *s, size_t len) {
    if (n + len + 1 >= out_size) return false;
    memcpy(out + n, s, len);
    n += len;
    out[n++] = '\n';
    return true;
  };
  // 新的放最前面：最近看的总能存下
  if (seconds > 0) {
    char line[300];
    int k = snprintf(line, sizeof(line), "%s\t%u", name, (unsigned)seconds);
    if (k > 0 && (size_t)k < sizeof(line)) add(line, k);
  }
  bool full = false;
  each_line(text, [&](const char *line, size_t len) {
    const char *tab;
    if (full || len == 0 || line_is(line, len, name, &tab)) return;
    if (!add(line, len)) full = true;
  });
  out[n] = '\0';
  return n;
}

void movie_display_name(const char *name, char *out, size_t out_size) {
  if (out_size == 0) return;
  snprintf(out, out_size, "%s", name);
  size_t n = strlen(out);
  if (n > 4 && out[n - 4] == '.' && tolower((unsigned char)out[n - 3]) == 'a' &&
      tolower((unsigned char)out[n - 2]) == 'v' && tolower((unsigned char)out[n - 1]) == 'i')
    out[n - 4] = '\0';
}

void movie_format_time(uint32_t ms, char *out, size_t out_size) {
  uint32_t s = ms / 1000;
  if (s >= 3600) {
    snprintf(out, out_size, "%u:%02u:%02u", (unsigned)(s / 3600), (unsigned)(s / 60 % 60), (unsigned)(s % 60));
  } else {
    snprintf(out, out_size, "%u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
  }
}
