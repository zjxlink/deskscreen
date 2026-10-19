// 番茄钟状态机的单元测试，在电脑上运行：pio test -e native
#include <unity.h>

#include "pomodoro.h"

void setUp() {}
void tearDown() {}

static const uint32_t MIN = 60 * 1000;

// 模拟主循环每秒 tick 一次，返回期间结束了几个阶段
static int run(Pomodoro &p, uint32_t &now, uint32_t ms, int day = 100) {
  int events = 0;
  for (uint32_t t = 0; t < ms; t += 1000) {
    now += 1000;
    if (p.tick(now, day)) events++;
  }
  return events;
}

static void test_idle_shows_full_focus() {
  Pomodoro p;
  TEST_ASSERT_EQUAL(PomoPhase::Idle, p.phase());
  TEST_ASSERT_FALSE(p.running());
  TEST_ASSERT_EQUAL_UINT32(25 * MIN, p.remaining_ms(0));
  uint32_t now = 0;
  TEST_ASSERT_EQUAL(0, run(p, now, 60 * MIN));  // 不开始就不会动
}

static void test_focus_then_short_break() {
  Pomodoro p;
  uint32_t now = 1000;
  p.toggle(now);
  TEST_ASSERT_EQUAL(PomoPhase::Focus, p.phase());
  TEST_ASSERT_TRUE(p.running());
  run(p, now, 10 * MIN);
  TEST_ASSERT_EQUAL_UINT32(15 * MIN, p.remaining_ms(now));

  TEST_ASSERT_EQUAL(1, run(p, now, 15 * MIN));
  TEST_ASSERT_EQUAL(PomoPhase::ShortBreak, p.phase());
  TEST_ASSERT_TRUE(p.running());  // 休息自动开始
  TEST_ASSERT_EQUAL(1, p.done_today());
  TEST_ASSERT_EQUAL(1, p.cycle_done());
}

static void test_break_end_waits_for_user() {
  Pomodoro p;
  uint32_t now = 0;
  p.toggle(now);
  run(p, now, 25 * MIN);
  TEST_ASSERT_EQUAL(1, run(p, now, 5 * MIN));
  TEST_ASSERT_EQUAL(PomoPhase::Focus, p.phase());
  TEST_ASSERT_FALSE(p.running());  // 下一个专注等用户点开始
  TEST_ASSERT_EQUAL_UINT32(25 * MIN, p.remaining_ms(now));
  TEST_ASSERT_EQUAL(0, run(p, now, 60 * MIN));
  TEST_ASSERT_EQUAL_UINT32(25 * MIN, p.remaining_ms(now));

  p.toggle(now);
  TEST_ASSERT_TRUE(p.running());
}

static void test_long_break_every_four() {
  Pomodoro p;
  uint32_t now = 0;
  for (int i = 0; i < 3; i++) {
    p.toggle(now);                 // 开始专注
    run(p, now, 25 * MIN);         // 专注结束 → 短休息
    TEST_ASSERT_EQUAL(PomoPhase::ShortBreak, p.phase());
    run(p, now, 5 * MIN);          // 休息结束 → 等待
  }
  p.toggle(now);
  run(p, now, 25 * MIN);
  TEST_ASSERT_EQUAL(PomoPhase::LongBreak, p.phase());
  TEST_ASSERT_EQUAL(4, p.done_today());
  TEST_ASSERT_EQUAL_UINT32(15 * MIN, p.remaining_ms(now));

  run(p, now, 15 * MIN);
  TEST_ASSERT_EQUAL(PomoPhase::Focus, p.phase());
  TEST_ASSERT_EQUAL(0, p.cycle_done());  // 长休息后新一轮
  TEST_ASSERT_EQUAL(4, p.done_today());
}

static void test_pause_freezes_time() {
  Pomodoro p;
  uint32_t now = 0;
  p.toggle(now);
  run(p, now, 5 * MIN);
  p.toggle(now);  // 暂停
  TEST_ASSERT_FALSE(p.running());
  run(p, now, 60 * MIN);
  TEST_ASSERT_EQUAL_UINT32(20 * MIN, p.remaining_ms(now));
  p.toggle(now);  // 继续
  TEST_ASSERT_EQUAL(1, run(p, now, 20 * MIN));
  TEST_ASSERT_EQUAL(PomoPhase::ShortBreak, p.phase());
}

static void test_skip_and_reset() {
  Pomodoro p;
  uint32_t now = 0;
  p.toggle(now);
  run(p, now, 3 * MIN);
  p.skip(now);  // 跳过专注：不计入完成
  TEST_ASSERT_EQUAL(PomoPhase::ShortBreak, p.phase());
  TEST_ASSERT_EQUAL(0, p.done_today());
  p.skip(now);  // 跳过休息
  TEST_ASSERT_EQUAL(PomoPhase::Focus, p.phase());
  TEST_ASSERT_FALSE(p.running());

  p.toggle(now);
  run(p, now, 25 * MIN);
  TEST_ASSERT_EQUAL(1, p.cycle_done());
  p.reset();
  TEST_ASSERT_EQUAL(PomoPhase::Idle, p.phase());
  TEST_ASSERT_EQUAL(0, p.cycle_done());
  TEST_ASSERT_EQUAL(1, p.done_today());  // 今日完成数保留
}

static void test_new_day_resets_count() {
  Pomodoro p;
  uint32_t now = 0;
  p.toggle(now);
  run(p, now, 25 * MIN, -1);  // 还没对时
  TEST_ASSERT_EQUAL(1, p.done_today());
  p.tick(now, 100);  // 第一次对时：不清零
  TEST_ASSERT_EQUAL(1, p.done_today());
  p.tick(now, 101);  // 过了午夜
  TEST_ASSERT_EQUAL(0, p.done_today());
}

static void test_custom_durations() {
  Pomodoro p;
  PomoConfig c;
  c.focus_min = 50;
  c.short_min = 10;
  c.long_min = 30;
  c.long_every = 2;
  p.configure(c);
  uint32_t now = 0;
  TEST_ASSERT_EQUAL_UINT32(50 * MIN, p.remaining_ms(now));
  p.toggle(now);
  run(p, now, 50 * MIN);
  TEST_ASSERT_EQUAL_UINT32(10 * MIN, p.remaining_ms(now));
  run(p, now, 10 * MIN);
  p.toggle(now);
  run(p, now, 50 * MIN);
  TEST_ASSERT_EQUAL(PomoPhase::LongBreak, p.phase());
  TEST_ASSERT_EQUAL_UINT32(30 * MIN, p.remaining_ms(now));
}

static void test_millis_wraparound() {
  Pomodoro p;
  uint32_t now = 0xFFFFFFFFu - 10 * MIN;
  p.toggle(now);
  TEST_ASSERT_EQUAL(0, run(p, now, 24 * MIN));  // 跨过回绕点也不会提前结束
  TEST_ASSERT_EQUAL_UINT32(1 * MIN, p.remaining_ms(now));
  TEST_ASSERT_EQUAL(1, run(p, now, 1 * MIN));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_idle_shows_full_focus);
  RUN_TEST(test_focus_then_short_break);
  RUN_TEST(test_break_end_waits_for_user);
  RUN_TEST(test_long_break_every_four);
  RUN_TEST(test_pause_freezes_time);
  RUN_TEST(test_skip_and_reset);
  RUN_TEST(test_new_day_resets_count);
  RUN_TEST(test_custom_durations);
  RUN_TEST(test_millis_wraparound);
  return UNITY_END();
}
