// 闹钟和整点报时的单元测试，在电脑上运行：pio test -e native
#include <unity.h>

#include "clock_events.h"

void setUp() {}
void tearDown() {}

static const int D = 20000;  // 随便取一天，星期几由 tick 的参数单独给出
static int hm(int h, int m) { return h * 60 + m; }

static Alarm make_alarm(int time, uint8_t days = 0x3E) {
  AlarmConfig c;
  c.on = true;
  c.time = time;
  c.days = days;
  Alarm a;
  a.configure(c);
  return a;
}

static void test_alarm_fires_once_at_time() {
  Alarm a = make_alarm(hm(7, 30));
  TEST_ASSERT_FALSE(a.tick(D, hm(7, 29), 1));
  TEST_ASSERT_TRUE(a.tick(D, hm(7, 30), 1));
  TEST_ASSERT_FALSE(a.tick(D, hm(7, 30), 1));  // 同一分钟里主循环会调用很多次
  TEST_ASSERT_FALSE(a.tick(D, hm(7, 31), 1));
  TEST_ASSERT_TRUE(a.tick(D + 1, hm(7, 30), 2));  // 第二天照常响
}

static void test_alarm_respects_weekdays() {
  Alarm a = make_alarm(hm(7, 30));  // 默认周一到周五
  TEST_ASSERT_FALSE(a.tick(D, hm(7, 30), 6));  // 周六
  TEST_ASSERT_FALSE(a.tick(D, hm(7, 30), 0));  // 周日
  TEST_ASSERT_TRUE(a.tick(D, hm(7, 30), 5));   // 周五

  Alarm weekend = make_alarm(hm(9, 0), 0x41);
  TEST_ASSERT_TRUE(weekend.tick(D, hm(9, 0), 6));
  TEST_ASSERT_FALSE(weekend.tick(D + 2, hm(9, 0), 1));
}

static void test_alarm_off_or_not_synced() {
  Alarm a;
  TEST_ASSERT_FALSE(a.tick(D, hm(7, 30), 1));  // 默认关闭
  Alarm b = make_alarm(hm(7, 30));
  TEST_ASSERT_FALSE(b.tick(-1, hm(7, 30), 1));
  TEST_ASSERT_FALSE(b.tick(D, -1, 1));
}

static void test_chime_every_hour_once() {
  HourlyChime c;
  TEST_ASSERT_FALSE(c.tick(D, hm(10, 0)));  // 默认关闭
  c.set_on(true);
  TEST_ASSERT_TRUE(c.tick(D, hm(10, 0)));
  TEST_ASSERT_FALSE(c.tick(D, hm(10, 0)));
  TEST_ASSERT_FALSE(c.tick(D, hm(10, 1)));
  TEST_ASSERT_FALSE(c.tick(D, hm(10, 59)));
  TEST_ASSERT_TRUE(c.tick(D, hm(11, 0)));
  TEST_ASSERT_TRUE(c.tick(D + 1, hm(0, 0)));  // 午夜
  TEST_ASSERT_FALSE(c.tick(-1, hm(12, 0)));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_alarm_fires_once_at_time);
  RUN_TEST(test_alarm_respects_weekdays);
  RUN_TEST(test_alarm_off_or_not_synced);
  RUN_TEST(test_chime_every_hour_once);
  return UNITY_END();
}
