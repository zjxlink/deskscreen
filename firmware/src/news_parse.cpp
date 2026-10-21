// 资讯的解析部分：不依赖网络和硬件，可在电脑上测试（pio test -e native）
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "news.h"

// 新浪滚动新闻的分类（lid）：2509 全部、2510 国内、2511 国际、2669 社会、2512 体育、2513 娱乐、2514 军事、2515 科技、2516 财经
#define SINA(lid) "https://feed.mix.sina.com.cn/api/roll/get?pageid=153&num=8&page=1&lid=" lid

const NewsSource kNewsSources[] = {
    {"yw", "要闻", NewsKind::Sina, SINA("2509")},
    {"gn", "国内", NewsKind::Sina, SINA("2510")},
    {"gj", "国际", NewsKind::Sina, SINA("2511")},
    {"sh", "社会", NewsKind::Sina, SINA("2669")},
    {"kj", "科技", NewsKind::Sina, SINA("2515")},
    {"cj", "财经", NewsKind::Sina, SINA("2516")},
    {"ty", "体育", NewsKind::Sina, SINA("2512")},
    {"yl", "娱乐", NewsKind::Sina, SINA("2513")},
    {"js", "军事", NewsKind::Sina, SINA("2514")},
    {"baidu", "百度热搜", NewsKind::Baidu, "https://top.baidu.com/api/board?platform=wise&tab=realtime"},
    {"zhihu", "知乎热榜", NewsKind::Zhihu, "https://api.zhihu.com/topstory/hot-list?limit=10"},
    {"ithome", "IT之家", NewsKind::Rss, "https://www.ithome.com/rss/"},
};
const int kNewsSourceCount = sizeof(kNewsSources) / sizeof(kNewsSources[0]);

const NewsSource *news_find_source(const char *id) {
  for (int i = 0; i < kNewsSourceCount; i++) {
    if (strcmp(kNewsSources[i].id, id) == 0) return &kNewsSources[i];
  }
  return nullptr;
}

bool news_has_source(const char *list, const char *id) {
  size_t n = strlen(id);
  if (n == 0) return false;
  for (const char *p = list; *p;) {
    const char *e = strchr(p, ',');
    size_t len = e ? (size_t)(e - p) : strlen(p);
    if (len == n && strncmp(p, id, n) == 0) return true;
    if (!e) break;
    p = e + 1;
  }
  return false;
}

// ---------- 文字清理 ----------

