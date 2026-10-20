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
  current.sit_on = prefs.getBool("sit_on", false);
  current.sit_min = prefs.getUChar("sit_min", 45);
  current.water_on = prefs.getBool("water_on", false);
  current.water_min = prefs.getUChar("water_min", 60);
  current.remind_start = prefs.getUShort("remind_start", 9 * 60);
  current.remind_end = prefs.getUShort("remind_end", 21 * 60);
  current.chime_on = prefs.getBool("chime_on", false);
  current.alarm_on = prefs.getBool("alarm_on", false);
  current.alarm_time = prefs.getUShort("alarm_time", 7 * 60 + 30);
  current.alarm_days = prefs.getUChar("alarm_days", 0x3E);
  current.climate_on = prefs.getBool("climate_on", true);
  current.hot_c = prefs.getUChar("hot_c", 30);
  current.humid_pct = prefs.getUChar("humid_pct", 80);
  current.temp_offset = prefs.getChar("temp_ofs", 0);
  current.away_off = prefs.getBool("away_off", true);
  current.away_min = prefs.getUChar("away_min", 5);
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
  prefs.putBool("sit_on", s.sit_on);
  prefs.putUChar("sit_min", s.sit_min);
  prefs.putBool("water_on", s.water_on);
  prefs.putUChar("water_min", s.water_min);
  prefs.putUShort("remind_start", s.remind_start);
  prefs.putUShort("remind_end", s.remind_end);
  prefs.putBool("chime_on", s.chime_on);
  prefs.putBool("alarm_on", s.alarm_on);
  prefs.putUShort("alarm_time", s.alarm_time);
  prefs.putUChar("alarm_days", s.alarm_days);
  prefs.putBool("climate_on", s.climate_on);
  prefs.putUChar("hot_c", s.hot_c);
  prefs.putUChar("humid_pct", s.humid_pct);
  prefs.putChar("temp_ofs", s.temp_offset);
  prefs.putBool("away_off", s.away_off);
  prefs.putUChar("away_min", s.away_min);
}

void settings_load_pomo_stats(PomoStatsData &d) {
  PomoStatsData loaded;
  if (prefs.getBytesLength("pomo_stats") == sizeof(loaded) &&
      prefs.getBytes("pomo_stats", &loaded, sizeof(loaded)) == sizeof(loaded)) {
    d = loaded;
  }
}

void settings_save_pomo_stats(const PomoStatsData &d) {
  prefs.putBytes("pomo_stats", &d, sizeof(d));
}

int settings_load_game_best() { return prefs.getUShort("run_best", 0); }

void settings_save_game_best(int best) { prefs.putUShort("run_best", best > 65535 ? 65535 : best); }
