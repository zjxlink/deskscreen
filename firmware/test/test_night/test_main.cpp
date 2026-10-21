// 夜间模式时间计算的单元测试，在电脑上运行：pio test -e native
#include <unity.h>

#include "night.h"

void setUp() {}
void tearDown() {}

static int hm(int h, int m) { return h * 60 + m; }

static void test_same_day_range() {
  // 13:00 ~ 14:00 午休
  TEST_ASSERT_FALSE(night_is_active(hm(12, 59), hm(13, 0), hm(14, 0)));
  TEST_ASSERT_TRUE(night_is_active(hm(13, 0), hm(13, 0), hm(14, 0)));
  TEST_ASSERT_TRUE(night_is_active(hm(13, 59), hm(13, 0), hm(14, 0)));
  TEST_ASSERT_FALSE(night_is_active(hm(14, 0), hm(13, 0), hm(14, 0)));
}

static void test_across_midnight() {
  int start = hm(23, 0), end = hm(7, 0);
  TEST_ASSERT_FALSE(night_is_active(hm(22, 59), start, end));
  TEST_ASSERT_TRUE(night_is_active(hm(23, 0), start, end));
  TEST_ASSERT_TRUE(night_is_active(hm(0, 0), start, end));
  TEST_ASSERT_TRUE(night_is_active(hm(6, 59), start, end));
  TEST_ASSERT_FALSE(night_is_active(hm(7, 0), start, end));
  TEST_ASSERT_FALSE(night_is_active(hm(12, 0), start, end));
}

static void test_equal_means_disabled() {
  TEST_ASSERT_FALSE(night_is_active(hm(8, 0), hm(8, 0), hm(8, 0)));
  TEST_ASSERT_FALSE(night_is_active(hm(3, 0), hm(8, 0), hm(8, 0)));
}

static void test_parse_hhmm() {
  TEST_ASSERT_EQUAL(hm(23, 0), night_parse_hhmm("23:00"));
  TEST_ASSERT_EQUAL(hm(7, 5), night_parse_hhmm("07:05"));
  TEST_ASSERT_EQUAL(hm(7, 5), night_parse_hhmm("7:05"));
  TEST_ASSERT_EQUAL(hm(0, 0), night_parse_hhmm("00:00"));
  TEST_ASSERT_EQUAL(hm(23, 59), night_parse_hhmm("23:59:30"));  // 浏览器可能带秒
  TEST_ASSERT_EQUAL(-1, night_parse_hhmm("24:00"));
  TEST_ASSERT_EQUAL(-1, night_parse_hhmm("12:60"));
  TEST_ASSERT_EQUAL(-1, night_parse_hhmm("12:5"));
  TEST_ASSERT_EQUAL(-1, night_parse_hhmm("1200"));
  TEST_ASSERT_EQUAL(-1, night_parse_hhmm(""));
  TEST_ASSERT_EQUAL(-1, night_parse_hhmm(nullptr));
}

static void test_format_hhmm() {
  char buf[8];
  night_format_hhmm(hm(7, 5), buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("07:05", buf);
  night_format_hhmm(hm(23, 0), buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("23:00", buf);
}

static void test_theme_fixed() {
  TEST_ASSERT_FALSE(theme_is_light(THEME_DARK, hm(12, 0), hm(6, 0), hm(18, 0)));
  TEST_ASSERT_TRUE(theme_is_light(THEME_LIGHT, hm(23, 0), hm(6, 0), hm(18, 0)));
  TEST_ASSERT_TRUE(theme_is_light(THEME_LIGHT, -1, -1, -1));  // 没对时也照样浅色
}

static void test_theme_auto() {
  // 日出到日落浅色，日落那一分钟起换深色
  TEST_ASSERT_FALSE(theme_is_light(THEME_AUTO, hm(6, 4), hm(6, 5), hm(17, 28)));
  TEST_ASSERT_TRUE(theme_is_light(THEME_AUTO, hm(6, 5), hm(6, 5), hm(17, 28)));
  TEST_ASSERT_TRUE(theme_is_light(THEME_AUTO, hm(17, 27), hm(6, 5), hm(17, 28)));
  TEST_ASSERT_FALSE(theme_is_light(THEME_AUTO, hm(17, 28), hm(6, 5), hm(17, 28)));
  // 还没拿到天气预报时按 7:00～19:00
  TEST_ASSERT_FALSE(theme_is_light(THEME_AUTO, hm(6, 59), -1, -1));
  TEST_ASSERT_TRUE(theme_is_light(THEME_AUTO, hm(18, 59), -1, -1));
  TEST_ASSERT_FALSE(theme_is_light(THEME_AUTO, hm(19, 0), -1, -1));
  // 还没对时先深色
  TEST_ASSERT_FALSE(theme_is_light(THEME_AUTO, -1, hm(6, 5), hm(17, 28)));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_same_day_range);
  RUN_TEST(test_across_midnight);
  RUN_TEST(test_equal_means_disabled);
  RUN_TEST(test_parse_hhmm);
  RUN_TEST(test_format_hhmm);
  RUN_TEST(test_theme_fixed);
  RUN_TEST(test_theme_auto);
  return UNITY_END();
}
