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
  TEST_ASSERT_EQUAL(PAGE_WEATHER, s.take_page());
}

static void test_wake_expires_and_returns_to_clock() {
  Sim s(night_cfg());
  s.minutes = hm(1, 0);
  s.settle();
  s.ctrl.on_input(s.now);  // 点亮
  s.run(1000);
  s.ctrl.on_input(s.now);  // 切到天气页，并顺延 30 秒
  TEST_ASSERT_EQUAL(PAGE_WEATHER, s.take_page());

  // 29 秒后仍是正常亮度，也不会被拉回时钟页
  s.run(29000);
  TEST_ASSERT_EQUAL(180, s.ctrl.brightness());
  TEST_ASSERT_EQUAL(PAGE_WEATHER, s.ctrl.page());

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
  c.page_unavailable = 1u << PAGE_NEWS;  // 没选新闻来源：资讯页手动切换也跳过
  Sim s(c);
  s.take_page();
  s.run(15500);
  TEST_ASSERT_EQUAL(PAGE_STOCKS, s.take_page());
  s.run(15500);
  TEST_ASSERT_EQUAL(PAGE_CLOCK, s.take_page());

  // 手动切换仍然经过 GitHub 页（能看到"请填写 Token"的提示）
  s.ctrl.on_input(s.now);
  TEST_ASSERT_EQUAL(PAGE_WEATHER, s.take_page());
  s.run(1000);
  s.ctrl.on_input(s.now);
  TEST_ASSERT_EQUAL(PAGE_GITHUB, s.take_page());
}

static void test_unavailable_page_skipped() {
  ScreenConfig c = night_cfg();
  c.page_enabled[PAGE_CHAT] = true;
  c.page_unavailable = 1u << PAGE_CHAT | 1u << PAGE_ALBUM;  // 没有麦克风、没有 TF 卡槽
  Sim s(c);
  s.ctrl.go_to(PAGE_POMODORO, s.now);
  s.take_page();
  s.ctrl.on_input(s.now);
  TEST_ASSERT_EQUAL(PAGE_CLOCK, s.take_page());  // 手动切换也跳过
  s.ctrl.go_to(PAGE_POMODORO, s.now);
  s.take_page();
  s.run(15500);
  TEST_ASSERT_EQUAL(PAGE_CLOCK, s.take_page());  // 自动轮播也跳过
}

static void test_hidden_page_skipped() {
  ScreenConfig c = night_cfg();
  c.page_enabled[PAGE_WEATHER] = true;
  c.page_enabled[PAGE_PET] = true;
  c.page_hidden = 1u << PAGE_WEATHER | 1u << PAGE_NEWS | 1u << PAGE_GITHUB | 1u << PAGE_CLOCK;  // 时钟页的位不算数
  Sim s(c);
  TEST_ASSERT_FALSE(s.ctrl.skipped(PAGE_CLOCK));
  TEST_ASSERT_TRUE(s.ctrl.skipped(PAGE_WEATHER));
  s.ctrl.on_input(s.now);
  TEST_ASSERT_EQUAL(PAGE_STOCKS, s.take_page());  // 手动切换跳过天气、资讯和 GitHub
  s.ctrl.on_input(s.now, true);
  TEST_ASSERT_EQUAL(PAGE_CLOCK, s.take_page());
  s.run(15500);
  TEST_ASSERT_EQUAL(PAGE_STOCKS, s.take_page());  // 自动轮播也跳过天气和 GitHub
  // 番茄钟、唤醒词这类直接跳过去的不受影响
  s.ctrl.go_to(PAGE_GITHUB, s.now);
  TEST_ASSERT_EQUAL(PAGE_GITHUB, s.take_page());
  s.ctrl.on_input(s.now);
  TEST_ASSERT_EQUAL(PAGE_STOCKS, s.take_page());
}

static void test_all_hidden_stays_on_clock() {
  ScreenConfig c = night_cfg();
  c.page_hidden = 0xFFFFFFFF;
  Sim s(c);
  s.take_page();
  s.ctrl.on_input(s.now);
  TEST_ASSERT_EQUAL(PAGE_CLOCK, s.take_page());
  s.run(15500);
  TEST_ASSERT_EQUAL(PAGE_CLOCK, s.take_page());
}

