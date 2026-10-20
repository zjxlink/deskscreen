#include <string.h>
#include <unity.h>

#include "astro.h"

void setUp() {}
void tearDown() {}

// 月相是一圈，0.99 和 0.01 只差 0.02
static float phase_diff(float a, float b) {
  float d = a - b;
  while (d > 0.5f) d -= 1;
  while (d < -0.5f) d += 1;
  return d;
}
#define ASSERT_PHASE(want, got) TEST_ASSERT_FLOAT_WITHIN(0.03, 0, phase_diff(want, got))

// 用公布的朔望日期核对，允许差一天左右
static void test_phase_known_dates() {
  ASSERT_PHASE(0.0f, moon_phase(2024, 4, 8));    // 新月（北美日全食那天）
  ASSERT_PHASE(0.5f, moon_phase(2024, 4, 24));   // 满月
  ASSERT_PHASE(0.5f, moon_phase(2025, 10, 7));   // 满月（中秋后一天）
  ASSERT_PHASE(0.25f, moon_phase(2026, 1, 26));  // 上弦
  ASSERT_PHASE(0.75f, moon_phase(2026, 6, 8));   // 下弦
  ASSERT_PHASE(0.5f, moon_phase(2026, 9, 26));   // 满月
}

static void test_phase_range() {
  for (int m = 1; m <= 12; m++)
    for (int d = 1; d <= 28; d++) {
      float p = moon_phase(2026, m, d);
      TEST_ASSERT_TRUE(p >= 0 && p < 1);
    }
  // 一天走 1/29.5
  float a = moon_phase(2026, 3, 1), b = moon_phase(2026, 3, 2);
  float step = b - a;
  if (step < 0) step += 1;
  TEST_ASSERT_FLOAT_WITHIN(0.002, 1 / 29.53, step);
  // 跨年也连续
  a = moon_phase(2025, 12, 31);
  b = moon_phase(2026, 1, 1);
  step = b - a;
  if (step < 0) step += 1;
  TEST_ASSERT_FLOAT_WITHIN(0.002, 1 / 29.53, step);
}

static void test_names() {
  TEST_ASSERT_EQUAL_STRING("新月", moon_phase_name(0.0f));
  TEST_ASSERT_EQUAL_STRING("新月", moon_phase_name(0.99f));
  TEST_ASSERT_EQUAL_STRING("蛾眉月", moon_phase_name(0.12f));
  TEST_ASSERT_EQUAL_STRING("上弦月", moon_phase_name(0.25f));
  TEST_ASSERT_EQUAL_STRING("盈凸月", moon_phase_name(0.38f));
  TEST_ASSERT_EQUAL_STRING("满月", moon_phase_name(0.5f));
  TEST_ASSERT_EQUAL_STRING("亏凸月", moon_phase_name(0.62f));
  TEST_ASSERT_EQUAL_STRING("下弦月", moon_phase_name(0.75f));
  TEST_ASSERT_EQUAL_STRING("残月", moon_phase_name(0.88f));
}

static int lit_count(float p) {
  int n = 0;
  for (int y = -10; y <= 10; y++)
    for (int x = -10; x <= 10; x++) n += moon_lit(p, x, y, 10);
  return n;
}

static void test_lit() {
  // 新月全暗，满月全亮，上弦右半边亮，下弦左半边亮
  TEST_ASSERT_EQUAL(0, lit_count(0.0f));
  int full = lit_count(0.5f);
  TEST_ASSERT_TRUE(full > 300);
  TEST_ASSERT_TRUE(moon_lit(0.25f, 5, 0, 10));
  TEST_ASSERT_FALSE(moon_lit(0.25f, -5, 0, 10));
  TEST_ASSERT_TRUE(moon_lit(0.75f, -5, 0, 10));
  TEST_ASSERT_FALSE(moon_lit(0.75f, 5, 0, 10));
  TEST_ASSERT_FALSE(moon_lit(0.5f, 11, 0, 10));  // 圆外面
  // 盈月越来越亮，亏月越来越暗
  TEST_ASSERT_TRUE(lit_count(0.1f) < lit_count(0.2f));
  TEST_ASSERT_TRUE(lit_count(0.3f) < lit_count(0.4f));
  TEST_ASSERT_TRUE(lit_count(0.6f) > lit_count(0.7f));
  TEST_ASSERT_TRUE(lit_count(0.8f) > lit_count(0.9f));
}

static void test_parse_time() {
  TEST_ASSERT_EQUAL(6 * 60 + 4, astro_parse_time("2026-10-20T06:04"));
  TEST_ASSERT_EQUAL(17 * 60 + 21, astro_parse_time("2026-10-20T17:21"));
  TEST_ASSERT_EQUAL(-1, astro_parse_time(nullptr));
  TEST_ASSERT_EQUAL(-1, astro_parse_time("2026-10-20"));
  TEST_ASSERT_EQUAL(-1, astro_parse_time("2026-10-20T25:00"));
  TEST_ASSERT_EQUAL(-1, astro_parse_time("2026-10-20 06:04"));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_phase_known_dates);
  RUN_TEST(test_phase_range);
  RUN_TEST(test_names);
  RUN_TEST(test_lit);
  RUN_TEST(test_parse_time);
  return UNITY_END();
}
