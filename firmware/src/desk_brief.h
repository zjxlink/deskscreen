#pragma once

#include <stdint.h>

#include <string>

#include "github.h"
#include "pomodoro.h"
#include "stock.h"
#include "weather.h"

// 聊天时告诉小橘的桌面信息：天气、室内温湿度、番茄钟、自选股、GitHub。没有的项填 nullptr 或 false
struct DeskInfo {
  const WeatherData *weather = nullptr;
  bool room_valid = false;
  float room_temp = 0;
  float room_humidity = 0;
  const Pomodoro *pomo = nullptr;
  uint32_t now_ms = 0;
  int pomo_today = 0;
  const StockData *stocks = nullptr;
  const GithubData *github = nullptr;
};

// 拼成几行中文，每行一项；什么都没有时返回空字符串。不依赖硬件，可在电脑上测试
std::string desk_brief(const DeskInfo &d);
