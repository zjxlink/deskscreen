// 资讯的单元测试：在电脑上运行：pio test -e native
#include <string.h>
#include <unity.h>

#include "news.h"

void setUp() {}
void tearDown() {}

static NewsChannel ch;

// 新浪滚动新闻：真实响应里每条有几十个字段，中文是 \uXXXX 转义
static const char kSina[] = R"json({"result":{"status":{"code":0,"msg":"succ"},"lid":2515,"data":[
 {"icons":"","ctime":"1792569193","mtime":"1792569193","media_name":"新浪科技","url":"https:\/\/x",
  "title":"宇树科技发布人形机器人“H2-X 功夫传奇”",
  "intro":" 新浪科技讯 10月21日下午消息，今日 ","images":[{"u":"x"}]},
 {"ctime":"1792572841","media_name":"环球市场播报","title":"A &amp; B","intro":""},
 {"ctime":"1792572841","media_name":"x","title":"   ","intro":"空标题跳过"}
]}})json";

static void test_sina() {
  JsonDocument filter;
  news_build_filter(NewsKind::Sina, filter);
  JsonDocument doc;
  TEST_ASSERT_FALSE(deserializeJson(doc, kSina, DeserializationOption::Filter(filter)));
  TEST_ASSERT_TRUE(doc["result"]["data"][0]["images"].isNull());  // 不要的字段被过滤掉
  TEST_ASSERT_TRUE(news_parse_sina(doc, ch));
  TEST_ASSERT_EQUAL(2, ch.count);
  TEST_ASSERT_EQUAL_STRING("宇树科技发布人形机器人“H2-X 功夫传奇”", ch.items[0].title);
  TEST_ASSERT_EQUAL_STRING("新浪科技讯 10月21日下午消息，今日", ch.items[0].summary);
  TEST_ASSERT_EQUAL_STRING("新浪科技", ch.items[0].source);
  TEST_ASSERT_EQUAL(1792569193LL, ch.items[0].ts);
  TEST_ASSERT_EQUAL_STRING("A & B", ch.items[1].title);

  JsonDocument bad;
  deserializeJson(bad, "{\"result\":{\"status\":{\"code\":1}}}");
  TEST_ASSERT_FALSE(news_parse_sina(bad, ch));
}

// 百度热搜手机版：content[0].content[]，第一条置顶的不要
static const char kBaidu[] = R"json({"success":true,"data":{"cards":[{"component":"tabTextList","content":[{"content":[
 {"isTop":true,"url":"u","word":"置顶宣传"},
 {"isTop":false,"index":1,"url":"u","word":"问界回应车辆起火","hotTag":"0","labelTag":{"day":{"text":"热议"}}},
 {"isTop":false,"index":2,"word":"“神探”李昌钰将安葬于扬州"}
]}]}]}})json";

static void test_baidu() {
  JsonDocument filter;
  news_build_filter(NewsKind::Baidu, filter);
  JsonDocument doc;
  TEST_ASSERT_FALSE(deserializeJson(doc, kBaidu, DeserializationOption::Filter(filter)));
  TEST_ASSERT_TRUE(news_parse_baidu(doc, ch));
  TEST_ASSERT_EQUAL(2, ch.count);
  TEST_ASSERT_EQUAL_STRING("问界回应车辆起火", ch.items[0].title);
  TEST_ASSERT_EQUAL_STRING("“神探”李昌钰将安葬于扬州", ch.items[1].title);
  TEST_ASSERT_EQUAL(0, ch.items[0].ts);

  // 电脑版是一层
  JsonDocument pc;
  deserializeJson(pc, R"({"data":{"cards":[{"content":[{"word":"甲","desc":"说明"},{"query":"乙"}]}]}})");
  TEST_ASSERT_TRUE(news_parse_baidu(pc, ch));
  TEST_ASSERT_EQUAL(2, ch.count);
  TEST_ASSERT_EQUAL_STRING("说明", ch.items[0].summary);
  TEST_ASSERT_EQUAL_STRING("乙", ch.items[1].title);
}

static void test_zhihu() {
  static const char kZhihu[] = R"json({"data":[{"type":"hot_list_feed","target":{"id":1,"title":"医保个人账户跨省共济落地全国，意味着什么？",
    "excerpt":"近日，国家医保局上线医保钱包2.0版。","created":1792510267,"author":{"name":"用户"}}},{"target":{"title":""}}]})json";
  JsonDocument filter;
  news_build_filter(NewsKind::Zhihu, filter);
  JsonDocument doc;
  TEST_ASSERT_FALSE(deserializeJson(doc, kZhihu, DeserializationOption::Filter(filter)));
  TEST_ASSERT_TRUE(doc["data"][0]["target"]["author"].isNull());
  TEST_ASSERT_TRUE(news_parse_zhihu(doc, ch));
  TEST_ASSERT_EQUAL(1, ch.count);
  TEST_ASSERT_EQUAL_STRING("医保个人账户跨省共济落地全国，意味着什么？", ch.items[0].title);
  TEST_ASSERT_EQUAL_STRING("近日，国家医保局上线医保钱包2.0版。", ch.items[0].summary);
}

