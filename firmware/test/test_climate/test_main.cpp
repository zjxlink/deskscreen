// 室内温湿度的单元测试：AHT30 数据解析和闷热提醒的时机，在电脑上运行：pio test -e native
#include <unity.h>

#include "climate.h"

void setUp() {}
void tearDown() {}

static const uint32_t MIN = 60 * 1000;

// 按 AHT30 的格式拼一帧数据
static void make_frame(float temp, float humidity, uint8_t raw[7], uint8_t status = 0x1C) {
  uint32_t h = (uint32_t)(humidity / 100.0f * (1 << 20));
  uint32_t t = (uint32_t)((temp + 50) / 200.0f * (1 << 20));
  raw[0] = status;
  raw[1] = h >> 12;
  raw[2] = h >> 4;
  raw[3] = (h & 0x0F) << 4 | (t >> 16 & 0x0F);
  raw[4] = t >> 8;
  raw[5] = t;
  raw[6] = aht_crc(raw, 6);
}

static void test_crc_known_value() {
  // 数据手册的例子：0xBE 0xEF 的 CRC 为 0x92
  const uint8_t d[] = {0xBE, 0xEF};
  TEST_ASSERT_EQUAL_HEX8(0x92, aht_crc(d, 2));
}

static void test_parse_frame() {
  uint8_t raw[7];
  make_frame(26.5f, 55.0f, raw);
  float t, h;
  TEST_ASSERT_TRUE(aht_parse(raw, t, h));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 26.5f, t);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 55.0f, h);

  make_frame(-10.0f, 3.0f, raw);
  TEST_ASSERT_TRUE(aht_parse(raw, t, h));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, -10.0f, t);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 3.0f, h);
}

static void test_parse_rejects_busy_and_bad_crc() {
  uint8_t raw[7];
  float t = 0, h = 0;
  make_frame(26.5f, 55.0f, raw, 0x9C);  // 还在测量
  TEST_ASSERT_FALSE(aht_parse(raw, t, h));
  make_frame(26.5f, 55.0f, raw);
  raw[6] ^= 1;
  TEST_ASSERT_FALSE(aht_parse(raw, t, h));
}

// 模拟每 5 秒读一次
struct Sim {
  ClimateAlert a;
  uint32_t now = 1000;
  Sim() {
    ClimateConfig c;
    c.hot_c = 30;
    c.humid_pct = 80;
    a.configure(c);
  }
  // 返回期间提醒了几次热、几次闷
  void run(uint32_t ms, float temp, float humidity, int &hot, int &humid, bool hold = false) {
    for (uint32_t t = 0; t < ms; t += 5000) {
      now += 5000;
      ClimateEvent e = a.tick(now, temp, humidity, hold);
      if (e == ClimateEvent::Hot) hot++;
      if (e == ClimateEvent::Humid) humid++;
    }
  }
};

static void test_hot_needs_three_minutes() {
  Sim s;
  int hot = 0, humid = 0;
  s.run(2 * MIN, 31, 50, hot, humid);
  TEST_ASSERT_EQUAL(0, hot);
  s.run(MIN + 5000, 31, 50, hot, humid);
  TEST_ASSERT_EQUAL(1, hot);
  s.run(3 * 60 * MIN, 31, 50, hot, humid);  // 一直热也只提醒一次
  TEST_ASSERT_EQUAL(1, hot);
  TEST_ASSERT_EQUAL(0, humid);
}

static void test_short_spike_ignored() {
  Sim s;
  int hot = 0, humid = 0;
  for (int i = 0; i < 10; i++) {
    s.run(2 * MIN, 32, 50, hot, humid);
    s.run(30000, 27, 50, hot, humid);  // 降回去又升上来，计时从头开始
  }
  TEST_ASSERT_EQUAL(0, hot);
}

static void test_hovering_near_threshold_does_not_repeat() {
  Sim s;
  int hot = 0, humid = 0;
  s.run(4 * MIN, 30.5f, 50, hot, humid);
  TEST_ASSERT_EQUAL(1, hot);
  for (int i = 0; i < 20; i++) {  // 在 29.5~30.5 之间来回，没有真正凉下来
    s.run(4 * MIN, 29.5f, 50, hot, humid);
    s.run(4 * MIN, 30.5f, 50, hot, humid);
  }
  TEST_ASSERT_EQUAL(1, hot);
}

static void test_cooled_down_then_hot_again() {
  Sim s;
  int hot = 0, humid = 0;
  s.run(4 * MIN, 31, 50, hot, humid);
  TEST_ASSERT_EQUAL(1, hot);
  s.run(10 * MIN, 27, 50, hot, humid);  // 开窗凉下来了
  s.run(10 * MIN, 31, 50, hot, humid);  // 又热了，但离上次不到 1 小时
  TEST_ASSERT_EQUAL(1, hot);
  s.run(45 * MIN, 31, 50, hot, humid);
  TEST_ASSERT_EQUAL(2, hot);
}

static void test_hold_delays_alert() {
  Sim s;
  int hot = 0, humid = 0;
  s.run(10 * MIN, 31, 50, hot, humid, true);  // 夜间或正在显示别的提醒
  TEST_ASSERT_EQUAL(0, hot);
  s.run(5000, 31, 50, hot, humid);
  TEST_ASSERT_EQUAL(1, hot);
}

static void test_hot_and_humid_one_at_a_time() {
  Sim s;
  int hot = 0, humid = 0;
  s.run(3 * MIN + 10000, 32, 85, hot, humid);
  TEST_ASSERT_EQUAL(1, hot);
  TEST_ASSERT_EQUAL(1, humid);  // 下一次读数时再提醒闷
  // 两次不在同一次读数：热的那次读数上闷不提醒
  Sim s2;
  int h2 = 0, m2 = 0;
  while (h2 == 0) s2.run(5000, 32, 85, h2, m2);
  TEST_ASSERT_EQUAL(0, m2);
  s2.run(5000, 32, 85, h2, m2);
  TEST_ASSERT_EQUAL(1, m2);
}

static void test_disabled() {
  ClimateAlert a;
  ClimateConfig c;
  c.on = false;
  a.configure(c);
  for (uint32_t t = 0; t < 60 * MIN; t += 5000) TEST_ASSERT_TRUE(a.tick(t, 40, 95, false) == ClimateEvent::None);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_crc_known_value);
  RUN_TEST(test_parse_frame);
  RUN_TEST(test_parse_rejects_busy_and_bad_crc);
  RUN_TEST(test_hot_needs_three_minutes);
  RUN_TEST(test_short_spike_ignored);
  RUN_TEST(test_hovering_near_threshold_does_not_repeat);
  RUN_TEST(test_cooled_down_then_hot_again);
  RUN_TEST(test_hold_delays_alert);
  RUN_TEST(test_hot_and_humid_one_at_a_time);
  RUN_TEST(test_disabled);
  return UNITY_END();
}
