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
}