// IT之家的 RSS：description 是转义过的整篇 HTML
static const char kRss[] =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n<rss version=\"2.0\">\r\n  <channel>\r\n    <title>IT之家</title>\r\n"
    "    <pubDate>Wed, 21 Oct 2026 09:09:25 GMT</pubDate>\r\n    <link>https://www.ithome.com/</link>\r\n"
    "    <item>\r\n      <title>极氪 9X 上市一周年累计交付突破 8.5 万台</title>\r\n"
    "      <description>&lt;p data-vmark=\"af1f\"&gt;IT之家 10 月 21 日消息，&lt;strong&gt;单车成交均价超 53 万元&lt;/strong&gt;。&lt;/p&gt;"
    "&lt;p&gt;&lt;img src=\"https://img.ithome.com/a.jpg\"&gt;&lt;/p&gt;</description>\r\n"
    "      <pubDate>Wed, 21 Oct 2026 09:06:00 GMT</pubDate>\r\n    </item>\r\n"
    "    <item><title><![CDATA[腾讯股价跌破 400 港元 <新低>]]></title><pubDate>Wed, 21 Oct 2026 16:00:00 +0800</pubDate>"
    "<description><![CDATA[<p>截至今日港股收盘</p>]]></description></item>\r\n"
    "    <!-- <item><title>注释里的不算</title></item> -->\r\n"
    "    <item><title></title></item>\r\n"
    "  </channel>\r\n</rss>\r\n";

static void test_rss() {
  // 一个字一个字地喂，和一口气喂，结果一样（网络上的数据随时可能断在标签中间）
  for (size_t step : {(size_t)1, (size_t)7, sizeof(kRss)}) {
    RssParser p(ch);
    size_t n = strlen(kRss);
    for (size_t i = 0; i < n; i += step) p.feed(kRss + i, i + step > n ? n - i : step);
    TEST_ASSERT_TRUE(p.ok());
    TEST_ASSERT_EQUAL_STRING("IT之家", p.feed_title());
    TEST_ASSERT_EQUAL(2, ch.count);
    TEST_ASSERT_EQUAL_STRING("极氪 9X 上市一周年累计交付突破 8.5 万台", ch.items[0].title);
    TEST_ASSERT_EQUAL_STRING("IT之家 10 月 21 日消息， 单车成交均价超 53 万元 。", ch.items[0].summary);
    TEST_ASSERT_EQUAL(1792573560LL, ch.items[0].ts);
    TEST_ASSERT_EQUAL_STRING("腾讯股价跌破 400 港元 <新低>", ch.items[1].title);
    TEST_ASSERT_EQUAL_STRING("截至今日港股收盘", ch.items[1].summary);
    TEST_ASSERT_EQUAL(1792569600LL, ch.items[1].ts);
  }
}

static void test_atom_and_limit() {
  std::string x = "<feed xmlns=\"http://www.w3.org/2005/Atom\"><title type=\"text\">少数派</title><link href=\"https://sspai.com\"/>";
  for (int i = 0; i < 12; i++) {
    x += "<entry><title>第" + std::to_string(i) + "条</title><link href=\"x\"/><updated>2026-10-21T08:30:00Z</updated>"
         "<summary type=\"html\">摘要</summary></entry>";
  }
  x += "</feed>";
  RssParser p(ch);
  p.feed(x.data(), x.size());
  TEST_ASSERT_TRUE(p.done());
  TEST_ASSERT_EQUAL(kNewsPerChannel, ch.count);
  TEST_ASSERT_EQUAL_STRING("少数派", p.feed_title());
  TEST_ASSERT_EQUAL_STRING("第0条", ch.items[0].title);
  TEST_ASSERT_EQUAL_STRING("摘要", ch.items[0].summary);
  TEST_ASSERT_EQUAL(1792571400LL, ch.items[0].ts);

  RssParser bad(ch);
  bad.feed("<html><body>404</body></html>", 29);
  TEST_ASSERT_FALSE(bad.ok());
}

