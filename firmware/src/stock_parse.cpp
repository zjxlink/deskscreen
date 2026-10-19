#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "stock.h"

// 东方财富的市场编号
static constexpr int kMarketSH = 1;
static constexpr int kMarketSZ = 0;  // 深市和北交所都是 0
static constexpr int kMarketHK = 116;
static constexpr int kMarketsUS[] = {105, 106, 107};  // 纳斯达克、纽交所、美交所

struct Candidate {
  int market;
  StockCode symbol;
};

static bool all_digits(const char *s) {
  if (!*s) return false;
  for (; *s; s++) {
    if (!isdigit((unsigned char)*s)) return false;
  }
  return true;
}

static bool starts_with_ci(const char *s, const char *prefix) {
  for (; *prefix; s++, prefix++) {
    if (tolower((unsigned char)*s) != *prefix) return false;
  }
  return true;
}

static int hk_candidate(const char *digits, Candidate *out) {
  size_t len = strlen(digits);
  if (len == 0 || len > 5) return 0;
  out->market = kMarketHK;
  snprintf(out->symbol, sizeof(out->symbol), "%05d", atoi(digits));
  return 1;
}

// 一个代码可能对应的 (市场, 代码)：A 股、港股各一个，美股要试三个交易所
static int candidates(const char *code, Candidate *out) {
  // 带前缀的写法：sh000001（上证指数，和深市的 000001 平安银行区分）、sz、bj、hk700
  if ((starts_with_ci(code, "sh") || starts_with_ci(code, "sz") || starts_with_ci(code, "bj")) &&
      all_digits(code + 2) && strlen(code + 2) == 6) {
    out->market = starts_with_ci(code, "sh") ? kMarketSH : kMarketSZ;
    strncpy(out->symbol, code + 2, sizeof(out->symbol) - 1);
    out->symbol[sizeof(out->symbol) - 1] = 0;
    return 1;
  }
  if (starts_with_ci(code, "hk") && all_digits(code + 2)) return hk_candidate(code + 2, out);

  if (all_digits(code)) {
    size_t len = strlen(code);
    if (len <= 5) return hk_candidate(code, out);
    if (len != 6) return 0;
    // 6 开头沪市主板，5 开头沪市基金，900 开头沪市 B 股；92/4/8 开头北交所；其余深市
    bool sh = code[0] == '6' || code[0] == '5' || (code[0] == '9' && code[1] != '2');
    out->market = sh ? kMarketSH : kMarketSZ;
    strcpy(out->symbol, code);
    return 1;
  }

  // 其余按美股处理：字母、数字和点（如 BRK.B），至少有一个字母
  bool has_alpha = false;
  for (const char *p = code; *p; p++) {
    if (isalpha((unsigned char)*p)) {
      has_alpha = true;
    } else if (!isdigit((unsigned char)*p) && *p != '.') {
      return 0;
    }
  }
  if (!has_alpha || strlen(code) >= sizeof(StockCode)) return 0;
  int n = 0;
  for (int market : kMarketsUS) {
    out[n].market = market;
    size_t i = 0;
    for (; code[i]; i++) out[n].symbol[i] = (char)toupper((unsigned char)code[i]);
    out[n].symbol[i] = 0;
    n++;
  }
  return n;
}

int stock_split_codes(const char *input, StockCode *codes, int max) {
  int n = 0;
  const char *p = input;
  while (*p && n < max) {
    // 分隔符：ASCII 逗号、分号、空白，以及中文逗号"，"和顿号"、"（UTF-8 三字节）
    if (*p == ',' || *p == ';' || isspace((unsigned char)*p)) {
      p++;
      continue;
    }
    if (!strncmp(p, "\xEF\xBC\x8C", 3) || !strncmp(p, "\xE3\x80\x81", 3)) {
      p += 3;
      continue;
    }
    size_t len = 0;
    while (p[len] && p[len] != ',' && p[len] != ';' && !isspace((unsigned char)p[len]) &&
           strncmp(p + len, "\xEF\xBC\x8C", 3) && strncmp(p + len, "\xE3\x80\x81", 3)) {
      len++;
    }
    if (len < sizeof(StockCode)) {
      memcpy(codes[n], p, len);
      codes[n][len] = 0;
      n++;
    }
    p += len;
  }
  return n;
}

