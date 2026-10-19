// 屏幕控制芯片看门狗的单元测试，在电脑上运行：pio test -e native
#include <unity.h>

#include "panel_watch.h"

void setUp() {}
void tearDown() {}

static const uint32_t OK = 0x55089C;     // 16 位色、横屏、已退出睡眠、显示打开
static const uint32_t RESET = 0x660008;  // 芯片复位后的状态
static const uint32_t GARBLED = 0x66089C;  // 像素格式被改掉（花屏）

static void test_steady_reading_never_reinits() {
  PanelWatch w;
  w.begin(OK, OK, 0);
  TEST_ASSERT_TRUE(w.readable());
  for (uint32_t t = 2000; t < 3600000; t += 2000) {
    TEST_ASSERT_TRUE(w.due(t));
    TEST_ASSERT_FALSE(w.check(OK, t));
  }
}

static void test_due_every_two_seconds() {
  PanelWatch w;
  w.begin(OK, OK, 1000);
  TEST_ASSERT_FALSE(w.due(2999));
  TEST_ASSERT_TRUE(w.due(3000));
  w.check(OK, 3000);
  TEST_ASSERT_FALSE(w.due(4000));
}

static void test_reset_detected_after_two_bad_reads() {
  PanelWatch w;
  w.begin(OK, OK, 0);
  TEST_ASSERT_FALSE(w.check(RESET, 2000));
  TEST_ASSERT_TRUE(w.check(RESET, 4000));
  w.recovered(OK, 4200);
  TEST_ASSERT_EQUAL_UINT32(1, w.recoveries());
  TEST_ASSERT_TRUE(w.readable());
  TEST_ASSERT_FALSE(w.check(OK, 6200));
}

static void test_partial_corruption_detected() {
  PanelWatch w;
  w.begin(OK, OK, 0);
  TEST_ASSERT_FALSE(w.check(GARBLED, 2000));
  TEST_ASSERT_TRUE(w.check(GARBLED, 4000));
}

static void test_single_glitched_read_ignored() {
  PanelWatch w;
  w.begin(OK, OK, 0);
  TEST_ASSERT_FALSE(w.check(0x00, 2000));
  TEST_ASSERT_FALSE(w.check(OK, 4000));
  TEST_ASSERT_FALSE(w.check(0x00, 6000));
  TEST_ASSERT_FALSE(w.check(OK, 8000));
}

static void test_unreadable_panel_reinits_blindly() {
  PanelWatch w;
  w.begin(0xFFFFFF, 0xFFFFFF, 0);
  TEST_ASSERT_FALSE(w.readable());
  TEST_ASSERT_FALSE(w.check(0, PanelWatch::kBlindReinitMs - 1));
  TEST_ASSERT_TRUE(w.check(0, PanelWatch::kBlindReinitMs));
  w.recovered(0, PanelWatch::kBlindReinitMs);
  TEST_ASSERT_EQUAL_UINT32(0, w.recoveries());
  TEST_ASSERT_FALSE(w.check(0, PanelWatch::kBlindReinitMs + 2000));
  TEST_ASSERT_TRUE(w.check(0, 2 * PanelWatch::kBlindReinitMs));
}

static void test_unstable_or_zero_baseline_is_unreadable() {
  PanelWatch a;
  a.begin(OK, 0x55089E, 0);
  TEST_ASSERT_FALSE(a.readable());
  PanelWatch b;
  b.begin(0x00, 0x00, 0);
  TEST_ASSERT_FALSE(b.readable());
}

static void test_gives_up_when_reinit_never_matches() {
  PanelWatch w;
  w.begin(OK, OK, 0);
  uint32_t t = 0;
  for (int i = 0; i < PanelWatch::kMaxFailedRecoveries; i++) {
    TEST_ASSERT_TRUE(w.readable());
    w.check(RESET, t += 2000);
    TEST_ASSERT_TRUE(w.check(RESET, t += 2000));
    w.recovered(RESET, t);
  }
  TEST_ASSERT_FALSE(w.readable());
  // 退回盲目模式：之后不再每两秒初始化一次
  TEST_ASSERT_FALSE(w.check(RESET, t + 2000));
}

static void test_good_recovery_resets_failure_count() {
  PanelWatch w;
  w.begin(OK, OK, 0);
  uint32_t t = 0;
  for (int i = 0; i < 10; i++) {
    w.check(RESET, t += 2000);
    TEST_ASSERT_TRUE(w.check(RESET, t += 2000));
    w.recovered(i % 2 ? OK : RESET, t);
  }
  TEST_ASSERT_TRUE(w.readable());
  TEST_ASSERT_EQUAL_UINT32(10, w.recoveries());
}

static void test_millis_wraparound() {
  PanelWatch w;
  w.begin(OK, OK, 0xFFFFFC18);  // millis() 再过 1 秒回绕
  TEST_ASSERT_FALSE(w.due(0xFFFFFFFF));
  TEST_ASSERT_TRUE(w.due(1000));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_steady_reading_never_reinits);
  RUN_TEST(test_due_every_two_seconds);
  RUN_TEST(test_reset_detected_after_two_bad_reads);
  RUN_TEST(test_partial_corruption_detected);
  RUN_TEST(test_single_glitched_read_ignored);
  RUN_TEST(test_unreadable_panel_reinits_blindly);
  RUN_TEST(test_unstable_or_zero_baseline_is_unreadable);
  RUN_TEST(test_gives_up_when_reinit_never_matches);
  RUN_TEST(test_good_recovery_resets_failure_count);
  RUN_TEST(test_millis_wraparound);
  return UNITY_END();
}