static void test_clean_text() {
  char out[64];
  news_clean_text("  a&nbsp;&lt;3\n\t c &#x4E2D;&#22269; &hellip; &unknown; ", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("a <3 c 中国 … &unknown;", out);
  news_clean_text("<p>段落</p>&lt;br&gt;下一行", out, sizeof(out));  // 转义过的标签也去掉
  TEST_ASSERT_EQUAL_STRING("段落 下一行", out);
  news_clean_text("全角　空格", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("全角 空格", out);
  // 按字截断，不留半个汉字：「中文」6 个字节，放进 6 字节（含结尾 0）只能留一个字
  news_clean_text("中文", out, 6);
  TEST_ASSERT_EQUAL_STRING("中", out);
}

static void test_time() {
  TEST_ASSERT_EQUAL(1792573560LL, news_parse_time("Wed, 21 Oct 2026 09:06:00 GMT"));
  TEST_ASSERT_EQUAL(1792573560LL, news_parse_time("21 Oct 2026 17:06:00 +0800"));
  TEST_ASSERT_EQUAL(1792573560LL, news_parse_time("Wed, 21 Oct 2026 17:06 CST"));  // 国内网站的 CST 是北京时间
  TEST_ASSERT_EQUAL(1792573560LL, news_parse_time("2026-10-21T09:06:00Z"));
  TEST_ASSERT_EQUAL(1792573560LL, news_parse_time("2026-10-21T17:06:00.123+08:00"));
  TEST_ASSERT_EQUAL(1792573560LL, news_parse_time("2026-10-21 17:06:00"));
  TEST_ASSERT_EQUAL(1792573560LL, news_parse_time("1792573560"));
  TEST_ASSERT_EQUAL(0, news_parse_time("昨天"));
  TEST_ASSERT_EQUAL(0, news_parse_time(""));

  char s[24];
  int64_t now = 1792573560;
  news_age_text(now - 30, now, s, sizeof(s));
  TEST_ASSERT_EQUAL_STRING("刚刚", s);
  news_age_text(now - 12 * 60, now, s, sizeof(s));
  TEST_ASSERT_EQUAL_STRING("12 分钟前", s);
  news_age_text(now - 3 * 3600 - 5, now, s, sizeof(s));
  TEST_ASSERT_EQUAL_STRING("3 小时前", s);
  news_age_text(now - 2 * 86400, now, s, sizeof(s));
  TEST_ASSERT_EQUAL_STRING("10月19日", s);
  news_age_text(0, now, s, sizeof(s));
  TEST_ASSERT_EQUAL_STRING("", s);
}

static void test_sources_and_urls() {
  TEST_ASSERT_TRUE(news_has_source("yw,kj,baidu", "kj"));
  TEST_ASSERT_TRUE(news_has_source("yw,kj,baidu", "baidu"));
  TEST_ASSERT_FALSE(news_has_source("yw,kj,baidu", "k"));
  TEST_ASSERT_FALSE(news_has_source("", "yw"));
  TEST_ASSERT_NOT_NULL(news_find_source("ithome"));
  TEST_ASSERT_NULL(news_find_source("weibo"));
  // 设置页的勾选项用 ns_<id> 做名字，id 不能重复
  for (int i = 0; i < kNewsSourceCount; i++) {
    for (int j = i + 1; j < kNewsSourceCount; j++) TEST_ASSERT_NOT_EQUAL(0, strcmp(kNewsSources[i].id, kNewsSources[j].id));
  }

  std::string u[kNewsMaxRss];
  TEST_ASSERT_EQUAL(3, news_split_urls(" https://a.com/rss\r\nftp://x\n http://b.cn/feed, https://c/d\nhttps://e", u, kNewsMaxRss));
  TEST_ASSERT_EQUAL_STRING("https://a.com/rss", u[0].c_str());
  TEST_ASSERT_EQUAL_STRING("http://b.cn/feed", u[1].c_str());
  TEST_ASSERT_EQUAL_STRING("https://c/d", u[2].c_str());
}

static void test_brief() {
  static NewsData d;
  memset(&d, 0, sizeof(d));
  d.nch = 2;
  strcpy(d.ch[0].name, "科技");
  d.ch[0].count = 3;
  strcpy(d.ch[0].items[0].title, "甲");
  strcpy(d.ch[0].items[1].title, "乙");
  strcpy(d.ch[0].items[2].title, "丙");
  strcpy(d.ch[1].name, "空的");
  TEST_ASSERT_EQUAL_STRING("【科技】甲；乙\n", news_brief(d, 2, 1000).c_str());
  TEST_ASSERT_EQUAL_STRING("", news_brief(d, 2, 5).c_str());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_sina);
  RUN_TEST(test_baidu);
  RUN_TEST(test_zhihu);
  RUN_TEST(test_rss);
  RUN_TEST(test_atom_and_limit);
  RUN_TEST(test_clean_text);
  RUN_TEST(test_time);
  RUN_TEST(test_sources_and_urls);
  RUN_TEST(test_brief);
  return UNITY_END();
}
