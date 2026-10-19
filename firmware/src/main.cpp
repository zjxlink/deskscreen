#include <Arduino.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <esp_heap_caps.h>
#include <lvgl.h>

#include "audio.h"
#include "board.h"
#include "display.h"
#include "github.h"
#include "ota.h"
#include "pomo_stats.h"
#include "pomodoro.h"
#include "reminder.h"
#include "screen_ctrl.h"
#include "settings.h"
#include "stock.h"
#include "ui.h"
#include "weather.h"
#include "web_config.h"

static constexpr const char *kHostname = "deskscreen";
static constexpr uint32_t kWelcomeMs = 8000;      // 连上 WiFi 后显示设置地址的时间
static constexpr uint32_t kResetHoldMs = 3000;    // 长按 BOOT 键清除 WiFi
static constexpr uint32_t kShortPressMs = 1000;   // 短按 BOOT 键切换页面
static constexpr uint32_t kPomoResetMs = 1500;    // 长按番茄钟圆环重置
static constexpr uint32_t kPomoMsgMs = 60000;     // 阶段结束的提示显示多久
static constexpr uint32_t kReminderShowMs = 5 * 60000;  // 久坐、喝水提醒没人理会时多久自动关掉

static WiFiManager wm;
static String ap_name;
static uint32_t welcome_until = 0;

static ScreenCtrl screen;
static bool ota_screen = false;  // 正在显示升级进度，页面切换先暂停

static Pomodoro pomo;
static const char *pomo_msg = nullptr;  // 阶段结束时的提示文字
static uint32_t pomo_msg_until = 0;
static bool pomo_dirty = true;           // 番茄钟页需要立即刷新
static bool sound_on = true;
static bool sound_quiet_night = true;

static PomoStats pomo_stats;
static int pomo_goal = 4;
static bool stats_dirty = true;  // 统计页需要重画

static Reminder sit_reminder;
static Reminder water_reminder;

// 全屏提醒（留言、久坐、喝水）：盖在页面上，点屏幕或按 BOOT 键关掉
static bool alert_on = false;
static bool alert_timed = false;  // 久坐、喝水提醒到时自动关掉；留言一直显示到有人关掉
static uint32_t alert_until = 0;

static void apply_settings() {
  Settings s = settings_get();
  ui_set_city(s.city.c_str());

  ScreenConfig c;
  c.day_brightness = s.brightness;
  c.night_on = s.night_on;
  c.night_start = s.night_start;
  c.night_end = s.night_end;
  c.night_brightness = s.night_brightness;
  c.page_seconds = s.page_seconds;
  c.page_enabled[PAGE_CLOCK] = true;
  c.page_enabled[PAGE_GITHUB] = s.github_token.length() > 0;
  c.page_enabled[PAGE_STOCKS] = s.stocks.length() > 0;
  c.page_enabled[PAGE_FOCUS] = pomo_stats.data().last_day >= 0;  // 用过番茄钟才轮播统计页
  screen.configure(c);  // 番茄钟页不参与轮播，进行中时固定显示

  PomoConfig pc;
  pc.focus_min = s.pomo_focus;
  pc.short_min = s.pomo_short;
  pc.long_min = s.pomo_long;
  pc.long_every = s.pomo_every;
  pomo.configure(pc);
  pomo_dirty = true;
  pomo_goal = s.pomo_goal;
  stats_dirty = true;

  ReminderConfig rc;
  rc.start = s.remind_start;
  rc.end = s.remind_end;
  rc.on = s.sit_on;
  rc.interval_min = s.sit_min;
  sit_reminder.configure(rc);
  rc.on = s.water_on;
  rc.interval_min = s.water_min;
  water_reminder.configure(rc);

  sound_on = s.sound_on;
  sound_quiet_night = s.sound_quiet_night;
  audio_set_volume(s.sound_volume);
}

// 当天第几分钟，还没对时返回 -1
static int local_minutes() {
  struct tm t;
  if (!getLocalTime(&t, 0) || t.tm_year < 2024 - 1900) return -1;
  return t.tm_hour * 60 + t.tm_min;
}

// 夜间时段默认不响
static void play(SoundId id) {
  if (sound_on && !(sound_quiet_night && screen.night())) audio_play(id);
}

