#include "sensors.h"

#include "board.h"

#ifdef HAS_SENSOR_DOCK

#include <Arduino.h>
#include <LovyanGFX.hpp>
#include <driver/i2c.h>

#include "climate.h"

static constexpr int kI2cPort = I2C_NUM_1;  // 0 号给了触摸和解码芯片
static constexpr uint32_t kI2cFreq = 100000;
static constexpr uint8_t kAhtAddr = 0x38;
static constexpr uint8_t kRadarAddr = 0x28;
static constexpr uint32_t kReadEveryMs = 5000;
static constexpr uint32_t kMeasureMs = 100;  // AHT30 一次测量最多 80ms

static bool dock = false;
static bool measuring = false;
static uint32_t next_at = 0;
static bool fresh = false;
static float last_temp = 0;
static float last_humidity = 0;

static bool radar_write(uint8_t reg, uint8_t val) {
  return lgfx::i2c::writeRegister8(kI2cPort, kRadarAddr, reg, val, 0, kI2cFreq).has_value();
}

// 雷达的默认配置，和乐鑫 esp-iot-solution 的 at581x 驱动（ATH581X_INITIALIZATION_CONFIG）一致
static bool radar_init() {
  static const uint8_t regs[][2] = {
      {0x68, 0x68}, {0x67, 0x8B},                              // 功耗 70uA
      {0x10, 0xC8}, {0x11, 0x00},                              // 灵敏度阈值 200
      {0x5C, 0x1B},                                            // 增益 3 档
      {0x3D, 0xF4}, {0x3E, 0x01}, {0x3F, 0x00}, {0x40, 0x00},  // 触发基准时间 500ms
      {0x41, 0x01}, {0x42, 0xDC}, {0x43, 0x05}, {0x44, 0x00}, {0x45, 0x00},  // 触发后保持 1.5 秒
      {0x38, 0xD0}, {0x39, 0x07},                              // 上电自检 2 秒
      {0x4E, 0xE8}, {0x4F, 0x03},                              // 两次触发之间的保护时间 1 秒
      {0x55, 0x04},
      {0x00, 0x00}, {0x00, 0x01},                              // 软复位，让配置生效
  };
  for (auto &r : regs) {
    if (!radar_write(r[0], r[1])) return false;
  }
  return true;
}

void sensors_begin() {
  lgfx::i2c::init(kI2cPort, PIN_DOCK_SDA, PIN_DOCK_SCL);
  uint8_t status = 0;
  bool aht = lgfx::i2c::transactionRead(kI2cPort, kAhtAddr, &status, 1, kI2cFreq).has_value();
  bool radar = aht && radar_init();
  dock = aht && radar;
  if (!dock) {
    log_i("sensor dock not found (aht %d)", aht);
    return;
  }
  pinMode(PIN_RADAR_OUT, INPUT_PULLDOWN);
  next_at = millis() + 200;  // AHT30 上电后要等一会儿
  log_i("sensor dock found");
}

void sensors_loop() {
  if (!dock || (int32_t)(millis() - next_at) < 0) return;
  if (!measuring) {
    static const uint8_t cmd[] = {0xAC, 0x33, 0x00};
    measuring = lgfx::i2c::transactionWrite(kI2cPort, kAhtAddr, cmd, sizeof(cmd), kI2cFreq).has_value();
    next_at = millis() + (measuring ? kMeasureMs : kReadEveryMs);
    return;
  }
  measuring = false;
  next_at = millis() + kReadEveryMs;
  uint8_t raw[7];
  if (!lgfx::i2c::transactionRead(kI2cPort, kAhtAddr, raw, sizeof(raw), kI2cFreq).has_value()) return;
  float t, h;
  if (!aht_parse(raw, t, h)) return;
  last_temp = t;
  last_humidity = h;
  fresh = true;
}

bool sensors_dock() { return dock; }

bool sensors_presence() { return dock && digitalRead(PIN_RADAR_OUT) == HIGH; }

bool sensors_take_climate(float &temp, float &humidity) {
  if (!fresh) return false;
  fresh = false;
  temp = last_temp;
  humidity = last_humidity;
  return true;
}

#else

void sensors_begin() {}
void sensors_loop() {}
bool sensors_dock() { return false; }
bool sensors_presence() { return false; }
bool sensors_take_climate(float &, float &) { return false; }

#endif
