// 聊天时告诉小橘的桌面信息的单元测试，在电脑上运行：pio test -e native
#include <string.h>
#include <unity.h>

#include "desk_brief.h"

void setUp() {}
void tearDown() {}

static bool has(const std::string &s, const char *part) { return s.find(part) != std::string::npos; }

static void test_empty() {
  DeskInfo d;
  TEST_ASSERT_EQUAL_STRING("", desk_brief(d).c_str());
  Pomodoro p;
  d.pomo = &p;  // 没开始、今天也没完成的番茄钟不用说
  TEST_ASSERT_EQUAL_STRING("", desk_brief(d).c_str());
}

static void test_weather() {
  WeatherData w = {};
  w.temp = 18.4f;
  w.humidity = 60;
  w.code = 3;
  w.ndays = 3;
  w.days[0] = {3, 20.2f, 11.8f, 0, 0, 2, 400, 1100};
  w.days[1] = {61, 17, 11, 80, 3, 3, 400, 1100};
  w.days[2] = {0, 22, 12, 0, 0, 4, 400, 1100};
  DeskInfo d;
  d.weather = &w;
  std::string s = desk_brief(d);
  TEST_ASSERT_TRUE(has(s, "现在 18°C"));
  TEST_ASSERT_TRUE(has(s, "湿度 60%"));
  TEST_ASSERT_TRUE(has(s, "今天"));
  TEST_ASSERT_TRUE(has(s, "12~20°C"));
  TEST_ASSERT_TRUE(has(s, "明天"));
  TEST_ASSERT_TRUE(has(s, "降水概率 80%"));
  TEST_ASSERT_TRUE(has(s, "后天"));
  w.error = 1;  // 拉取失败时不说
  TEST_ASSERT_EQUAL_STRING("", desk_brief(d).c_str());
}

static void test_pomo_stocks_github() {
  Pomodoro p;
  p.toggle(0);
  StockData st = {};
  st.status = StockStatus::Ok;
  st.count = 2;
  strcpy(st.updated, "10:32");
  strcpy(st.quotes[0].name, "贵州茅台");
  st.quotes[0].price = 1500.5f;
  st.quotes[0].change_pct = 1.23f;
  st.quotes[0].valid = true;
  st.quotes[1].valid = false;  // 停牌的不说
  GithubData g = {};
  g.status = GithubStatus::Ok;
  g.today = 3;
  g.streak = 12;
  g.total = 800;
  DeskInfo d;
  d.pomo = &p;
  d.now_ms = 5 * 60 * 1000;
  d.pomo_today = 2;
  d.stocks = &st;
  d.github = &g;
  d.room_valid = true;
  d.room_temp = 23.46f;
  d.room_humidity = 45;
  std::string s = desk_brief(d);
  TEST_ASSERT_TRUE(has(s, "室内：23.5°C，湿度 45%"));
  TEST_ASSERT_TRUE(has(s, "番茄钟：专注中，还剩 20 分钟，今天完成了 2 个"));
  TEST_ASSERT_TRUE(has(s, "自选股（10:32 更新）：贵州茅台 1500.50（+1.23%）。"));
  TEST_ASSERT_TRUE(has(s, "GitHub：今天 3 次贡献，已经连续 12 天"));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_empty);
  RUN_TEST(test_weather);
  RUN_TEST(test_pomo_stocks_github);
  return UNITY_END();
}
