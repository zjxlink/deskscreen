// 番茄钟完成数统计的单元测试，在电脑上运行：pio test -e native
#include <unity.h>

#include "pomo_stats.h"

void setUp() {}
void tearDown() {}

static const int D = 20000;  // 随便取一天

static void record_n(PomoStats &s, int day, int n) {
  for (int i = 0; i < n; i++) s.record(day);
}

static void test_empty() {
  PomoStats s;
  TEST_ASSERT_EQUAL(0, s.count(D));
  TEST_ASSERT_EQUAL(0, s.week_total(D));
}

static void test_counts_per_day_and_week() {
  PomoStats s;
  record_n(s, D, 3);
  record_n(s, D + 2, 5);  // 中间空一天
  TEST_ASSERT_EQUAL(3, s.count(D));
  TEST_ASSERT_EQUAL(0, s.count(D + 1));
  TEST_ASSERT_EQUAL(5, s.count(D + 2));
  TEST_ASSERT_EQUAL(8, s.week_total(D + 2));
  // 一周后第一天的记录滑出窗口
  TEST_ASSERT_EQUAL(8, s.week_total(D + 6));
  TEST_ASSERT_EQUAL(5, s.week_total(D + 7));
  TEST_ASSERT_EQUAL(0, s.week_total(D + 9));
}

static void test_long_gap_clears_window() {
  PomoStats s;
  record_n(s, D, 4);
  record_n(s, D + 30, 1);
  TEST_ASSERT_EQUAL(0, s.count(D));
  TEST_ASSERT_EQUAL(1, s.count(D + 30));
  TEST_ASSERT_EQUAL(1, s.week_total(D + 30));
}

static void test_clock_goes_back() {
  PomoStats s;
  record_n(s, D + 10, 3);
  s.record(D);  // 时间被改回去了，从头开始统计
  TEST_ASSERT_EQUAL(1, s.count(D));
  TEST_ASSERT_EQUAL(0, s.count(D + 10));
}

static void test_not_synced_is_ignored() {
  PomoStats s;
  s.record(-1);
  TEST_ASSERT_EQUAL(-1, s.data().last_day);
}

static void test_save_and_load() {
  PomoStats a;
  record_n(a, D, 4);
  record_n(a, D + 1, 2);
  PomoStats b;
  b.load(a.data());
  TEST_ASSERT_EQUAL(4, b.count(D));
  TEST_ASSERT_EQUAL(2, b.count(D + 1));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_empty);
  RUN_TEST(test_counts_per_day_and_week);
  RUN_TEST(test_long_gap_clears_window);
  RUN_TEST(test_clock_goes_back);
  RUN_TEST(test_not_synced_is_ignored);
  RUN_TEST(test_save_and_load);
  return UNITY_END();
}
