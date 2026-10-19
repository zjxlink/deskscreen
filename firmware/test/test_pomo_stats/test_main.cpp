// 番茄钟统计的单元测试，在电脑上运行：pio test -e native
#include <unity.h>

#include "pomo_stats.h"

void setUp() {}
void tearDown() {}

static const int D = 20000;  // 随便取一天

static void record_n(PomoStats &s, int day, int n, int goal = 4) {
  for (int i = 0; i < n; i++) s.record(day, goal);
}

static void test_empty() {
  PomoStats s;
  TEST_ASSERT_EQUAL(0, s.count(D));
  TEST_ASSERT_EQUAL(0, s.week_total(D));
  TEST_ASSERT_EQUAL(0, s.streak(D));
  TEST_ASSERT_FALSE(s.any(D));
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

static void test_streak_counts_consecutive_goal_days() {
  PomoStats s;
  record_n(s, D, 4);
  TEST_ASSERT_EQUAL(1, s.streak(D));
  record_n(s, D, 3);  // 超过目标不重复计数
  TEST_ASSERT_EQUAL(1, s.streak(D));
  record_n(s, D + 1, 4);
  record_n(s, D + 2, 4);
  TEST_ASSERT_EQUAL(3, s.streak(D + 2));

  // 第二天还没达标时，昨天的连续天数照样显示；再过一天就断了
  record_n(s, D + 3, 2);
  TEST_ASSERT_EQUAL(3, s.streak(D + 3));
  TEST_ASSERT_EQUAL(0, s.streak(D + 4));

  // 断了以后重新从 1 开始
  record_n(s, D + 5, 4);
  TEST_ASSERT_EQUAL(1, s.streak(D + 5));
}

static void test_streak_survives_beyond_window() {
  PomoStats s;
  for (int d = 0; d < 20; d++) record_n(s, D + d, 2, 2);
  TEST_ASSERT_EQUAL(20, s.streak(D + 19));
}

static void test_goal_lowered_mid_day() {
  PomoStats s;
  record_n(s, D, 3, 8);
  TEST_ASSERT_EQUAL(0, s.streak(D));
  s.record(D, 2);  // 目标改小后，下一个番茄就算当天达标
  TEST_ASSERT_EQUAL(1, s.streak(D));
}

static void test_clock_goes_back() {
  PomoStats s;
  record_n(s, D + 10, 3);
  s.record(D, 4);  // 时间被改回去了，从头开始统计
  TEST_ASSERT_EQUAL(1, s.count(D));
  TEST_ASSERT_EQUAL(0, s.count(D + 10));
}

static void test_not_synced_is_ignored() {
  PomoStats s;
  s.record(-1, 4);
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
  TEST_ASSERT_EQUAL(1, b.streak(D + 1));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_empty);
  RUN_TEST(test_counts_per_day_and_week);
  RUN_TEST(test_long_gap_clears_window);
  RUN_TEST(test_streak_counts_consecutive_goal_days);
  RUN_TEST(test_streak_survives_beyond_window);
  RUN_TEST(test_goal_lowered_mid_day);
  RUN_TEST(test_clock_goes_back);
  RUN_TEST(test_not_synced_is_ignored);
  RUN_TEST(test_save_and_load);
  return UNITY_END();
}
