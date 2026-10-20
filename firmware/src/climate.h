#pragma once

#include <stdint.h>

// 室内温湿度：解析底座 AHT30 传感器的数据，判断什么时候该提醒「有点热」「有点闷」。
// 不依赖硬件，时间和读数由调用方传入，可在电脑上测试。

// AHT30 测量结果 7 个字节（状态、湿度 20 位、温度 20 位、CRC），校验不过或还在测量时返回 false
bool aht_parse(const uint8_t raw[7], float &temp, float &humidity);

// AHT30 的 CRC8（多项式 0x31，初值 0xFF）
uint8_t aht_crc(const uint8_t *data, int len);

struct ClimateConfig {
  bool on = true;          // 太热、太潮时提醒
  uint8_t hot_c = 30;      // 温度达到多少度算热
  uint8_t humid_pct = 80;  // 湿度达到多少算闷

  bool operator==(const ClimateConfig &o) const {
    return on == o.on && hot_c == o.hot_c && humid_pct == o.humid_pct;
  }
};

enum class ClimateEvent { None, Hot, Humid };

class ClimateAlert {
 public:
  static constexpr uint32_t kHoldMs = 3 * 60000;   // 连续超过这么久才提醒，开门、哈气不算
  static constexpr uint32_t kQuietMs = 60 * 60000;  // 同一种提醒至少隔这么久
  static constexpr float kTempBand = 1.0f;          // 降到阈值以下这么多才算恢复，免得在阈值附近反复提醒
  static constexpr float kHumidBand = 5.0f;

  // 设置变了从头判断
  void configure(const ClimateConfig &cfg);

  // 每次有新读数时调用。hold 为 true 时（夜间、正在显示别的提醒）到点也先不提醒，等解除后再说
  ClimateEvent tick(uint32_t now_ms, float temp, float humidity, bool hold);

  const ClimateConfig &config() const { return cfg_; }

 private:
  struct Track {
    bool over = false;       // 正在超过阈值
    uint32_t since = 0;      // 从什么时候开始超过
    bool fired = false;      // 这一轮已经提醒过，恢复正常前不再提醒
    bool ever = false;       // 提醒过至少一次
    uint32_t last = 0;       // 上次提醒的时间
  };

  // 返回 true 表示该提醒了
  static bool step(Track &t, uint32_t now_ms, bool over, bool recovered, bool hold);

  ClimateConfig cfg_;
  Track hot_;
  Track humid_;
};

// 温度是否偏高（时钟页上显示成橙色），没开提醒时也按阈值判断
bool climate_is_hot(const ClimateConfig &cfg, float temp);
bool climate_is_humid(const ClimateConfig &cfg, float humidity);
