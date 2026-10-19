// 久坐、喝水提醒的单元测试：按时间线模拟主循环（每秒一次），在电脑上运行：pio test -e native
#include <unity.h>

#include "reminder.h"

void setUp() {}
void tearDown() {}

static const uint32_t MIN = 60 * 1000;
static int hm(int h, int m) { return h * 60 + m; }

struct Sim {
  Reminder r;
  uint32_t now = 1000;
  int minutes = hm(10, 0);
  uint32_t ms_in_minute = 0;

  explicit Sim(uint16_t interval, bool on = true) {
    ReminderConfig c;
    c.on = on;
    c.interval_min = interval;
    c.start = hm(9, 0);
    c.end = hm(21, 0);
    r.configure(c);
  }

  // 返回期间提醒了几次
  int run(uint32_t ms, bool hold = false) {
    int fired = 0;
    for (uint32_t t = 0; t < ms; t += 1000) {
      now += 1000;
      ms_in_minute += 1000;
      if (ms_in_minute >= MIN) {
        ms_in_minute = 0;
        minutes = (minutes + 1) % (24 * 60);
      }
      if (r.tick(now, minutes, hold)) fired++;
    }
    return fired;
  }
};

static void test_fires_every_interval() {
  Sim s(45);
  TEST_ASSERT_EQUAL(0, s.run(44 * MIN));
  TEST_ASSERT_EQUAL(1, s.run(2 * MIN));
  TEST_ASSERT_EQUAL(2, s.run(90 * MIN));
}

static void test_off_never_fires() {
  Sim s(45, false);
  TEST_ASSERT_EQUAL(0, s.run(300 * MIN));
  TEST_ASSERT_FALSE(s.r.armed());
}

static void test_only_inside_window() {
  Sim s(60);
  s.minutes = hm(20, 30);
  TEST_ASSERT_EQUAL(0, s.run(60 * MIN));  // 21:00 前没到一小时，之后不提醒
  TEST_ASSERT_FALSE(s.r.armed());
  // 一直跑到第二天 9:00 之后一小时
  s.minutes = hm(8, 59);
  TEST_ASSERT_EQUAL(0, s.run(60 * MIN));
  TEST_ASSERT_EQUAL(1, s.run(2 * MIN));
}

static void test_not_synced() {
  Sim s(10);
  s.minutes = -1;
  int fired = 0;
  for (int i = 0; i < 3600; i++) fired += s.r.tick(s.now += 1000, -1, false);
  TEST_ASSERT_EQUAL(0, fired);
}

static void test_hold_defers_until_released() {
  Sim s(45);
  TEST_ASSERT_EQUAL(0, s.run(60 * MIN, true));  // 专注中到点也不提醒
  TEST_ASSERT_EQUAL(1, s.run(1000));             // 专注一结束就提醒
  TEST_ASSERT_EQUAL(0, s.run(44 * MIN));         // 从提醒时重新计时
}

static void test_restart() {
  Sim s(45);
  s.run(40 * MIN);
  s.r.restart(s.now);
  TEST_ASSERT_EQUAL(0, s.run(44 * MIN));
  TEST_ASSERT_EQUAL(1, s.run(2 * MIN));
}

static void test_same_config_keeps_timer() {
  Sim s(45);
  s.run(40 * MIN);
  ReminderConfig c = s.r.config();
  s.r.configure(c);  // 保存了别的设置，提醒不应重新计时
  TEST_ASSERT_EQUAL(1, s.run(6 * MIN));

  c.interval_min = 30;
  s.r.configure(c);  // 间隔改了，从现在开始重新计时
  TEST_ASSERT_EQUAL(0, s.run(29 * MIN));
  TEST_ASSERT_EQUAL(1, s.run(2 * MIN));
}

static void test_millis_wraparound() {
  Sim s(45);
  s.now = 0xFFFFFFFFu - 10 * MIN;
  TEST_ASSERT_EQUAL(0, s.run(44 * MIN));
  TEST_ASSERT_EQUAL(1, s.run(2 * MIN));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_fires_every_interval);
  RUN_TEST(test_off_never_fires);
  RUN_TEST(test_only_inside_window);
  RUN_TEST(test_not_synced);
  RUN_TEST(test_hold_defers_until_released);
  RUN_TEST(test_restart);
  RUN_TEST(test_same_config_keeps_timer);
  RUN_TEST(test_millis_wraparound);
  return UNITY_END();
}