bool stock_build_secids(const StockCode *codes, int n, char *buf, size_t len) {
  size_t used = 0;
  buf[0] = 0;
  for (int i = 0; i < n; i++) {
    Candidate c[3];
    int m = candidates(codes[i], c);
    for (int j = 0; j < m; j++) {
      int w = snprintf(buf + used, len - used, "%s%d.%s", used ? "," : "", c[j].market, c[j].symbol);
      if (w < 0 || (size_t)w >= len - used) return false;
      used += w;
    }
  }
  return true;
}

// 复制 UTF-8 字符串，超长时在字符边界截断，避免半个汉字
static void copy_utf8(char *dst, size_t cap, const char *src) {
  size_t n = strlen(src);
  if (n >= cap) {
    n = cap - 1;
    while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) n--;
  }
  memcpy(dst, src, n);
  dst[n] = 0;
}

static bool is_number(JsonVariant v) {
  return v.is<float>() || v.is<long>();
}

static bool fill_quote(JsonObject e, StockQuote &q) {
  copy_utf8(q.code, sizeof(q.code), e["f12"] | "");
  copy_utf8(q.name, sizeof(q.name), e["f14"] | "");
  // 停牌时价格字段是 "-"
  q.valid = is_number(e["f2"]);
  q.price = e["f2"] | 0.0f;
  q.change = e["f4"] | 0.0f;
  q.change_pct = e["f3"] | 0.0f;
  return true;
}

StockStatus stock_parse(JsonDocument &doc, const StockCode *codes, int n, StockData &out) {
  if ((doc["rc"] | -1) != 0) return StockStatus::Error;

  // diff 一般是数组，个别情况下是 {"0": {...}} 这样的对象
  JsonVariant diff = doc["data"]["diff"];
  JsonObject entries[kMaxStocks * 3];
  int count = 0;
  if (diff.is<JsonArray>()) {
    for (JsonObject e : diff.as<JsonArray>()) {
      if (count < (int)(sizeof(entries) / sizeof(entries[0]))) entries[count++] = e;
    }
  } else if (diff.is<JsonObject>()) {
    for (JsonPair kv : diff.as<JsonObject>()) {
      if (count < (int)(sizeof(entries) / sizeof(entries[0]))) entries[count++] = kv.value().as<JsonObject>();
    }
  }

  out.count = n;
  for (int i = 0; i < n; i++) {
    StockQuote &q = out.quotes[i];
    memset(&q, 0, sizeof(q));
    Candidate c[3];
    int m = candidates(codes[i], c);
    bool found = false;
    for (int j = 0; j < m && !found; j++) {
      for (int k = 0; k < count && !found; k++) {
        const char *sym = entries[k]["f12"] | "";
        if ((entries[k]["f13"] | -1) == c[j].market && !strcasecmp(sym, c[j].symbol)) {
          found = fill_quote(entries[k], q);
        }
      }
    }
    if (!found) {
      // 代码写错或者不存在：用户填的原文当名字显示
      copy_utf8(q.code, sizeof(q.code), codes[i]);
      copy_utf8(q.name, sizeof(q.name), codes[i]);
      q.valid = false;
    }
  }
  return StockStatus::Ok;
}

uint32_t stock_refresh_ms(const struct tm &now) {
  static constexpr uint32_t kFast = 30 * 1000;
  static constexpr uint32_t kSlow = 10 * 60 * 1000;
  int minutes = now.tm_hour * 60 + now.tm_min;
  int wday = now.tm_wday;  // 0 = 周日
  bool weekday = wday >= 1 && wday <= 5;

  // A 股、港股：周一到周五 9:10 ~ 16:15
  if (weekday && minutes >= 9 * 60 + 10 && minutes <= 16 * 60 + 15) return kFast;
  // 美股（北京时间，覆盖夏令时和冬令时）：周一到周五晚上 21:25 开盘，到次日凌晨 5:15
  if (weekday && minutes >= 21 * 60 + 25) return kFast;
  if (wday >= 2 && wday <= 6 && minutes <= 5 * 60 + 15) return kFast;
  return kSlow;
}