// 今天是 1970-01-01 起第几天（北京时间），还没对时返回 -1
static int local_day() {
  time_t now = time(nullptr);
  struct tm t;
  if (!getLocalTime(&t, 0) || t.tm_year < 2024 - 1900) return -1;
  return (int)((now + 8 * 3600) / 86400);
}

static void show_alert(AlertIcon icon, const char *title, const char *body, const char *foot,
                       uint32_t show_ms) {
  uint32_t now = millis();
  alert_on = true;
  alert_timed = show_ms > 0;
  alert_until = now + show_ms;
  if (screen.started()) screen.wake(now);
  if (!ota_screen) ui_show_alert(icon, title, body, foot);
}

static void close_alert() {
  alert_on = false;
  if (!ota_screen) ui_show_page(screen.page(), false);
}

// 设置页面发来的留言
static void on_message(const String &text, bool sound) {
  struct tm t;
  char foot[48] = "点屏幕或按 BOOT 键关闭";
  if (getLocalTime(&t, 0) && t.tm_year >= 2024 - 1900) {
    snprintf(foot, sizeof(foot), "%02d:%02d 收到 · 点屏幕或按 BOOT 键关闭", t.tm_hour, t.tm_min);
  }
  show_alert(AlertIcon::Message, "新留言", text.c_str(), foot, 0);
  if (sound) play(SoundId::Message);
}

// 触摸、设置页面的番茄钟操作都走这里
static void pomo_command(PomoCmd cmd) {
  uint32_t now = millis();
  switch (cmd) {
    case PomoCmd::Toggle: pomo.toggle(now); break;
    case PomoCmd::Skip: pomo.skip(now); break;
    case PomoCmd::Reset: pomo.reset(); break;
  }
  pomo_msg = nullptr;
  pomo_dirty = true;
}

// 推进番茄钟；阶段结束时点亮屏幕、跳到番茄钟页并提示。进行中或提示期间固定在番茄钟页
static void update_pomodoro() {
  uint32_t now = millis();
  struct tm t;
  int day = getLocalTime(&t, 0) && t.tm_year >= 2024 - 1900 ? t.tm_yday : -1;
  if (pomo.tick(now, day)) {
    bool focus_done = pomo.phase() != PomoPhase::Focus;
    pomo_msg = focus_done ? "专注完成，休息一下吧" : "休息结束，点圆环开始";
    pomo_msg_until = now + kPomoMsgMs;
    pomo_dirty = true;
    play(focus_done ? SoundId::FocusDone : SoundId::BreakDone);
    if (focus_done) {
      pomo_stats.record(local_day(), pomo_goal);
      settings_save_pomo_stats(pomo_stats.data());
      stats_dirty = true;
      sit_reminder.restart(now);  // 休息时会起身，久坐提醒重新计时
      apply_settings();           // 第一次有记录时统计页加入轮播
    }
    if (screen.started()) {
      screen.wake(now);
      screen.go_to(PAGE_POMODORO, now);
    }
  }
  if (pomo_msg && (int32_t)(now - pomo_msg_until) >= 0) {
    pomo_msg = nullptr;
    pomo_dirty = true;
  }
  if (pomo.running() || pomo_msg) {
    screen.pin(PAGE_POMODORO);
  } else {
    screen.unpin();
  }

  // 秒数变了或状态变了才重画
  static uint32_t last_secs = UINT32_MAX;
  uint32_t secs = (pomo.remaining_ms(now) + 999) / 1000;
  if (secs != last_secs || pomo_dirty) {
    last_secs = secs;
    pomo_dirty = false;
    int day = local_day();
    ui_set_pomodoro(pomo, now, pomo_msg, day >= 0 ? pomo_stats.count(day) : pomo.done_today());
  }
}

// 统计页：有新记录或者过了零点时重画
static void update_focus_stats() {
  static int last_day = -2;
  int day = local_day();
  if (day == last_day && !stats_dirty) return;
  last_day = day;
  stats_dirty = false;
  ui_set_focus_stats(pomo_stats, day, pomo_goal);
}

