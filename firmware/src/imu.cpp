#include "imu.h"

#include "board.h"

#ifdef HAS_IMU

#include <Arduino.h>
#include <LovyanGFX.hpp>
#include <driver/i2c.h>

static constexpr int kI2cPort = I2C_NUM_0;  // 和触摸、解码芯片共用
static constexpr uint32_t kI2cFreq = 400000;
static constexpr uint8_t kRegWhoAmI = 0x75;
static constexpr uint8_t kRegPwrMgmt0 = 0x1F;
static constexpr uint8_t kRegAccelConfig0 = 0x21;
static constexpr uint8_t kRegAccelData = 0x0B;
static constexpr float kLsbPerG = 8192;  // ±4g 量程

static bool ready = false;

bool imu_begin() {
  uint8_t id = 0;
  if (!lgfx::i2c::readRegister(kI2cPort, IMU_ADDR, kRegWhoAmI, &id, 1, kI2cFreq).has_value() ||
      (id != 0x60 && id != 0x67)) {  // ICM-42607 / ICM-42670
    log_i("imu not found (id 0x%02x)", id);
    return false;
  }
  // 加速度计 ±4g、100Hz，低噪声模式；陀螺仪不用，保持关闭
  ready = lgfx::i2c::writeRegister8(kI2cPort, IMU_ADDR, kRegAccelConfig0, 2 << 5 | 9, 0, kI2cFreq).has_value() &&
          lgfx::i2c::writeRegister8(kI2cPort, IMU_ADDR, kRegPwrMgmt0, 0x03, 0, kI2cFreq).has_value();
  delay(2);  // 打开加速度计后要等一会儿才有数据
  log_i("imu 0x%02x %s", id, ready ? "ready" : "config failed");
  return ready;
}

bool imu_read(float &x, float &y, float &z) {
  if (!ready) return false;
  uint8_t d[6];
  if (!lgfx::i2c::readRegister(kI2cPort, IMU_ADDR, kRegAccelData, d, sizeof(d), kI2cFreq).has_value()) return false;
  if (d[0] == 0x80 && d[1] == 0x00) return false;  // 还没有数据时读出来是 -32768
  x = (int16_t)(d[0] << 8 | d[1]) / kLsbPerG;
  y = (int16_t)(d[2] << 8 | d[3]) / kLsbPerG;
  z = (int16_t)(d[4] << 8 | d[5]) / kLsbPerG;
  return true;
}

#else

bool imu_begin() { return false; }
bool imu_read(float &, float &, float &) { return false; }

#endif
