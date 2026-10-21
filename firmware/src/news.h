#pragma once

#include <ArduinoJson.h>
#include <stddef.h>
#include <stdint.h>

#include <string>

// 资讯：后台联网任务每 30 分钟拉一轮新闻标题。每个来源（新浪的一个分类、百度热搜、知乎热榜、IT之家、自定义 RSS）
// 是一个「频道」，资讯页上下滑动翻新闻、点频道名换频道，时钟页的要闻条轮流显示各频道的第一条。
// 数据放在 PSRAM（几十 KB），内部 RAM 只用到解析时的一点临时内存

static constexpr int kNewsMaxChannels = 12;  // 内置来源 + 最多 3 条自定义 RSS
static constexpr int kNewsPerChannel = 8;
static constexpr int kNewsMaxRss = 3;

struct NewsItem {
  char title[192];    // 已经去掉 HTML 标签和转义，按 UTF-8 字符截断
  char summary[240];  // 摘要，没有就是空字符串（一屏一条的样式里显示）
  char source[24];    // 媒体名，如「IT之家」；没有时用频道名
  int64_t ts;         // 发布时间（Unix 秒），不知道时为 0（热搜榜就没有）
};

struct NewsChannel {
  char name[24];  // 频道名，显示在资讯页顶栏，如「科技」「百度热搜」
  int count;
  int error;      // 0 表示成功；否则是最近一次失败的原因（HTTP 状态码等），失败时保留上次的新闻
  NewsItem items[kNewsPerChannel];
};

struct NewsData {
  int nch;
  int64_t updated;  // 最近一次有频道拉到新闻的时间（Unix 秒），0 表示还没成功过
  NewsChannel ch[kNewsMaxChannels];
};

// 启动后台任务
void news_begin();

// 设置改了之后调用，立即重新拉一轮
void news_request_refresh();

// 夜间模式时传 true：不再拉新的，天亮后马上拉一轮
void news_set_quiet(bool quiet);

// 有新数据时返回 true 并写入 out，只在主循环中调用（out 放在 PSRAM 里）
bool news_take_update(NewsData &out);

// AI 聊天开始时告诉小橘的最新标题（每个频道前几条），没有新闻时返回空字符串。任何任务都能调用
std::string news_voice_brief();

// 以下在 news_parse.cpp 中实现，不依赖网络，可在电脑上测试

enum class NewsKind : uint8_t { Sina, Baidu, Zhihu, Rss };

struct NewsSource {
  const char *id;    // 设置里存的名字
  const char *name;  // 频道名
  NewsKind kind;
  const char *url;
};

// 内置来源，设置页的勾选项和这里的顺序一样
extern const NewsSource kNewsSources[];
extern const int kNewsSourceCount;

// 按 id 找内置来源，没有返回 nullptr
const NewsSource *news_find_source(const char *id);

// 设置里的来源列表（逗号分隔的 id）里有没有这个 id
bool news_has_source(const char *list, const char *id);

// 新浪滚动新闻 / 百度热搜 / 知乎热榜的 JSON 只保留需要的字段
void news_build_filter(NewsKind kind, JsonDocument &filter);

// 解析过滤后的响应，填好 ch.items 和 ch.count（频道名不动）。格式不对返回 false
bool news_parse_sina(JsonDocument &doc, NewsChannel &ch);
bool news_parse_baidu(JsonDocument &doc, NewsChannel &ch);
bool news_parse_zhihu(JsonDocument &doc, NewsChannel &ch);

// RSS 2.0 / Atom 的流式解析：响应可能有几百 KB（IT之家的 description 是全文），
// 一段一段喂进来，只记住前 kNewsPerChannel 条的标题、时间和摘要，不用把整个响应放在内存里
class RssParser {
 public:
  explicit RssParser(NewsChannel &ch);
  void feed(const char *data, size_t n);
  bool done() const { return ch_.count >= kNewsPerChannel; }  // 够数了，后面的不用再收
  bool ok() const { return ch_.count > 0; }
  const char *feed_title() const { return feed_title_.c_str(); }  // 频道自己的名字（<channel><title>）

 private:
  void on_tag(const std::string &tag);
  void on_text_end();
  NewsChannel &ch_;
  std::string tag_;   // 正在读的标签
  std::string text_;  // 正在收的文字
  std::string feed_title_;
  enum class St : uint8_t { Text, Tag, Cdata, Comment } st_ = St::Text;
  int field_ = 0;     // 正在收哪个字段：0 不收，1 标题，2 时间，3 摘要，4 频道标题
  bool in_item_ = false;
  bool in_cdata_ = false;
  NewsItem cur_;
};

// 去掉 HTML 标签、解码 &amp; &#x4E2D; 等转义、把连续空白合成一个空格、去掉首尾空白，
// 然后按 UTF-8 字符截断到 size（结尾的半个汉字不要）
void news_clean_text(const char *in, char *out, size_t size);

// RFC 822（RSS：Mon, 21 Oct 2026 08:30:00 +0800 / GMT）和 ISO 8601（Atom：2026-10-21T08:30:00+08:00 / Z）转 Unix 秒，
// 认不出返回 0
int64_t news_parse_time(const char *s);

// 「刚刚」「12 分钟前」「3 小时前」「10月19日」，ts 为 0 时写空字符串
void news_age_text(int64_t ts, int64_t now, char *out, size_t size);

// 设置里的自定义 RSS（一行一个地址）拆出来，只要 http:// 或 https:// 开头的，最多 max 条
int news_split_urls(const char *text, std::string *out, int max);

// 小橘的新闻提要：每个频道前 per 条标题，超过 max_bytes 就不再加
std::string news_brief(const NewsData &d, int per, size_t max_bytes);
