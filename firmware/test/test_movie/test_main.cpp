#include <string.h>
#include <unity.h>

#include "movie_util.h"

void setUp() {}
void tearDown() {}

static void test_is_file() {
  TEST_ASSERT_TRUE(movie_is_file("千与千寻.avi"));
  TEST_ASSERT_TRUE(movie_is_file("Movie.AVI"));
  TEST_ASSERT_FALSE(movie_is_file(".progress"));
  TEST_ASSERT_FALSE(movie_is_file("._千与千寻.avi"));  // macOS 拷文件时生成的
  TEST_ASSERT_FALSE(movie_is_file("a.mp4"));
  TEST_ASSERT_FALSE(movie_is_file(".avi"));
  TEST_ASSERT_FALSE(movie_is_file("avi"));
  TEST_ASSERT_FALSE(movie_is_file(""));
  TEST_ASSERT_FALSE(movie_is_file(nullptr));
}

static void test_progress() {
  const char *text = "龙猫.avi\t120\r\n千与千寻.avi\t2850\n坏的一行\n千与千寻.avi.bak\t9\n";
  TEST_ASSERT_EQUAL(2850, movie_progress_find(text, "千与千寻.avi"));
  TEST_ASSERT_EQUAL(120, movie_progress_find(text, "龙猫.avi"));
  TEST_ASSERT_EQUAL(0, movie_progress_find(text, "千与千寻"));  // 要整个文件名都一样
  TEST_ASSERT_EQUAL(0, movie_progress_find("", "龙猫.avi"));

  char out[256];
  // 改一部：新的放最前面，旧的那行去掉，别的照抄（坏的行也留着，不丢别人的记录）
  movie_progress_set(text, "千与千寻.avi", 3000, out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("千与千寻.avi\t3000\n龙猫.avi\t120\n坏的一行\n千与千寻.avi.bak\t9\n", out);
  TEST_ASSERT_EQUAL(3000, movie_progress_find(out, "千与千寻.avi"));
  // 0 秒：删掉这一行
  movie_progress_set(out, "龙猫.avi", 0, out + 128, 128);
  TEST_ASSERT_EQUAL(0, movie_progress_find(out + 128, "龙猫.avi"));
  TEST_ASSERT_EQUAL(3000, movie_progress_find(out + 128, "千与千寻.avi"));
  // 放不下时丢掉最后面（最久没看）的
  char small[40];
  movie_progress_set(text, "新片.avi", 5, small, sizeof(small));
  TEST_ASSERT_EQUAL_STRING("新片.avi\t5\n龙猫.avi\t120\n", small);
}

static void test_format_time() {
  char t[16];
  movie_format_time(0, t, sizeof(t));
  TEST_ASSERT_EQUAL_STRING("0:00", t);
  movie_format_time(2850000, t, sizeof(t));
  TEST_ASSERT_EQUAL_STRING("47:30", t);
  movie_format_time(7500999, t, sizeof(t));
  TEST_ASSERT_EQUAL_STRING("2:05:00", t);
}

static void test_display_name() {
  char t[32];
  movie_display_name("千与千寻.avi", t, sizeof(t));
  TEST_ASSERT_EQUAL_STRING("千与千寻", t);
  movie_display_name("Movie.AVI", t, sizeof(t));
  TEST_ASSERT_EQUAL_STRING("Movie", t);
  movie_display_name("a.b", t, sizeof(t));
  TEST_ASSERT_EQUAL_STRING("a.b", t);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_is_file);
  RUN_TEST(test_progress);
  RUN_TEST(test_format_time);
  RUN_TEST(test_display_name);
  return UNITY_END();
}