// 久坐、喝水提醒。专注中先不打扰，等休息时再提醒
static void update_reminders() {
  uint32_t now = millis();
  int minutes = local_minutes();
  bool hold = pomo.phase() == PomoPhase::Focus && pomo.running();
  bool sit = sit_reminder.tick(now, minutes, hold);
  bool water = water_reminder.tick(now, minutes, hold);
  // 留言还没看就不盖掉它，这次提醒跳过，下一轮再说
  if ((sit || water) && !(alert_on && !alert_timed)) {
    char body[96];
    const char *title;
    AlertIcon icon;
    if (sit && water) {
      title = "起来走走，顺便喝杯水";
      icon = AlertIcon::Sit;
      snprintf(body, sizeof(body), "已经坐了 %d 分钟了\n站起来活动一下，再去倒杯水吧", sit_reminder.config().interval_min);
    } else if (sit) {
      title = "该起来活动一下了";
      icon = AlertIcon::Sit;
      snprintf(body, sizeof(body), "已经坐了 %d 分钟了\n站起来走走，伸个懒腰，看看远处", sit_reminder.config().interval_min);
    } else {
      title = "该喝水了";
      icon = AlertIcon::Water;
      snprintf(body, sizeof(body), "每 %d 分钟喝一杯水\n小口慢慢喝，别等口渴了再喝", water_reminder.config().interval_min);
    }
    show_alert(icon, title, body, "点屏幕或按 BOOT 键关闭", kReminderShowMs);
    play(SoundId::Reminder);
  }
  if (alert_on && alert_timed && (int32_t)(now - alert_until) >= 0) close_alert();
}

// 把 ScreenCtrl 的状态同步到屏幕：亮度和当前页面
static void update_screen() {
  static int last_brightness = -1;
  screen.tick(millis(), local_minutes());
  if (screen.brightness() != last_brightness) {
    last_brightness = screen.brightness();
    display_set_brightness(last_brightness);
  }
  Page p;
  bool animate;
  // 升级进度和全屏提醒期间页面照常计算，只是先不显示
  if (screen.take_page_change(p, animate) && !ota_screen && !alert_on) ui_show_page(p, animate);
}

// 在线升级或网页上传固件时显示进度；成功后重启，失败时提示几秒再回到原来的页面
static void update_ota_screen() {
  static int last_progress = -1;
  OtaStatus s = ota_status();
  if (s.state == OtaState::Downloading) {
    if (!ota_screen || s.progress != last_progress) {
      ota_screen = true;
      last_progress = s.progress;
      char detail[48];
      snprintf(detail, sizeof(detail), "请不要断电（%d%%）", s.progress);
      ui_show_progress("正在升级固件", detail, s.progress);
    }
  } else if (s.state == OtaState::Done) {
    ui_show_progress("升级完成", "正在重启…", 100);
    ui_refresh_now();
    delay(1500);
    ESP.restart();
  } else if (ota_screen) {
    ota_screen = false;
    last_progress = -1;
    ui_show_status("升级失败", s.error);
    ui_refresh_now();
    delay(3000);
    if (alert_on) {
      close_alert();  // 提醒的内容已经被升级画面盖掉了，直接回到页面
    } else {
      ui_show_page(screen.page(), false);
    }
  }
}

static void connect_wifi() {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char name[32];
  snprintf(name, sizeof(name), "DeskScreen-%02X%02X", mac[4], mac[5]);
  ap_name = name;

  WiFi.setHostname(kHostname);
  wm.setTitle("桌面信息屏");
  wm.setConfigPortalTimeout(300);
  wm.setAPCallback([](WiFiManager *) {
    ui_show_provision(ap_name.c_str());
    ui_refresh_now();
  });

  if (!wm.autoConnect(ap_name.c_str())) {
    ui_show_status("配网超时", "5 分钟内没有完成配置\n即将重启后重试");
    ui_refresh_now();
    delay(3000);
    ESP.restart();
  }
}

// 点屏幕或短按 BOOT 键：有全屏提醒时先关掉提醒（屏幕调暗时第一下只点亮），否则切换页面
static void on_tap(uint32_t now) {
  if (alert_on && !ota_screen && !screen.dimmed()) {
    close_alert();
    return;
  }
  screen.on_input(now);
}

