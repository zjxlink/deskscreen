#include "pages.h"

#include <string.h>

const PageInfo kPageInfo[PAGE_COUNT] = {
    {"clock", "时钟"},  {"weather", "天气"},  {"news", "资讯"},  {"github", "GitHub"},  {"stocks", "股票"},
    {"pet", "小橘"},    {"pomodoro", "番茄钟"}, {"chat", "AI 聊天"}, {"album", "相册"},
};

int page_by_key(const char *key) {
  for (int p = 0; p < PAGE_COUNT; p++) {
    if (strcmp(kPageInfo[p].key, key) == 0) return p;
  }
  return -1;
}

uint32_t pages_parse_hidden(const char *list) {
  uint32_t mask = 0;
  char word[16];
  size_t len = 0;
  for (const char *c = list;; c++) {
    if (*c == ',' || *c == '\0') {
      word[len] = '\0';
      int p = page_by_key(word);
      if (p > PAGE_CLOCK) mask |= 1u << p;  // 不认识的名字（旧版本以后删掉的页面）直接忽略
      len = 0;
      if (*c == '\0') break;
    } else if (*c != ' ' && len < sizeof(word) - 1) {
      word[len++] = *c;
    }
  }
  return mask;
}

void pages_format_hidden(uint32_t mask, char *out, size_t n) {
  size_t len = 0;
  if (n) out[0] = '\0';
  for (int p = PAGE_CLOCK + 1; p < PAGE_COUNT; p++) {
    if (!(mask & (1u << p))) continue;
    size_t k = strlen(kPageInfo[p].key);
    if (len + (len ? 1 : 0) + k + 1 > n) break;
    if (len) out[len++] = ',';
    memcpy(out + len, kPageInfo[p].key, k + 1);
    len += k;
  }
}