static void test_back_goes_to_previous_page() {
  ScreenConfig c = night_cfg();
  c.page_unavailable = 1u << PAGE_ALBUM;  // 没有 TF 卡槽
  Sim s(c);
  Page p;
  bool animate, back = false;
  s.ctrl.on_input(s.now, true);  // 时钟页往回翻：跳过没有的相册页，到 AI 聊天页
  TEST_ASSERT_TRUE(s.ctrl.take_page_change(p, animate, &back));
  TEST_ASSERT_EQUAL(PAGE_CHAT, p);
  TEST_ASSERT_TRUE(animate);
  TEST_ASSERT_TRUE(back);
  s.ctrl.on_input(s.now, true);
  TEST_ASSERT_TRUE(s.ctrl.take_page_change(p, animate, &back));
  TEST_ASSERT_EQUAL(PAGE_POMODORO, p);
  s.ctrl.on_input(s.now);  // 再往前翻回来，动画方向也跟着变
  TEST_ASSERT_TRUE(s.ctrl.take_page_change(p, animate, &back));
  TEST_ASSERT_EQUAL(PAGE_CHAT, p);
  TEST_ASSERT_FALSE(back);
}

static void test_stay_delays_rotation() {
  ScreenConfig c = night_cfg();
  c.page_enabled[PAGE_CHAT] = true;
  Sim s(c);
  s.ctrl.go_to(PAGE_CHAT, s.now);
  s.take_page();
  // 聊天中每秒说一次「还在用」，过了好几个轮播间隔也不切走
  for (int i = 0; i < 40; i++) {
    s.run(1000);
    s.ctrl.stay(s.now);
  }
  TEST_ASSERT_EQUAL(PAGE_CHAT, s.take_page());
  s.run(15500);
  TEST_ASSERT_EQUAL(PAGE_CLOCK, s.take_page());  // 聊完了照常轮播
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

static void test_pinned_page_stays_and_returns() {
  Sim s(night_cfg());
  s.take_page();
  s.ctrl.go_to(PAGE_POMODORO, s.now);
  s.ctrl.pin(PAGE_POMODORO);
  TEST_ASSERT_EQUAL(PAGE_POMODORO, s.take_page());
  s.run(60000);  // 固定期间不轮播
  TEST_ASSERT_EQUAL(PAGE_POMODORO, s.ctrl.page());

  // 手动切走，过一个轮播间隔（15 秒）自动回来
  s.ctrl.on_input(s.now);
  TEST_ASSERT_EQUAL(PAGE_CHAT, s.take_page());  // 番茄钟后面是 AI 聊天页
  s.run(14000);
  TEST_ASSERT_EQUAL(PAGE_CHAT, s.ctrl.page());
  s.run(2000);
  TEST_ASSERT_EQUAL(PAGE_POMODORO, s.take_page());

  // 取消固定后恢复正常轮播
  s.ctrl.unpin();
  s.run(16000);
  TEST_ASSERT_EQUAL(PAGE_CLOCK, s.take_page());
}

static void test_pinned_page_wins_at_night() {
  Sim s(night_cfg());
  s.ctrl.go_to(PAGE_POMODORO, s.now);
  s.ctrl.pin(PAGE_POMODORO);
  s.minutes = hm(23, 30);
  s.run(60000);
  TEST_ASSERT_EQUAL(PAGE_POMODORO, s.ctrl.page());  // 夜里也不回时钟页
  TEST_ASSERT_EQUAL(10, s.ctrl.brightness());       // 但照样调暗
}

static void test_wake_for_reminder() {
  Sim s(night_cfg());
  s.minutes = hm(1, 0);
  s.settle();
  TEST_ASSERT_EQUAL(10, s.ctrl.brightness());
  s.ctrl.wake(s.now);  // 番茄钟到点提醒
  s.settle();
  TEST_ASSERT_EQUAL(180, s.ctrl.brightness());
  s.run(31000);
  s.settle();
  TEST_ASSERT_EQUAL(10, s.ctrl.brightness());
}

static ScreenConfig away_cfg() {
  ScreenConfig c = night_cfg();
  c.away_off = true;
  c.away_min = 5;
  return c;
}

// 雷达：没人 5 分钟熄屏，有人回来再亮
static void test_away_turns_off_and_back_on() {
  Sim s(away_cfg());
  s.ctrl.set_presence(true, s.now);
  s.settle();
  TEST_ASSERT_EQUAL(180, s.ctrl.brightness());
  s.ctrl.set_presence(false, s.now);
  s.run(4 * 60000);
  s.settle();
  TEST_ASSERT_EQUAL(180, s.ctrl.brightness());  // 不到 5 分钟
  TEST_ASSERT_FALSE(s.ctrl.away());
  s.run(61000);
  s.settle();
  TEST_ASSERT_TRUE(s.ctrl.away());
  TEST_ASSERT_EQUAL(0, s.ctrl.brightness());
  s.ctrl.set_presence(true, s.now);
  s.run(5);
  TEST_ASSERT_FALSE(s.ctrl.away());
  s.settle();
  TEST_ASSERT_EQUAL(180, s.ctrl.brightness());
}

// 坐着偶尔动一下就一直亮着
static void test_occasional_motion_keeps_screen_on() {
  Sim s(away_cfg());
  for (int i = 0; i < 10; i++) {
    s.ctrl.set_presence(true, s.now);
    s.run(5);
    s.ctrl.set_presence(false, s.now);
    s.run(4 * 60000);
  }
  TEST_ASSERT_FALSE(s.ctrl.away());
  TEST_ASSERT_EQUAL(180, s.ctrl.brightness());
}

// 熄屏时第一下只点亮，不切页面；之后重新计时
static void test_tap_while_away_only_wakes() {
  Sim s(away_cfg());
  s.ctrl.set_presence(false, s.now);
  s.run(6 * 60000);
  s.take_page();
  TEST_ASSERT_TRUE(s.ctrl.dimmed());
  Page before = s.ctrl.page();
  TEST_ASSERT_FALSE(s.ctrl.on_input(s.now));
  TEST_ASSERT_EQUAL(before, s.take_page());
  s.settle();
  TEST_ASSERT_EQUAL(180, s.ctrl.brightness());
  s.run(4 * 60000);
  TEST_ASSERT_FALSE(s.ctrl.away());
  s.run(61000);
  TEST_ASSERT_TRUE(s.ctrl.away());
}

// 夜里有人回来只恢复到夜间亮度
static void test_away_at_night_returns_to_night_brightness() {
  Sim s(away_cfg());
  s.minutes = hm(1, 0);
  s.ctrl.set_presence(false, s.now);
  s.run(6 * 60000);
  s.settle();
  TEST_ASSERT_EQUAL(0, s.ctrl.brightness());
  s.ctrl.set_presence(true, s.now);
  s.settle();
  TEST_ASSERT_EQUAL(10, s.ctrl.brightness());
}

// 提醒会点亮屏幕 30 秒，即使没人
static void test_wake_overrides_away() {
  Sim s(away_cfg());
  s.ctrl.set_presence(false, s.now);
  s.run(6 * 60000);
  s.settle();
  TEST_ASSERT_EQUAL(0, s.ctrl.brightness());
  s.minutes = hm(12, 0);
  s.ctrl.wake(s.now);
  s.settle();
  TEST_ASSERT_EQUAL(180, s.ctrl.brightness());
  s.run(31000);
  s.settle();
  TEST_ASSERT_EQUAL(0, s.ctrl.brightness());
}

// 关了这个功能，或者没有雷达（从没调用 set_presence），屏幕一直亮
static void test_away_disabled_or_no_radar() {
  ScreenConfig c = away_cfg();
  c.away_off = false;
  Sim off(c);
  off.ctrl.set_presence(false, off.now);
  off.run(10 * 60000);
  off.settle();
  TEST_ASSERT_EQUAL(180, off.ctrl.brightness());

  Sim no_radar(away_cfg());
  no_radar.run(10 * 60000);
  no_radar.settle();
  TEST_ASSERT_EQUAL(180, no_radar.ctrl.brightness());
}

// 快捷设置页上的点击：点亮屏幕但不切换页面
static void test_activity_wakes_without_page_change() {
  Sim s(away_cfg());
  s.ctrl.set_presence(false, s.now);
  s.run(6 * 60000);
  s.settle();
  s.take_page();
  TEST_ASSERT_EQUAL(0, s.ctrl.brightness());
  Page before = s.ctrl.page();
  s.ctrl.activity(s.now);
  TEST_ASSERT_FALSE(s.ctrl.dimmed());
  s.settle();
  TEST_ASSERT_EQUAL(180, s.ctrl.brightness());
  Page p;
  bool animate;
  TEST_ASSERT_FALSE(s.ctrl.take_page_change(p, animate));
  TEST_ASSERT_EQUAL(before, s.ctrl.page());
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
  RUN_TEST(test_unavailable_page_skipped);
  RUN_TEST(test_hidden_page_skipped);
  RUN_TEST(test_all_hidden_stays_on_clock);
  RUN_TEST(test_back_goes_to_previous_page);
  RUN_TEST(test_stay_delays_rotation);
  RUN_TEST(test_rotation_off_and_single_page);
  RUN_TEST(test_night_brightness_zero_turns_off);
  RUN_TEST(test_night_mode_disabled);
  RUN_TEST(test_input_ignored_before_start);
  RUN_TEST(test_settings_change_fades);
  RUN_TEST(test_millis_wraparound);
  RUN_TEST(test_pinned_page_stays_and_returns);
  RUN_TEST(test_pinned_page_wins_at_night);
  RUN_TEST(test_wake_for_reminder);
  RUN_TEST(test_away_turns_off_and_back_on);
  RUN_TEST(test_occasional_motion_keeps_screen_on);
  RUN_TEST(test_tap_while_away_only_wakes);
  RUN_TEST(test_away_at_night_returns_to_night_brightness);
  RUN_TEST(test_wake_overrides_away);
  RUN_TEST(test_away_disabled_or_no_radar);
  RUN_TEST(test_activity_wakes_without_page_change);
  return UNITY_END();
}
