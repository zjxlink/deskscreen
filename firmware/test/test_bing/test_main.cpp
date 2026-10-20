// 必应每日一图的单元测试：在电脑上运行：pio test -e native
#include <string.h>
#include <unity.h>

#include "bing.h"

void setUp() {}
void tearDown() {}

static const char kSample[] = R"json({"images":[
 {"startdate":"20261019","fullstartdate":"202610191600","enddate":"20261020",
  "url":"/th?id=OHR.AuroraChurch_ZH-CN1234567890_1920x1080.jpg&rf=LaDigue_1920x1080.jpg&pid=hp",
  "urlbase":"/th?id=OHR.AuroraChurch_ZH-CN1234567890",
  "copyright":"冰岛维克附近的教堂 (© Lukas/Getty Images)","title":"极光下的教堂","hsh":"abc"},
 {"startdate":"20261018","urlbase":"/th?id=OHR.Bad\"Path","copyright":"x","title":"y"},
 {"startdate":"2026101","urlbase":"/th?id=OHR.Short_ZH-CN1","copyright":"x","title":"y"},
 {"startdate":"20261017","urlbase":"/th?id=OHR.Fox_ZH-CN42","copyright":"雪地里的赤狐，加拿大（© Jim/Minden Pictures）","title":""},
 {"startdate":"20261017","urlbase":"/th?id=OHR.Dup_ZH-CN1","copyright":"重复","title":"重复"}
],"tooltips":{"loading":"正在加载..."}})json";

static void test_parse() {
  JsonDocument filter;
  bing_build_filter(filter);
  JsonDocument doc;
  TEST_ASSERT_FALSE(deserializeJson(doc, kSample, DeserializationOption::Filter(filter)));
  TEST_ASSERT_TRUE(doc["tooltips"].isNull());  // 不要的字段被过滤掉
  BingImage imgs[kBingApiImages];
  int n = bing_parse(doc, imgs, kBingApiImages);
  TEST_ASSERT_EQUAL(2, n);
  TEST_ASSERT_EQUAL_STRING("20261019", imgs[0].date);
  TEST_ASSERT_EQUAL_STRING("/th?id=OHR.AuroraChurch_ZH-CN1234567890_320x240.jpg", imgs[0].path);
  TEST_ASSERT_EQUAL_STRING("极光下的教堂 · 冰岛维克附近的教堂", imgs[0].caption);
  TEST_ASSERT_EQUAL_STRING("20261017", imgs[1].date);
  TEST_ASSERT_EQUAL_STRING("雪地里的赤狐，加拿大", imgs[1].caption);

  TEST_ASSERT_EQUAL(1, bing_parse(doc, imgs, 1));
  JsonDocument empty;
  deserializeJson(empty, "{\"market\":{}}");
  TEST_ASSERT_EQUAL(0, bing_parse(empty, imgs, kBingApiImages));
}

static void test_caption() {
  char out[160];
  bing_caption("标题", "标题 (© 某人)", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("标题", out);  // 标题和地点一样时只显示一次
  bing_caption(nullptr, nullptr, out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("", out);
  bing_caption("  只有标题 ", "(© 某人)", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("只有标题", out);
  bing_caption("", "没有版权信息的地点", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("没有版权信息的地点", out);
  // 截断时不切开汉字：每个汉字 3 字节，10 字节的缓冲区放得下 3 个
  char small[10];
  bing_caption("一二三四五", "", small, sizeof(small));
  TEST_ASSERT_EQUAL_STRING("一二三", small);
  bing_caption("ab", "一二三四 (© x)", small, sizeof(small));
  TEST_ASSERT_EQUAL_STRING("ab · 一", small);  // 2 + 4 + 3 字节
}

static void test_names() {
  TEST_ASSERT_TRUE(bing_is_date("20261020"));
  TEST_ASSERT_FALSE(bing_is_date("2026102"));
  TEST_ASSERT_FALSE(bing_is_date("../../x1"));
  TEST_ASSERT_FALSE(bing_is_date(nullptr));
  TEST_ASSERT_TRUE(bing_is_file("20261020.jpg"));
  TEST_ASSERT_FALSE(bing_is_file("20261020.txt"));
  TEST_ASSERT_FALSE(bing_is_file("IMG_0001.jpg"));
  TEST_ASSERT_FALSE(bing_is_file("20261020.jpg.tmp"));
}

static void test_next_source() {
  TEST_ASSERT_EQUAL(-1, album_next_source(-1, false, false));
  TEST_ASSERT_EQUAL(0, album_next_source(-1, true, false));
  TEST_ASSERT_EQUAL(0, album_next_source(0, true, false));
  TEST_ASSERT_EQUAL(1, album_next_source(1, false, true));
  // 两边都有时交替，第一张先放自己的照片
  TEST_ASSERT_EQUAL(0, album_next_source(-1, true, true));
  TEST_ASSERT_EQUAL(1, album_next_source(0, true, true));
  TEST_ASSERT_EQUAL(0, album_next_source(1, true, true));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_parse);
  RUN_TEST(test_caption);
  RUN_TEST(test_names);
  RUN_TEST(test_next_source);
  return UNITY_END();
}
