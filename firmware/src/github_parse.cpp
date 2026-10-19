#include <ArduinoJson.h>
#include <string.h>

#include "github.h"

static constexpr int kMaxDays = 7 * 54;  // 日历最多 53 周多一点

static uint8_t level_from(const char *s) {
  if (!s) return 0;
  if (!strcmp(s, "FIRST_QUARTILE")) return 1;
  if (!strcmp(s, "SECOND_QUARTILE")) return 2;
  if (!strcmp(s, "THIRD_QUARTILE")) return 3;
  if (!strcmp(s, "FOURTH_QUARTILE")) return 4;
  return 0;
}

void github_streaks(const uint16_t *counts, int n, uint16_t &current, uint16_t &longest) {
  current = 0;
  longest = 0;
  uint16_t run = 0;
  for (int i = 0; i < n; i++) {
    run = counts[i] > 0 ? run + 1 : 0;
    if (run > longest) longest = run;
  }

  // 和 GitHub 一样：今天还没提交时，从昨天开始往回数
  int i = n - 1;
  if (i >= 0 && counts[i] == 0) i--;
  while (i >= 0 && counts[i] > 0) {
    current++;
    i--;
  }
}

void github_build_filter(JsonDocument &filter) {
  JsonObject viewer = filter["data"]["viewer"].to<JsonObject>();
  viewer["login"] = true;
  JsonObject cal = viewer["contributionsCollection"]["contributionCalendar"].to<JsonObject>();
  cal["totalContributions"] = true;
  JsonObject day = cal["weeks"][0]["contributionDays"][0].to<JsonObject>();
  day["contributionCount"] = true;
  day["contributionLevel"] = true;
  day["weekday"] = true;
}

GithubStatus github_parse(JsonDocument &doc, GithubData &out) {
  JsonObject v = doc["data"]["viewer"];
  JsonObject cal = v["contributionsCollection"]["contributionCalendar"];
  JsonArray weeks = cal["weeks"];
  if (v.isNull() || weeks.isNull()) return GithubStatus::Error;

  strncpy(out.login, v["login"] | "", sizeof(out.login) - 1);
  out.login[sizeof(out.login) - 1] = 0;
  out.total = cal["totalContributions"] | 0;
  memset(out.level, kGithubNoDay, sizeof(out.level));

  static uint16_t counts[kMaxDays];
  int n = 0;
  int first_week = (int)weeks.size() - kGithubWeeks;
  int w = 0;
  for (JsonObject week : weeks) {
    for (JsonObject d : week["contributionDays"].as<JsonArray>()) {
      if (n < kMaxDays) counts[n++] = d["contributionCount"] | 0;
      int col = w - first_week;
      int row = d["weekday"] | 0;
      if (col >= 0 && row >= 0 && row < 7) {
        out.level[col][row] = level_from(d["contributionLevel"].as<const char *>());
      }
    }
    w++;
  }

  out.today = n > 0 ? counts[n - 1] : 0;
  github_streaks(counts, n, out.streak, out.longest);
  return GithubStatus::Ok;
}
