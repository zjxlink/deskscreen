#include "settings.h"

#include <Preferences.h>

static Preferences prefs;
static Settings current;
static SemaphoreHandle_t lock;

void settings_begin() {
  lock = xSemaphoreCreateMutex();
  prefs.begin("desk", false);
  current.city = prefs.getString("city", "北京");
  current.lat = prefs.getFloat("lat", 39.9042f);
  current.lon = prefs.getFloat("lon", 116.4074f);
  current.brightness = prefs.getUChar("bright", 180);
  current.github_token = prefs.getString("gh_token", "");
  current.stocks = prefs.getString("stocks", "");
  current.page_seconds = prefs.getUChar("page_sec", 15);
  current.night_on = prefs.getBool("night_on", true);
  current.night_start = prefs.getUShort("night_start", 23 * 60);
  current.night_end = prefs.getUShort("night_end", 7 * 60);
  current.night_brightness = prefs.getUChar("night_bright", 10);
  current.auto_update = prefs.getBool("auto_upd", true);
  current.pomo_focus = prefs.getUChar("pomo_focus", 25);
  current.pomo_short = prefs.getUChar("pomo_short", 5);
  current.pomo_long = prefs.getUChar("pomo_long", 15);
  current.pomo_every = prefs.getUChar("pomo_every", 4);
  current.sound_on = prefs.getBool("sound_on", true);
  current.sound_volume = prefs.getUChar("sound_vol", 60);
  current.sound_quiet_night = prefs.getBool("sound_quiet", true);
}

Settings settings_get() {
  xSemaphoreTake(lock, portMAX_DELAY);
  Settings s = current;
  xSemaphoreGive(lock);
  return s;
}

void settings_save(const Settings &s) {
  xSemaphoreTake(lock, portMAX_DELAY);
  current = s;
  xSemaphoreGive(lock);
  prefs.putString("city", s.city);
  prefs.putFloat("lat", s.lat);
  prefs.putFloat("lon", s.lon);
  prefs.putUChar("bright", s.brightness);
  prefs.putString("gh_token", s.github_token);
  prefs.putString("stocks", s.stocks);
  prefs.putUChar("page_sec", s.page_seconds);
  prefs.putBool("night_on", s.night_on);
  prefs.putUShort("night_start", s.night_start);
  prefs.putUShort("night_end", s.night_end);
  prefs.putUChar("night_bright", s.night_brightness);
  prefs.putBool("auto_upd", s.auto_update);
  prefs.putUChar("pomo_focus", s.pomo_focus);
  prefs.putUChar("pomo_short", s.pomo_short);
  prefs.putUChar("pomo_long", s.pomo_long);
  prefs.putUChar("pomo_every", s.pomo_every);
  prefs.putBool("sound_on", s.sound_on);
  prefs.putUChar("sound_vol", s.sound_volume);
  prefs.putBool("sound_quiet", s.sound_quiet_night);
}
