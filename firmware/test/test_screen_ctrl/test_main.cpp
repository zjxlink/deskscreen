// 屏幕亮度和页面切换的场景测试：按时间线模拟主循环（每 5ms 一次），在电脑上运行：pio test -e native
#include <unity.h>

#include "screen_ctrl.h"

void setUp() {}
void tearDown() {}

static int hm(int h, int m) { return h * 60 + m; }

// 模拟设备：记录时间，按主循环的节奏调用 tick
struct Sim {
  ScreenCtrl ctrl;
  uint32_t now = 1000;
  int minutes = hm(12, 0);
  int min_seen = 999, max_seen = -1;

  explicit Sim(const ScreenConfig &cfg) {
    ctrl.configure(cfg);
    ctrl.tick(now, minutes);
    ctrl.start(now);
  }

  void run(uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += 5) {
      now += 5;
      ctrl.tick(now, minutes);
      int b = ctrl.brightness();
      if (b < min_seen) min_seen = b;
      if (b > max_seen) max_seen = b;
    }
  }

  // 跑到亮度稳定为止，返回用了多少毫秒
  uint32_t settle(uint32_t limit = 5000) {
    uint32_t start = now;
    int last = -1, same = 0;
    while (now - start < limit && same < 20) {
      run(5);
      same = ctrl.brightness() == last ? same + 1 : 0;
      last = ctrl.brightness();
    }
    return now - start;
  }

  Page take_page() {
    Page p;
    bool animate;
    ctrl.take_page_change(p, animate);
    return ctrl.page();
  }
};

static ScreenConfig night_cfg() {
  ScreenConfig c;
  c.day_brightness = 180;
  c.night_on = true;
  c.night_start = hm(23, 0);
  c.night_end = hm(7, 0);
  c.night_brightness = 10;
  c.page_seconds = 15;
  c.page_enabled[PAGE_GITHUB] = true;
  c.page_enabled[PAGE_STOCKS] = true;
  return c;
}

static void test_boot_uses_day_brightness_without_fade() {
  ScreenCtrl c;
  c.configure(night_cfg());
  c.tick(0, -1);  // 还没对时
  TEST_ASSERT_EQUAL(180, c.brightness());
  TEST_ASSERT_FALSE(c.night());

  // 开机时已经是夜里：直接用夜间亮度，不从白天亮度渐变
  ScreenCtrl n;
  n.configure(night_cfg());
  n.tick(0, hm(2, 0));
  TEST_ASSERT_EQUAL(10, n.brightness());
}

static void test_fades_down_at_night_start() {
  Sim s(night_cfg());
  s.minutes = hm(22, 59);
  s.run(1000);
  TEST_ASSERT_EQUAL(180, s.ctrl.brightness());

  s.minutes = hm(23, 0);
  s.run(30);
  int after_one_step = s.ctrl.brightness();
  TEST_ASSERT_TRUE(after_one_step < 180 && after_one_step > 10);  // 不是一下跳到底

  uint32_t took = s.settle();
  TEST_ASSERT_EQUAL(10, s.ctrl.brightness());
  // 渐变大约 1 秒，留些余量
  TEST_ASSERT_TRUE(took > 500 && took < 2000);
  TEST_ASSERT_TRUE(s.ctrl.dimmed());
}

static void test_tap_at_night_only_wakes() {
  Sim s(night_cfg());
  s.minutes = hm(23, 30);
  s.settle();
  s.take_page();

  // 第一下：只点亮，不切页
  TEST_ASSERT_FALSE(s.ctrl.on_input(s.now));
  Page p;
  bool animate;
  TEST_ASSERT_FALSE(s.ctrl.take_page_change(p, animate));
  s.settle();
  TEST_ASSERT_EQUAL(180, s.ctrl.brightness());

  // 点亮期间：正常切页
  TEST_ASSERT_TRUE(s.ctrl.on_input(s.now));
  TEST_ASSERT_EQUAL(PAGE_GITHUB, s.take_page());
}

static void test_wake_expires_and_returns_to_clock() {
  Sim s(night_cfg());
  s.minutes = hm(1, 0);
  s.settle();
  s.ctrl.on_input(s.now);  // 点亮
  s.run(1000);
  s.ctrl.on_input(s.now);  // 切到 GitHub 页，并顺延 30 秒
  TEST_ASSERT_EQUAL(PAGE_GITHUB, s.take_page());

  // 29 秒后仍是正常亮度，也不会被拉回时钟页
  s.run(29000);
  TEST_ASSERT_EQUAL(180, s.ctrl.brightness());
  TEST_ASSERT_EQUAL(PAGE_GITHUB, s.ctrl.page());

  // 超过 30 秒：回到时钟页并重新变暗
  s.run(2000);
  TEST_ASSERT_EQUAL(PAGE_CLOCK, s.take_page());
  s.settle();
  TEST_ASSERT_EQUAL(10, s.ctrl.brightness());
}

static void test_no_auto_rotation_at_night() {
  Sim s(night_cfg());
  s.minutes = hm(3, 0);
  s.run(60000);  // 一分钟，远超 15 秒轮播间隔
  TEST_ASSERT_EQUAL(PAGE_CLOCK, s.ctrl.page());
}