// 短按切换页面，长按 3 秒清除 WiFi
static void check_button() {
  static uint32_t pressed_at = 0;
  if (digitalRead(PIN_BUTTON) == LOW) {
    if (pressed_at == 0) pressed_at = millis();
    if (millis() - pressed_at > kResetHoldMs) {
      ui_show_status("重新配置 WiFi", "已清除 WiFi 信息，正在重启…");
      ui_refresh_now();
      wm.resetSettings();
      delay(1000);
      ESP.restart();
    }
  } else {
    if (pressed_at != 0 && millis() - pressed_at < kShortPressMs) on_tap(millis());
    pressed_at = 0;
  }
}

// 点一下屏幕切换页面（按下的瞬间触发）。
// 番茄钟页的圆环例外：松手时开始/暂停，按住 1.5 秒重置
static void check_touch() {
  static bool was_touched = false;
  static bool on_ring = false;
  static bool long_done = false;
  static uint32_t down_at = 0;
  static uint32_t last_poll = 0;
  uint32_t now = millis();
  if (now - last_poll < 30) return;  // 每次读触摸都要走 I2C，没必要太频繁
  last_poll = now;

  int x = 0, y = 0;
  bool touched = display_touch(x, y);
  if (touched && !was_touched) {
    down_at = now;
    long_done = false;
    // 屏幕调暗时第一下只点亮，交给 ScreenCtrl 处理
    on_ring = screen.started() && !screen.dimmed() && !ota_screen && !alert_on &&
              screen.page() == PAGE_POMODORO && ui_pomodoro_hit(x, y);
    if (!on_ring) on_tap(now);
  } else if (touched && on_ring && !long_done && now - down_at >= kPomoResetMs) {
    long_done = true;
    pomo_command(PomoCmd::Reset);
  } else if (!touched && was_touched && on_ring && !long_done) {
    pomo_command(PomoCmd::Toggle);
  }
  was_touched = touched;
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_BUTTON, INPUT_PULLUP);

  settings_begin();
  PomoStatsData sd;
  settings_load_pomo_stats(sd);
  pomo_stats.load(sd);
  display_init();
  audio_begin();  // 解码芯片和触摸共用 I2C，要在屏幕初始化之后
  ui_init();
  apply_settings();
  update_screen();  // 还没对时，先用白天亮度

  ui_show_status("桌面信息屏", "正在连接 WiFi…");
  ui_refresh_now();
  connect_wifi();

  configTzTime(TIMEZONE, "ntp.aliyun.com", "cn.pool.ntp.org", "pool.ntp.org");
  MDNS.begin(kHostname);
  MDNS.addService("http", "tcp", 80);
  web_config_begin();
  web_config_set_pomodoro(&pomo, pomo_command);
  web_config_set_pomo_stats(&pomo_stats, local_day);
  web_config_set_message_hook(on_message);
  // 网页上传固件期间主循环被阻塞，由上传处理顺带刷新进度画面
  web_config_set_progress_hook([] {
    update_ota_screen();
    ui_refresh_now();
  });
  weather_begin();
  github_begin();
  stock_begin();
  ota_begin();
  log_i("heap: internal %u (largest block %u), psram %u", heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL), heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

  String detail = "用同一 WiFi 下的手机或电脑打开\n#58A6FF http://" + WiFi.localIP().toString() +
                  "#\n或 #58A6FF http://" + kHostname + ".local#\n即可设置城市、亮度和 GitHub";
  ui_show_status("已连接 WiFi", detail.c_str());
  welcome_until = millis() + kWelcomeMs;
}

void loop() {
  lv_timer_handler();
  web_config_loop();
  check_button();
  check_touch();

  if (welcome_until && (int32_t)(millis() - welcome_until) >= 0) {
    welcome_until = 0;
    screen.start(millis());
  }
  if (web_config_take_changed()) apply_settings();
  update_pomodoro();
  update_focus_stats();
  update_reminders();
  update_screen();
  update_ota_screen();

  WeatherData w;
  if (weather_take_update(w)) ui_set_weather(w);

  static GithubData g;
  if (github_take_update(g)) ui_set_github(g);

  static StockData st;
  if (stock_take_update(st)) ui_set_stocks(st);

  static bool last_wifi = false;
  bool wifi = WiFi.isConnected();
  if (wifi != last_wifi) {
    last_wifi = wifi;
    ui_set_wifi(wifi);
  }

  ui_tick_clock();
  delay(5);
}
