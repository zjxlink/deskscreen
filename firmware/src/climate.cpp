#include "climate.h"

uint8_t aht_crc(const uint8_t *data, int len) {
  uint8_t crc = 0xFF;
  for (int i = 0; i < len; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++) crc = crc & 0x80 ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
  }
  return crc;
}

bool aht_parse(const uint8_t raw[7], float &temp, float &humidity) {
  if (raw[0] & 0x80) return false;  // 还在测量
  if (aht_crc(raw, 6) != raw[6]) return false;
  uint32_t h = (uint32_t)raw[1] << 12 | (uint32_t)raw[2] << 4 | raw[3] >> 4;
  uint32_t t = (uint32_t)(raw[3] & 0x0F) << 16 | (uint32_t)raw[4] << 8 | raw[5];
  humidity = h * 100.0f / (1 << 20);
  temp = t * 200.0f / (1 << 20) - 50;
  return true;
}

void ClimateAlert::configure(const ClimateConfig &cfg) {
  if (cfg == cfg_) return;
  cfg_ = cfg;
  hot_ = Track();
  humid_ = Track();
}

bool ClimateAlert::step(Track &t, uint32_t now_ms, bool over, bool recovered, bool hold) {
  if (recovered) {
    t.over = false;
    t.fired = false;
    return false;
  }
  if (!over) {
    t.over = false;  // 在阈值附近，计时从头开始，但这一轮提醒过的不再提醒
    return false;
  }
  if (!t.over) {
    t.over = true;
    t.since = now_ms;
  }
  if (t.fired || hold || now_ms - t.since < kHoldMs) return false;
  if (t.ever && now_ms - t.last < kQuietMs) return false;
  t.fired = true;
  t.ever = true;
  t.last = now_ms;
  return true;
}

ClimateEvent ClimateAlert::tick(uint32_t now_ms, float temp, float humidity, bool hold) {
  if (!cfg_.on) {
    hot_ = Track();
    humid_ = Track();
    return ClimateEvent::None;
  }
  bool hot = step(hot_, now_ms, climate_is_hot(cfg_, temp), temp < cfg_.hot_c - kTempBand, hold);
  // 同时到点时先说热，闷的那条等下一次读数再说
  bool humid = step(humid_, now_ms, climate_is_humid(cfg_, humidity), humidity < cfg_.humid_pct - kHumidBand, hold || hot);
  if (hot) return ClimateEvent::Hot;
  if (humid) return ClimateEvent::Humid;
  return ClimateEvent::None;
}

bool climate_is_hot(const ClimateConfig &cfg, float temp) { return temp >= cfg.hot_c; }
bool climate_is_humid(const ClimateConfig &cfg, float humidity) { return humidity >= cfg.humid_pct; }
