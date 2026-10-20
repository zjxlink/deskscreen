// 动作识别的单元测试：用合成的加速度数据模拟摇一摇、敲桌子、急刹车、倒过来，在电脑上运行：pio test -e native
#include <math.h>
#include <unity.h>

#include "motion.h"

void setUp() {}
void tearDown() {}

struct Sim {
  MotionDetector d;
  uint32_t now = 1000;
  int shakes = 0, jolts = 0;
  float gx = 0, gy = 0, gz = 1;  // 重力方向

  void step(float ax, float ay, float az) {
    now += 20;
    MotionEvent e = d.update(now, ax, ay, az);
    if (e == MotionEvent::Shake) shakes++;
    if (e == MotionEvent::Jolt) jolts++;
  }
  void rest(uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += 20) step(gx, gy, gz);
  }
};

static void test_rest_no_events() {
  Sim s;
  s.rest(60000);
  TEST_ASSERT_EQUAL(0, s.shakes);
  TEST_ASSERT_EQUAL(0, s.jolts);
  TEST_ASSERT_FALSE(s.d.upside_down());
}

static void test_shake() {
  Sim s;
  s.rest(3000);
  for (int i = 0; i < 75; i++) {  // 1.5 秒，每秒来回 3 次，幅度 1.5g
    float x = 1.5f * sinf(2 * 3.14159f * 3 * i * 0.02f);
    s.step(x, 0, 1);
  }
  s.rest(2000);
  TEST_ASSERT_EQUAL(1, s.shakes);
  TEST_ASSERT_EQUAL(0, s.jolts);
}

static void test_long_shake_respects_cooldown() {
  Sim s;
  s.rest(3000);
  for (int i = 0; i < 250; i++) {  // 摇 5 秒
    float x = 1.5f * sinf(2 * 3.14159f * 3 * i * 0.02f);
    s.step(x, 0, 1);
  }
  TEST_ASSERT_EQUAL(2, s.shakes);
}

static void test_desk_tap_ignored() {
  Sim s;
  s.rest(3000);
  for (int k = 0; k < 10; k++) {
    s.step(0, 0, 3.0f);  // 一下 20ms 的冲击
    s.rest(1000);
  }
  TEST_ASSERT_EQUAL(0, s.shakes);
  TEST_ASSERT_EQUAL(0, s.jolts);
}

static void test_braking_jolt() {
  Sim s;
  s.rest(3000);
  for (int i = 0; i < 10; i++) s.step(0, 0.045f * i, 1);  // 200ms 内加到 0.45g
  for (int i = 0; i < 75; i++) s.step(0, 0.45f, 1);      // 持续 1.5 秒
  s.rest(3000);
  TEST_ASSERT_EQUAL(1, s.jolts);
  TEST_ASSERT_EQUAL(0, s.shakes);
}

static void test_gentle_driving_ignored() {
  Sim s;
  s.rest(3000);
  for (int i = 0; i < 3000; i++) {  // 一分钟的轻微颠簸和 0.15g 的平缓加减速
    float y = 0.15f * sinf(i * 0.01f) + 0.05f * sinf(i * 1.3f);
    s.step(0.03f * sinf(i * 0.7f), y, 1 + 0.05f * sinf(i * 2.1f));
  }
  TEST_ASSERT_EQUAL(0, s.jolts);
  TEST_ASSERT_EQUAL(0, s.shakes);
}

static void rotate_to(Sim &s, float gx, float gy, float gz, uint32_t ms) {
  float sx = s.gx, sy = s.gy, sz = s.gz;
  int steps = ms / 20;
  for (int i = 1; i <= steps; i++) {
    float t = (float)i / steps;
    float x = sx + (gx - sx) * t, y = sy + (gy - sy) * t, z = sz + (gz - sz) * t;
    float l = sqrtf(x * x + y * y + z * z);
    if (l < 0.01f) l = 0.01f, y = 0.01f;
    s.step(x / l, y / l, z / l);
  }
  s.gx = gx;
  s.gy = gy;
  s.gz = gz;
}

static void test_upside_down_and_back() {
  Sim s;
  s.gx = 0, s.gy = -1, s.gz = 0;  // 立在桌上
  s.rest(5000);
  rotate_to(s, 1, 0, 0, 500);
  rotate_to(s, 0, 1, 0, 500);  // 转了 180 度
  s.rest(3000);
  TEST_ASSERT_TRUE(s.d.upside_down());
  s.rest(60000);  // 倒着放久了也还是倒的
  TEST_ASSERT_TRUE(s.d.upside_down());
  rotate_to(s, 1, 0, 0, 500);
  rotate_to(s, 0, -1, 0, 500);
  s.rest(3000);
  TEST_ASSERT_FALSE(s.d.upside_down());
}

static void test_tilted_mount_is_normal() {
  Sim s;
  s.gx = 0, s.gy = -0.7f, s.gz = 0.7f;  // 车里斜着装
  s.rest(60000);
  TEST_ASSERT_FALSE(s.d.upside_down());
  TEST_ASSERT_EQUAL(0, s.jolts);
}

static void test_lying_flat_is_not_upside_down() {
  Sim s;
  s.gx = 0, s.gy = -1, s.gz = 0;
  s.rest(5000);
  rotate_to(s, 0, 0, 1, 800);  // 放倒平躺，只转了 90 度
  s.rest(5 * 60000);
  TEST_ASSERT_FALSE(s.d.upside_down());
}

static void test_millis_wraparound() {
  Sim s;
  s.now = 0xFFFFFFFF - 3000;
  s.rest(3000);
  for (int i = 0; i < 75; i++) {
    float x = 1.5f * sinf(2 * 3.14159f * 3 * i * 0.02f);
    s.step(x, 0, 1);
  }
  s.rest(2000);
  TEST_ASSERT_EQUAL(1, s.shakes);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_rest_no_events);
  RUN_TEST(test_shake);
  RUN_TEST(test_long_shake_respects_cooldown);
  RUN_TEST(test_desk_tap_ignored);
  RUN_TEST(test_braking_jolt);
  RUN_TEST(test_gentle_driving_ignored);
  RUN_TEST(test_upside_down_and_back);
  RUN_TEST(test_tilted_mount_is_normal);
  RUN_TEST(test_lying_flat_is_not_upside_down);
  RUN_TEST(test_millis_wraparound);
  return UNITY_END();
}
