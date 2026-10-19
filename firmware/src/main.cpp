#include <Arduino.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <lvgl.h>

#include "board.h"
#include "display.h"
#include "settings.h"
#include "ui.h"
#include "weather.h"
#include "web_config.h"

static constexpr const char *kHostname = "deskscreen";
static constexpr uint32_t kWelcomeMs = 8000;      // 连上 WiFi 后显示设置地址的时间
static constexpr uint32_t kResetHoldMs = 3000;    // 长按 BOOT 键清除 WiFi

static WiFiManager wm;
static String ap_name;
static uint32_t welcome_until = 0;

static void apply_settings() {
  Settings s = settings_get();
  display_set_brightness(s.brightness);
  ui_set_city(s.city.c_str());
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

static void check_reset_button() {
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
    pressed_at = 0;
  }
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

  String detail = "用同一 WiFi 下的手机或电脑打开\n#58A6FF http://" + WiFi.localIP().toString() +
                  "#\n或 #58A6FF http://" + kHostname + ".local#\n即可设置城市和亮度";
  ui_show_status("已连接 WiFi", detail.c_str());
  welcome_until = millis() + kWelcomeMs;
}

void loop() {
  lv_timer_handler();
  web_config_loop();
  check_reset_button();

  if (welcome_until && (int32_t)(millis() - welcome_until) >= 0) {
    welcome_until = 0;
    ui_show_main();
  }

  if (web_config_take_changed()) apply_settings();

  WeatherData w;
  if (weather_take_update(w)) ui_set_weather(w);

  static bool last_wifi = false;
  bool wifi = WiFi.isConnected();
  if (wifi != last_wifi) {
    last_wifi = wifi;
    ui_set_wifi(wifi);
  }

  ui_tick_clock();
  delay(5);
}
