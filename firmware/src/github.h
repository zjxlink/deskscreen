#pragma once

#include <ArduinoJson.h>
#include <stdint.h>

// 屏幕上显示最近多少周
static constexpr int kGithubWeeks = 24;
static constexpr uint8_t kGithubNoDay = 0xFF;

enum class GithubStatus : uint8_t {
  NoToken,   // 还没在设置页面填写 Token
  Loading,
  Ok,
  BadToken,  // Token 无效或已过期
  Error,     // 网络等其他错误，稍后自动重试
};

struct GithubData {
  GithubStatus status;
  int error;         // 状态为 Error 时的错误码（HTTP 状态码或负数），显示在屏幕上方便排查
  char login[40];
  uint16_t total;    // 过去一年的贡献数
  uint16_t today;
  uint16_t streak;   // 当前连续贡献天数（今天还没提交不算断）
  uint16_t longest;  // 过去一年最长连续天数
  // [周][星期几]，星期日为 0；值为 0~4 级，kGithubNoDay 表示这一格没有日期（未来的日子）
  uint8_t level[kGithubWeeks][7];
};

// 启动后台任务，每 30 分钟通过 GraphQL API 拉取一次贡献日历
void github_begin();

// Token 变更后调用，立即重新拉取
void github_request_refresh();

// 有新数据时返回 true 并写入 out，只在主循环中调用
bool github_take_update(GithubData &out);

// 以下在 github_parse.cpp 中实现，不依赖网络，可在电脑上测试

// 连续天数计算，counts 为按日期排列的每日贡献数，最后一个是今天
void github_streaks(const uint16_t *counts, int n, uint16_t &current, uint16_t &longest);

// GraphQL 响应只保留需要的字段，整份响应有几十 KB
void github_build_filter(JsonDocument &filter);

// 解析过滤后的响应，成功返回 Ok
GithubStatus github_parse(JsonDocument &doc, GithubData &out);
