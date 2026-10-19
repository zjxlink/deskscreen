#include <Arduino.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <lvgl.h>

#include "audio.h"
#include "board.h"
#include "display.h"
#include "github.h"
#include "ota.h"
#include "pomodoro.h"
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
  screen.configure(c);  // 番茄钟页不参与轮播，进行中时固定显示

  PomoConfig pc;
  pc.focus_min = s.pomo_focus;
  pc.short_min = s.pomo_short;
  pc.long_min = s.pomo_long;
  pc.long_every = s.pomo_every;
  pomo.configure(pc);
  pomo_dirty = true;

  sound_on = s.sound_on;
  sound_quiet_night = s.sound_quiet_night;
  audio_set_volume(s.sound_volume);
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
    pomo_msg = pomo.phase() == PomoPhase::Focus ? "休息结束，点圆环开始" : "专注完成，休息一下吧";
    pomo_msg_until = now + kPomoMsgMs;
    pomo_dirty = true;
    // 夜间时段默认不响，只点亮屏幕
    if (sound_on && !(sound_quiet_night && screen.night())) {
      audio_play(pomo.phase() == PomoPhase::Focus ? SoundId::BreakDone : SoundId::FocusDone);
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
    ui_set_pomodoro(pomo, now, pomo_msg);
  }
}

// 当天第几分钟，还没对时返回 -1
static int local_minutes() {
  struct tm t;
  if (!getLocalTime(&t, 0) || t.tm_year < 2024 - 1900) return -1;
  return t.tm_hour * 60 + t.tm_min;
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
  if (!ota_screen && screen.take_page_change(p, animate)) ui_show_page(p, animate);
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
    ui_show_page(screen.page(), false);
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
    if (pressed_at != 0 && millis() - pressed_at < kShortPressMs) screen.on_input(millis());
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
    on_ring = screen.started() && !screen.dimmed() && !ota_screen && screen.page() == PAGE_POMODORO &&
              ui_pomodoro_hit(x, y);
    if (!on_ring) screen.on_input(now);
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
  // 网页上传固件期间主循环被阻塞，由上传处理顺带刷新进度画面
  web_config_set_progress_hook([] {
    update_ota_screen();
    ui_refresh_now();
  });
  weather_begin();
  github_begin();
  stock_begin();
  ota_begin();

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
