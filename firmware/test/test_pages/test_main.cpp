// 隐藏页面列表的解析和格式化，在电脑上运行：pio test -e native
#include <unity.h>

#include "pages.h"

void setUp() {}
void tearDown() {}

static void test_parse() {
  TEST_ASSERT_EQUAL_UINT32(0, pages_parse_hidden(""));
  TEST_ASSERT_EQUAL_UINT32(1u << PAGE_GITHUB | 1u << PAGE_ALBUM, pages_parse_hidden("github,album"));
  TEST_ASSERT_EQUAL_UINT32(1u << PAGE_STOCKS, pages_parse_hidden(" stocks , "));
  // 时钟页不能隐藏；不认识的名字（以后删掉的页面）忽略
  TEST_ASSERT_EQUAL_UINT32(1u << PAGE_PET, pages_parse_hidden("clock,radio,pet,averyveryverylongname"));
}

static void test_format_round_trip() {
  char buf[96];
  uint32_t mask = 1u << PAGE_WEATHER | 1u << PAGE_CHAT | 1u << PAGE_CLOCK;
  pages_format_hidden(mask, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("weather,chat", buf);
  TEST_ASSERT_EQUAL_UINT32(mask & ~1u, pages_parse_hidden(buf));
  pages_format_hidden(0, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("", buf);
  pages_format_hidden(1u << PAGE_WEATHER | 1u << PAGE_GITHUB, buf, 12);  // 放不下就少写，不写半个名字
  TEST_ASSERT_EQUAL_STRING("weather", buf);
}

static void test_keys_unique() {
  for (int a = 0; a < PAGE_COUNT; a++) {
    TEST_ASSERT_EQUAL(a, page_by_key(kPageInfo[a].key));
    TEST_ASSERT_NOT_NULL(kPageInfo[a].name);
  }
  TEST_ASSERT_EQUAL(-1, page_by_key("nope"));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_parse);
  RUN_TEST(test_format_round_trip);
  RUN_TEST(test_keys_unique);
  return UNITY_END();
}
