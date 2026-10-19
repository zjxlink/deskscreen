// GitHub 贡献日历解析的单元测试，在电脑上运行：pio test -e native
#include <ArduinoJson.h>
#include <string.h>
#include <unity.h>

#include "github.h"

void setUp() {}
void tearDown() {}

static void check_streaks(std::initializer_list<uint16_t> counts, int current, int longest) {
  uint16_t c = 99, l = 99;
  github_streaks(counts.begin(), (int)counts.size(), c, l);
  TEST_ASSERT_EQUAL(current, c);
  TEST_ASSERT_EQUAL(longest, l);
}

static void test_streaks() {
  check_streaks({}, 0, 0);
  check_streaks({0, 0, 0}, 0, 0);
  check_streaks({1, 2, 3}, 3, 3);
  check_streaks({1, 1, 0, 1, 1, 0}, 2, 2);  // 今天还没提交，从昨天往前算
  check_streaks({1, 1, 1, 0, 0}, 0, 3);     // 昨天也没提交，已经断了
  check_streaks({1, 0, 1}, 1, 1);
}

static const char *kLevels[] = {"NONE", "FIRST_QUARTILE", "SECOND_QUARTILE", "THIRD_QUARTILE",
                                "FOURTH_QUARTILE"};

// 构造一份和 GraphQL 结构一样的日历：53 周，第一周从周三开始，最后一周到周一（今天）为止
static void build_calendar(JsonDocument &doc, int &total) {
  JsonObject v = doc["data"]["viewer"].to<JsonObject>();
  v["login"] = "octocat";
  JsonObject cal = v["contributionsCollection"]["contributionCalendar"].to<JsonObject>();
  JsonArray weeks = cal["weeks"].to<JsonArray>();
  total = 0;
  int day = 0;
  for (int w = 0; w < 53; w++) {
    JsonArray days = weeks.add<JsonObject>()["contributionDays"].to<JsonArray>();
    int from = w == 0 ? 3 : 0;
    int to = w == 52 ? 1 : 6;
    for (int wd = from; wd <= to; wd++, day++) {
      int count = day % 5 == 0 ? 0 : day % 4 + 1;  // 每 5 天断一次，最长连续 4 天
      JsonObject d = days.add<JsonObject>();
      d["contributionCount"] = count;
      d["contributionLevel"] = kLevels[count];
      d["weekday"] = wd;
      total += count;
    }
  }
  cal["totalContributions"] = total;
}

static void test_parse_calendar() {
  JsonDocument doc;
  int total;
  build_calendar(doc, total);

  GithubData g{};
  TEST_ASSERT_EQUAL(GithubStatus::Ok, github_parse(doc, g));
  TEST_ASSERT_EQUAL_STRING("octocat", g.login);
  TEST_ASSERT_EQUAL(total, g.total);

  // 一共 4 + 51*7 + 2 = 363 天，最后一天是第 362 天：362 % 5 = 2，贡献 362 % 4 + 1 = 3
  TEST_ASSERT_EQUAL(3, g.today);
  // 第 360 天为 0，之后 361、362 两天连续
  TEST_ASSERT_EQUAL(2, g.streak);
  TEST_ASSERT_EQUAL(4, g.longest);

  // 最后一列（本周）只有周日和周一
  const int last = kGithubWeeks - 1;
  TEST_ASSERT_EQUAL(3, g.level[last][1]);
  for (int wd = 2; wd < 7; wd++) TEST_ASSERT_EQUAL(kGithubNoDay, g.level[last][wd]);
  // 前面各列都是满的
  for (int w = 0; w < last; w++) {
    for (int wd = 0; wd < 7; wd++) TEST_ASSERT_TRUE(g.level[w][wd] <= 4);
  }
}

static void test_filter_drops_unused_fields() {
  const char *json =
      "{\"data\":{\"viewer\":{\"login\":\"a\",\"name\":\"x\",\"contributionsCollection\":"
      "{\"contributionCalendar\":{\"totalContributions\":1,\"colors\":[\"#fff\"],\"weeks\":"
      "[{\"firstDay\":\"2026-10-18\",\"contributionDays\":[{\"contributionCount\":1,"
      "\"contributionLevel\":\"FOURTH_QUARTILE\",\"weekday\":0,\"date\":\"2026-10-18\","
      "\"color\":\"#39d353\"}]}]}}}}}";
  JsonDocument filter;
  github_build_filter(filter);
  JsonDocument doc;
  TEST_ASSERT_FALSE(deserializeJson(doc, json, DeserializationOption::Filter(filter)));
  TEST_ASSERT_TRUE(doc["data"]["viewer"]["name"].isNull());
  JsonObject day = doc["data"]["viewer"]["contributionsCollection"]["contributionCalendar"]["weeks"][0]
                      ["contributionDays"][0];
  TEST_ASSERT_TRUE(day["date"].isNull());
  TEST_ASSERT_EQUAL_STRING("FOURTH_QUARTILE", day["contributionLevel"].as<const char *>());

  GithubData g{};
  TEST_ASSERT_EQUAL(GithubStatus::Ok, github_parse(doc, g));
  TEST_ASSERT_EQUAL(4, g.level[kGithubWeeks - 1][0]);
  TEST_ASSERT_EQUAL(kGithubNoDay, g.level[0][0]);  // 新账号不足 24 周，前面都是空位
}

static void test_error_response() {
  JsonDocument filter;
  github_build_filter(filter);
  JsonDocument doc;
  deserializeJson(doc, "{\"data\":null,\"errors\":[{\"message\":\"Bad credentials\",\"type\":\"X\"}]}",
                  DeserializationOption::Filter(filter));
  TEST_ASSERT_EQUAL_STRING("Bad credentials", doc["errors"][0]["message"].as<const char *>());
  TEST_ASSERT_TRUE(doc["errors"][0]["type"].isNull());
  GithubData g{};
  TEST_ASSERT_EQUAL(GithubStatus::Error, github_parse(doc, g));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_streaks);
  RUN_TEST(test_parse_calendar);
  RUN_TEST(test_filter_drops_unused_fields);
  RUN_TEST(test_error_response);
  return UNITY_END();
}
