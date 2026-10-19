#include <Arduino.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <lvgl.h>

#include "board.h"
#include "display.h"
#include "github.h"
#include "settings.h"
#include "stock.h"
#include "ui.h"
#include "weather.h"
#include "web_config.h"

static constexpr const char *kHostname = "deskscreen";
static constexpr uint32_t kWelcomeMs = 8000;      // 连上 WiFi 后显示设置地址的时间
static constexpr uint32_t kResetHoldMs = 3000;    // 长按 BOOT 键清除 WiFi
static constexpr uint32_t kShortPressMs = 1000;   // 短按 BOOT 键切换页面

static WiFiManager wm;
static String ap_name;
static uint32_t welcome_until = 0;

static Page page = PAGE_CLOCK;
static uint32_t page_shown_at = 0;
static uint8_t page_seconds = 0;
static bool github_enabled = false;
static bool stocks_enabled = false;

static void apply_settings() {
  Settings s = settings_get();
  display_set_brightness(s.brightness);
  ui_set_city(s.city.c_str());
  page_seconds = s.page_seconds;
  github_enabled = s.github_token.length() > 0;
  stocks_enabled = s.stocks.length() > 0;
}

// 自动轮播只轮到已经设置好的页面
static bool page_enabled(Page p) {
  switch (p) {
    case PAGE_GITHUB: return github_enabled;
    case PAGE_STOCKS: return stocks_enabled;
    default: return true;
  }
}

static void show_page(Page p, bool animate) {
  page = p;
  page_shown_at = millis();
  ui_show_page(p, animate);
}

// 手动切换时轮流显示所有页面（没设置的页面也能看到提示）
static void next_page() {
  show_page((Page)((page + 1) % PAGE_COUNT), true);
}

static void auto_rotate() {
  if (page_seconds == 0 || millis() - page_shown_at < page_seconds * 1000UL) return;
  for (int k = 1; k <= PAGE_COUNT; k++) {
    Page p = (Page)((page + k) % PAGE_COUNT);
    if (page_enabled(p)) {
      show_page(p, p != page);
      return;
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
    if (pressed_at != 0 && millis() - pressed_at < kShortPressMs && !welcome_until) next_page();
    pressed_at = 0;
  }
}

// 点一下屏幕切换页面（按下的瞬间触发）
static void check_touch() {
  static bool was_touched = false;
  static uint32_t last_poll = 0;
  if (millis() - last_poll < 30) return;  // 每次读触摸都要走 I2C，没必要太频繁
  last_poll = millis();
  bool touched = display_touched();
  if (touched && !was_touched && !welcome_until) next_page();
  was_touched = touched;
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_BUTTON, INPUT_PULLUP);

  settings_begin();
  display_init();
  ui_init();
  apply_settings();

  ui_show_status("桌面信息屏", "正在连接 WiFi…");
  ui_refresh_now();
  connect_wifi();

  configTzTime(TIMEZONE, "ntp.aliyun.com", "cn.pool.ntp.org", "pool.ntp.org");
  MDNS.begin(kHostname);
  MDNS.addService("http", "tcp", 80);
  web_config_begin();
  weather_begin();
  github_begin();
  stock_begin();

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
    show_page(PAGE_CLOCK, false);
  }
  if (!welcome_until) auto_rotate();

  if (web_config_take_changed()) apply_settings();

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