static void put_utf8(std::string &s, uint32_t cp) {
  if (cp < 0x80) {
    s += (char)cp;
  } else if (cp < 0x800) {
    s += (char)(0xC0 | (cp >> 6));
    s += (char)(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    s += (char)(0xE0 | (cp >> 12));
    s += (char)(0x80 | ((cp >> 6) & 0x3F));
    s += (char)(0x80 | (cp & 0x3F));
  } else if (cp < 0x110000) {
    s += (char)(0xF0 | (cp >> 18));
    s += (char)(0x80 | ((cp >> 12) & 0x3F));
    s += (char)(0x80 | ((cp >> 6) & 0x3F));
    s += (char)(0x80 | (cp & 0x3F));
  }
}

struct Entity {
  const char *name;
  uint32_t cp;
};
static const Entity kEntities[] = {
    {"amp", '&'},      {"lt", '<'},       {"gt", '>'},       {"quot", '"'},     {"apos", '\''},
    {"nbsp", ' '},     {"hellip", 0x2026}, {"mdash", 0x2014}, {"ndash", 0x2013}, {"ldquo", 0x201C},
    {"rdquo", 0x201D}, {"lsquo", 0x2018}, {"rsquo", 0x2019}, {"middot", 0x00B7}, {"times", 0x00D7},
};

// &xxx; 解码，认不出的原样保留
static std::string decode_entities(const std::string &in) {
  std::string out;
  out.reserve(in.size());
  for (size_t i = 0; i < in.size();) {
    if (in[i] == '&') {
      size_t semi = in.find(';', i + 1);
      if (semi != std::string::npos && semi - i <= 10) {
        std::string name = in.substr(i + 1, semi - i - 1);
        uint32_t cp = 0;
        if (name.size() > 1 && name[0] == '#') {
          bool hex = name[1] == 'x' || name[1] == 'X';
          const char *digits = name.c_str() + (hex ? 2 : 1);
          char *end = nullptr;
          unsigned long v = strtoul(digits, &end, hex ? 16 : 10);
          if (*digits && end && *end == 0) cp = (uint32_t)v;
        } else {
          for (const Entity &e : kEntities) {
            if (name == e.name) cp = e.cp;
          }
        }
        if (cp) {
          put_utf8(out, cp);
          i = semi + 1;
          continue;
        }
      }
    }
    out += in[i++];
  }
  return out;
}

// 只认 < 后面紧跟英文字母、/ 或 ! 的标签，标题里的「<新低>」「<3」这种保留
static std::string strip_tags(const std::string &in) {
  std::string out;
  out.reserve(in.size());
  bool in_tag = false;
  for (size_t i = 0; i < in.size(); i++) {
    char c = in[i];
    if (in_tag) {
      if (c == '>') {
        in_tag = false;
        out += ' ';  // <br>、</p> 这类标签两边的字不要粘在一起
      }
    } else if (c == '<' && i + 1 < in.size() && (isalpha((uint8_t)in[i + 1]) || in[i + 1] == '/' || in[i + 1] == '!')) {
      in_tag = true;
    } else {
      out += c;
    }
  }
  return out;
}

// 按 UTF-8 字符截断，保证结果（含结尾的 0）不超过 size 字节
static void copy_utf8(const std::string &s, char *out, size_t size) {
  if (size == 0) return;
  size_t n = s.size() < size - 1 ? s.size() : size - 1;
  if (n < s.size()) {
    while (n > 0 && ((uint8_t)s[n] & 0xC0) == 0x80) n--;  // 退回到这个字的开头
  }
  memcpy(out, s.data(), n);
  out[n] = 0;
}

void news_clean_text(const char *in, char *out, size_t size) {
  // 先去标签再解码；摘要里常见转义过的 HTML（&lt;p&gt;），解码后再去一次
  std::string s = decode_entities(strip_tags(in));
  if (s.find('<') != std::string::npos) s = strip_tags(s);
  std::string r;
  r.reserve(s.size());
  bool space = false;
  for (size_t i = 0; i < s.size(); i++) {
    uint8_t c = (uint8_t)s[i];
    bool ws = c == ' ' || c == '\t' || c == '\n' || c == '\r';
    size_t skip = 0;
    if (c == 0xC2 && i + 1 < s.size() && (uint8_t)s[i + 1] == 0xA0) skip = 1;  // 不换行空格
    if (c == 0xE3 && i + 2 < s.size() && (uint8_t)s[i + 1] == 0x80 && (uint8_t)s[i + 2] == 0x80) skip = 2;  // 全角空格
    if (ws || skip) {
      space = true;
      i += skip;
      continue;
    }
    if (space && !r.empty()) r += ' ';
    space = false;
    r += (char)c;
  }
  copy_utf8(r, out, size);
}

// ---------- 时间 ----------

static int64_t days_from_civil(int y, int m, int d) {
  y -= m <= 2;
  int64_t era = (y >= 0 ? y : y - 399) / 400;
  int yoe = (int)(y - era * 400);
  int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

static int64_t make_time(int y, int mo, int d, int h, int mi, int s, int offset_min) {
  if (y < 1990 || mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || s > 60) return 0;
  return days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + s - offset_min * 60;
}

static constexpr int kBeijingMin = 8 * 60;

int64_t news_parse_time(const char *s) {
  if (!s) return 0;
  while (*s == ' ') s++;
  // 新浪的 ctime 是 Unix 秒的字符串
  if (isdigit((uint8_t)s[0])) {
    char *end = nullptr;
    long long v = strtoll(s, &end, 10);
    if (end && *end == 0 && v > 600000000LL) return v;
  }
  int y, mo, d, h, mi, sec = 0, n = 0;
  // ISO 8601：2026-10-21T08:30:00+08:00、...Z、2026-10-21 08:30:00（没有时区当北京时间）
  if (sscanf(s, "%d-%d-%d%*1[T ]%d:%d%n", &y, &mo, &d, &h, &mi, &n) == 5) {
    const char *p = s + n;
    if (*p == ':') {
      int k = 0;
      if (sscanf(p, ":%d%n", &sec, &k) == 1) p += k;
    }
    if (*p == '.') {
      p++;
      while (isdigit((uint8_t)*p)) p++;
    }
    int ofs = kBeijingMin;
    if (*p == 'Z' || *p == 'z') {
      ofs = 0;
    } else if (*p == '+' || *p == '-') {
      int oh = 0, om = 0;
      if (sscanf(p + 1, "%2d:%2d", &oh, &om) == 2 || sscanf(p + 1, "%2d%2d", &oh, &om) >= 1) {
        ofs = (oh * 60 + om) * (*p == '-' ? -1 : 1);
      }
    }
    return make_time(y, mo, d, h, mi, sec, ofs);
  }
  // RFC 822：Mon, 21 Oct 2026 08:30:00 +0800（星期可以没有）
  const char *p = strchr(s, ',');
  p = p ? p + 1 : s;
  char mon[4] = {0}, zone[8] = {0};
  int k = sscanf(p, "%d %3s %d %d:%d:%d %7s", &d, mon, &y, &h, &mi, &sec, zone);
  if (k < 5) {
    sec = 0;
    k = sscanf(p, "%d %3s %d %d:%d %7s", &d, mon, &y, &h, &mi, zone);
    if (k < 5) return 0;
  }
  static const char *const kMonths = "JanFebMarAprMayJunJulAugSepOctNovDec";
  const char *m = strstr(kMonths, mon);
  if (!m || strlen(mon) != 3 || (m - kMonths) % 3) return 0;
  mo = (int)(m - kMonths) / 3 + 1;
  if (y < 100) y += 2000;
  int ofs = kBeijingMin;  // 没写时区，或者写成 CST（国内的网站一般是指北京时间）
  if (zone[0] == '+' || zone[0] == '-') {
    int v = atoi(zone + 1);
    ofs = (v / 100 * 60 + v % 100) * (zone[0] == '-' ? -1 : 1);
  } else if (!strcmp(zone, "GMT") || !strcmp(zone, "UT") || !strcmp(zone, "UTC") || !strcmp(zone, "Z")) {
    ofs = 0;
  }
  return make_time(y, mo, d, h, mi, sec, ofs);
}

void news_age_text(int64_t ts, int64_t now, char *out, size_t size) {
  if (size == 0) return;
  out[0] = 0;
  if (ts <= 0) return;
  int64_t diff = now - ts;
  if (diff < 60) {
    snprintf(out, size, "刚刚");
  } else if (diff < 3600) {
    snprintf(out, size, "%d 分钟前", (int)(diff / 60));
  } else if (diff < 86400) {
    snprintf(out, size, "%d 小时前", (int)(diff / 3600));
  } else {
    // 按北京时间的日期
    int64_t z = (ts + kBeijingMin * 60) / 86400 + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int doe = (int)(z - era * 146097);
    int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int mp = (5 * doy + 2) / 153;
    int d = doy - (153 * mp + 2) / 5 + 1;
    int m = mp + (mp < 10 ? 3 : -9);
    snprintf(out, size, "%d月%d日", m, d);
  }
}

// ---------- JSON 来源 ----------

void news_build_filter(NewsKind kind, JsonDocument &filter) {
  filter.clear();
  if (kind == NewsKind::Sina) {
    JsonObject it = filter["result"]["data"].add<JsonObject>();
    it["title"] = true;
    it["intro"] = true;
    it["ctime"] = true;
    it["media_name"] = true;
  } else if (kind == NewsKind::Baidu) {
    JsonObject card = filter["data"]["cards"].add<JsonObject>();
    JsonObject it = card["content"].add<JsonObject>();
    it["word"] = true;
    it["query"] = true;
    it["desc"] = true;
    it["isTop"] = true;
    it["content"].add<JsonObject>()["word"] = true;  // 手机版是两层：content[0].content[]
    it["content"][0]["query"] = true;
    it["content"][0]["desc"] = true;
    it["content"][0]["isTop"] = true;
  } else if (kind == NewsKind::Zhihu) {
    JsonObject t = filter["data"].add<JsonObject>()["target"].to<JsonObject>();
    t["title"] = true;
    t["excerpt"] = true;
  }
}

static void add_item(NewsChannel &ch, const char *title, const char *summary, const char *source, int64_t ts) {
  if (ch.count >= kNewsPerChannel) return;
  NewsItem &it = ch.items[ch.count];
  news_clean_text(title ? title : "", it.title, sizeof(it.title));
  if (!it.title[0]) return;
  news_clean_text(summary ? summary : "", it.summary, sizeof(it.summary));
  news_clean_text(source ? source : "", it.source, sizeof(it.source));
  it.ts = ts;
  ch.count++;
}

bool news_parse_sina(JsonDocument &doc, NewsChannel &ch) {
  JsonArray arr = doc["result"]["data"];
  if (arr.isNull()) return false;
  ch.count = 0;
  for (JsonObject o : arr) {
    const char *ctime = o["ctime"] | "";
    int64_t ts = o["ctime"].is<long long>() ? o["ctime"].as<long long>() : news_parse_time(ctime);
    add_item(ch, o["title"] | "", o["intro"] | "", o["media_name"] | "", ts);
  }
  return ch.count > 0;
}

bool news_parse_baidu(JsonDocument &doc, NewsChannel &ch) {
  JsonArray content = doc["data"]["cards"][0]["content"];
  if (content.isNull()) return false;
  // 电脑版是一层 content[]，手机版（platform=wise）是 content[0].content[]
  if (content[0]["word"].isNull() && content[0]["query"].isNull() && !content[0]["content"].isNull()) {
    content = content[0]["content"];
  }
  ch.count = 0;
  for (JsonObject o : content) {
    if (o["isTop"] | false) continue;  // 置顶的那条一般是宣传，不算热搜
    const char *w = o["word"] | (o["query"] | "");
    add_item(ch, w, o["desc"] | "", "百度热搜", 0);
  }
  return ch.count > 0;
}

bool news_parse_zhihu(JsonDocument &doc, NewsChannel &ch) {
  JsonArray arr = doc["data"];
  if (arr.isNull()) return false;
  ch.count = 0;
  for (JsonObject o : arr) {
    JsonObject t = o["target"];
    // created 是提问的时间，不是上热榜的时间，不显示
    add_item(ch, t["title"] | "", t["excerpt"] | "", "知乎", 0);
  }
  return ch.count > 0;
}

// ---------- RSS / Atom ----------

static constexpr size_t kMaxFieldBytes = 2048;  // 摘要只要开头一段，太长的不再收

RssParser::RssParser(NewsChannel &ch) : ch_(ch) {
  ch_.count = 0;
  memset(&cur_, 0, sizeof(cur_));
}

void RssParser::feed(const char *data, size_t n) {
  for (size_t i = 0; i < n && !done(); i++) {
    char c = data[i];
    switch (st_) {
      case St::Text:
        if (c == '<') {
          st_ = St::Tag;
          tag_.clear();
        } else if (field_ && text_.size() < kMaxFieldBytes) {
          text_ += c;
        }
        break;
      case St::Tag:
        tag_ += c;
        if (tag_ == "![CDATA[") {
          st_ = St::Cdata;
        } else if (tag_ == "!--") {
          st_ = St::Comment;
        } else if (c == '>') {
          tag_.pop_back();
          st_ = St::Text;
          on_tag(tag_);
        } else if (tag_.size() > 256) {
          st_ = St::Text;  // 不是正常的标签，丢掉
        }
        break;
      case St::Cdata:
        // CDATA 里的东西原样收下，直到 ]]>
        if (field_ && text_.size() < kMaxFieldBytes) text_ += c;
        tag_ += c;
        if (tag_.size() >= 3 && tag_.compare(tag_.size() - 3, 3, "]]>") == 0) {
          if (field_ && text_.size() >= 3 && text_.compare(text_.size() - 3, 3, "]]>") == 0) text_.resize(text_.size() - 3);
          st_ = St::Text;
        }
        if (tag_.size() > 16) tag_.erase(0, tag_.size() - 3);
        break;
      case St::Comment:
        tag_ += c;
        if (tag_.size() >= 3 && tag_.compare(tag_.size() - 3, 3, "-->") == 0) st_ = St::Text;
        if (tag_.size() > 16) tag_.erase(0, tag_.size() - 3);
        break;
    }
  }
}

void RssParser::on_tag(const std::string &raw) {
  if (raw.empty() || raw[0] == '?' || raw[0] == '!') return;
  bool closing = raw[0] == '/';
  bool self_closing = raw.back() == '/';
  size_t start = closing ? 1 : 0;
  size_t end = raw.find_first_of(" \t\r\n/", start);
  std::string name = raw.substr(start, end == std::string::npos ? std::string::npos : end - start);
  for (char &ch : name) ch = (char)tolower((uint8_t)ch);
  if (self_closing && !closing) return;

  if (closing) {
    if (field_) on_text_end();
    if (name == "item" || name == "entry") {
      if (in_item_ && cur_.title[0] && ch_.count < kNewsPerChannel) ch_.items[ch_.count++] = cur_;
      in_item_ = false;
    }
    return;
  }
  if (name == "item" || name == "entry") {
    if (field_) on_text_end();
    in_item_ = true;
    memset(&cur_, 0, sizeof(cur_));
    return;
  }
  if (field_) return;  // 字段里面又套了标签（不该有），继续收
  int f = 0;
  if (name == "title") {
    f = in_item_ ? 1 : (feed_title_.empty() ? 4 : 0);
  } else if (in_item_ && (name == "pubdate" || name == "published" || name == "updated" || name == "dc:date")) {
    f = cur_.ts ? 0 : 2;
  } else if (in_item_ && (name == "description" || name == "summary")) {
    f = cur_.summary[0] ? 0 : 3;
  }
  if (f) {
    field_ = f;
    text_.clear();
  }
}

void RssParser::on_text_end() {
  switch (field_) {
    case 1:
      news_clean_text(text_.c_str(), cur_.title, sizeof(cur_.title));
      break;
    case 2:
      cur_.ts = news_parse_time(text_.c_str());
      break;
    case 3:
      news_clean_text(text_.c_str(), cur_.summary, sizeof(cur_.summary));
      break;
    case 4: {
      char buf[sizeof(ch_.name)];
      news_clean_text(text_.c_str(), buf, sizeof(buf));
      feed_title_ = buf;
      break;
    }
  }
  field_ = 0;
  text_.clear();
}

// ---------- 其他 ----------

int news_split_urls(const char *text, std::string *out, int max) {
  int n = 0;
  const char *p = text;
  while (*p && n < max) {
    while (*p && (isspace((uint8_t)*p) || *p == ',')) p++;
    const char *b = p;
    while (*p && !isspace((uint8_t)*p) && *p != ',') p++;
    std::string u(b, p - b);
    if (u.rfind("http://", 0) == 0 || u.rfind("https://", 0) == 0) out[n++] = u;
  }
  return n;
}

std::string news_brief(const NewsData &d, int per, size_t max_bytes) {
  std::string out;
  for (int c = 0; c < d.nch; c++) {
    const NewsChannel &ch = d.ch[c];
    if (ch.count == 0) continue;
    std::string line = std::string("【") + ch.name + "】";
    for (int i = 0; i < ch.count && i < per; i++) {
      if (i) line += "；";
      line += ch.items[i].title;
    }
    line += "\n";
    if (out.size() + line.size() > max_bytes) break;
    out += line;
  }
  return out;
}
