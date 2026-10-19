#include "night.h"

#include <ctype.h>
#include <stdio.h>

bool night_is_active(int now, int start, int end) {
  if (start == end) return false;
  if (start < end) return now >= start && now < end;
  return now >= start || now < end;  // 跨午夜
}

int night_parse_hhmm(const char *s) {
  if (!s || !isdigit((unsigned char)s[0])) return -1;
  int h = 0, m = 0, i = 0;
  while (isdigit((unsigned char)s[i]) && i < 2) h = h * 10 + (s[i++] - '0');
  if (s[i++] != ':') return -1;
  if (!isdigit((unsigned char)s[i]) || !isdigit((unsigned char)s[i + 1])) return -1;
  m = (s[i] - '0') * 10 + (s[i + 1] - '0');
  // 浏览器的 time 输入框可能带秒（"23:00:00"），秒忽略
  if (s[i + 2] != 0 && s[i + 2] != ':') return -1;
  if (h > 23 || m > 59) return -1;
  return h * 60 + m;
}

void night_format_hhmm(int minutes, char *buf, size_t len) {
  snprintf(buf, len, "%02d:%02d", minutes / 60 % 24, minutes % 60);
}