static void test_morning_restores_day() {
  Sim s(night_cfg());
  s.minutes = hm(6, 59);
  s.settle();
  TEST_ASSERT_EQUAL(10, s.ctrl.brightness());
  s.minutes = hm(7, 0);
  s.settle();
  TEST_ASSERT_EQUAL(180, s.ctrl.brightness());
  TEST_ASSERT_FALSE(s.ctrl.night());
  // 白天轮播恢复
  s.run(16000);
  TEST_ASSERT_EQUAL(PAGE_GITHUB, s.ctrl.page());
}

static void test_day_rotation_skips_unconfigured_pages() {
  ScreenConfig c = night_cfg();
  c.page_enabled[PAGE_GITHUB] = false;  // 没填 Token
  Sim s(c);
  s.take_page();
  s.run(15500);
  TEST_ASSERT_EQUAL(PAGE_STOCKS, s.take_page());
  s.run(15500);
  TEST_ASSERT_EQUAL(PAGE_CLOCK, s.take_page());

  // 手动切换仍然经过 GitHub 页（能看到"请填写 Token"的提示）
  s.ctrl.on_input(s.now);
  TEST_ASSERT_EQUAL(PAGE_GITHUB, s.take_page());
}

static void test_rotation_off_and_single_page() {
  ScreenConfig c = night_cfg();
  c.page_seconds = 0;
  Sim s(c);
  s.run(60000);
  TEST_ASSERT_EQUAL(PAGE_CLOCK, s.ctrl.page());

  ScreenConfig only_clock = night_cfg();
  only_clock.page_enabled[PAGE_GITHUB] = false;
  only_clock.page_enabled[PAGE_STOCKS] = false;
  Sim t(only_clock);
  t.take_page();
  t.run(60000);
  Page p;
  bool animate;
  TEST_ASSERT_FALSE(t.ctrl.take_page_change(p, animate));  // 原地不动，也不重复刷新
}

static void test_night_brightness_zero_turns_off() {
  ScreenConfig c = night_cfg();
  c.night_brightness = 0;
  Sim s(c);
  s.minutes = hm(23, 0);
  s.settle();
  TEST_ASSERT_EQUAL(0, s.ctrl.brightness());
  TEST_ASSERT_FALSE(s.ctrl.on_input(s.now));  // 黑屏时点一下只点亮
  s.settle();
  TEST_ASSERT_EQUAL(180, s.ctrl.brightness());
}

static void test_night_mode_disabled() {
  ScreenConfig c = night_cfg();
  c.night_on = false;
  Sim s(c);
  s.minutes = hm(2, 0);
  s.run(3000);
  TEST_ASSERT_EQUAL(180, s.ctrl.brightness());
  TEST_ASSERT_EQUAL(180, s.min_seen);
}

static void test_input_ignored_before_start() {
  ScreenCtrl c;
  c.configure(night_cfg());
  c.tick(0, hm(12, 0));
  TEST_ASSERT_FALSE(c.on_input(10));  // 开机提示页期间
  Page p;
  bool animate;
  TEST_ASSERT_FALSE(c.take_page_change(p, animate));
  c.start(20);
  TEST_ASSERT_TRUE(c.take_page_change(p, animate));  // 提示页结束，切到时钟页
  TEST_ASSERT_EQUAL(PAGE_CLOCK, p);
  TEST_ASSERT_FALSE(animate);
}

static void test_settings_change_fades() {
  Sim s(night_cfg());
  ScreenConfig c = night_cfg();
  c.day_brightness = 60;
  s.ctrl.configure(c);
  s.run(30);
  TEST_ASSERT_TRUE(s.ctrl.brightness() < 180 && s.ctrl.brightness() > 60);
  s.settle();
  TEST_ASSERT_EQUAL(60, s.ctrl.brightness());
}

static void test_millis_wraparound() {
  // millis() 约 49.7 天回绕一次，回绕前后点亮仍应正常计时
  Sim s(night_cfg());
  s.now = 0xFFFFFFFFu - 10000;
  s.minutes = hm(2, 0);
  s.settle();
  s.ctrl.on_input(s.now);
  s.run(20000);  // 跨过回绕点
  TEST_ASSERT_EQUAL(180, s.ctrl.brightness());
  s.run(15000);
  s.settle();
  TEST_ASSERT_EQUAL(10, s.ctrl.brightness());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_boot_uses_day_brightness_without_fade);
  RUN_TEST(test_fades_down_at_night_start);
  RUN_TEST(test_tap_at_night_only_wakes);
  RUN_TEST(test_wake_expires_and_returns_to_clock);
  RUN_TEST(test_no_auto_rotation_at_night);
  RUN_TEST(test_morning_restores_day);
  RUN_TEST(test_day_rotation_skips_unconfigured_pages);
  RUN_TEST(test_rotation_off_and_single_page);
  RUN_TEST(test_night_brightness_zero_turns_off);
  RUN_TEST(test_night_mode_disabled);
  RUN_TEST(test_input_ignored_before_start);
  RUN_TEST(test_settings_change_fades);
  RUN_TEST(test_millis_wraparound);
  return UNITY_END();
}
