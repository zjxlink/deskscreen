// 小橘跑酷的单元测试：在电脑上运行：pio test -e native
#include <unity.h>

#include "runner.h"

void setUp() {}
void tearDown() {}

static void run(RunnerGame &g, uint32_t &now, uint32_t ms, bool autopilot = false) {
  for (uint32_t t = 0; t < ms && g.state() == RunnerGame::State::Playing; t += 16) {
    now += 16;
    if (autopilot) {
      // 前面有黄瓜靠近时跳
      for (int i = 0; i < RunnerGame::kMaxObstacles; i++) {
        const auto &o = g.obstacle(i);
        float lead = g.speed() * 0.14f;
        if (o.active && o.x > RunnerGame::kCatX + RunnerGame::kCatSize - 6 &&
            o.x < RunnerGame::kCatX + RunnerGame::kCatSize + lead)
          g.tap(now);
      }
    }
    g.tick(now);
  }
}

static void test_ready_until_tap() {
  RunnerGame g;
  uint32_t now = 1000;
  g.tick(now + 5000);
  TEST_ASSERT_TRUE(g.state() == RunnerGame::State::Ready);
  g.tap(now);
  TEST_ASSERT_TRUE(g.state() == RunnerGame::State::Playing);
}

static void test_no_jump_eventually_crashes() {
  RunnerGame g;
  uint32_t now = 1000;
  g.tap(now);
  run(g, now, 20000);
  TEST_ASSERT_TRUE(g.state() == RunnerGame::State::Over);
  TEST_ASSERT_TRUE(g.score() > 0);
}

static void test_jump_arc() {
  RunnerGame g;
  uint32_t now = 1000;
  g.tap(now);
  TEST_ASSERT_TRUE(g.on_ground());
  g.tap(now);  // 跳
  float top = g.cat_y();
  uint32_t landed = 0;
  for (int i = 0; i < 100; i++) {
    now += 10;
    g.tick(now);
    if (g.cat_y() < top) top = g.cat_y();
    if (i > 5 && g.on_ground() && !landed) landed = i * 10;
  }
  float height = (RunnerGame::kGroundY - RunnerGame::kCatSize) - top;
  TEST_ASSERT_TRUE(height > 70 && height < 110);
  TEST_ASSERT_TRUE(landed > 550 && landed < 850);
}

static void test_no_double_jump() {
  RunnerGame g;
  uint32_t now = 1000;
  g.tap(now);
  g.tap(now);
  now += 200;
  g.tick(now);
  float y = g.cat_y();
  g.tap(now);  // 空中再点不会再跳
  now += 10;
  g.tick(now);
  TEST_ASSERT_TRUE(g.cat_y() >= y - 6);
}

// 每个间隔都跳得过去：自动驾驶能跑很久
static void test_always_jumpable() {
  for (uint32_t seed = 1; seed <= 20; seed++) {
    RunnerGame g(seed);
    uint32_t now = 1000;
    g.tap(now);
    run(g, now, 120000, true);
    TEST_ASSERT_TRUE_MESSAGE(g.state() == RunnerGame::State::Playing, "autopilot crashed");
  }
}

static void test_speeds_up_and_caps() {
  RunnerGame g;
  uint32_t now = 1000;
  g.tap(now);
  float s0 = g.speed();
  run(g, now, 10000, true);
  TEST_ASSERT_TRUE(g.speed() > s0);
  run(g, now, 120000, true);
  TEST_ASSERT_TRUE(g.speed() <= 330.01f);
}

static void test_restart_guard() {
  RunnerGame g;
  uint32_t now = 1000;
  g.tap(now);
  run(g, now, 30000);
  TEST_ASSERT_TRUE(g.state() == RunnerGame::State::Over);
  g.tap(now + 100);  // 撞上后马上点，不算
  TEST_ASSERT_TRUE(g.state() == RunnerGame::State::Over);
  g.tap(now + 700);
  TEST_ASSERT_TRUE(g.state() == RunnerGame::State::Playing);
  TEST_ASSERT_EQUAL(0, g.score());
}

static void test_long_stall_does_not_tunnel() {
  RunnerGame g;
  uint32_t now = 1000;
  g.tap(now);
  // 主循环卡了 5 秒（比如在连 WiFi）：最多只推进 0.2 秒
  now += 5000;
  g.tick(now);
  TEST_ASSERT_TRUE(g.score() < 10);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_ready_until_tap);
  RUN_TEST(test_no_jump_eventually_crashes);
  RUN_TEST(test_jump_arc);
  RUN_TEST(test_no_double_jump);
  RUN_TEST(test_always_jumpable);
  RUN_TEST(test_speeds_up_and_caps);
  RUN_TEST(test_restart_guard);
  RUN_TEST(test_long_stall_does_not_tunnel);
  return UNITY_END();
}
