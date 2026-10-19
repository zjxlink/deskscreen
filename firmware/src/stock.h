#pragma once

#include <ArduinoJson.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

static constexpr int kMaxStocks = 6;

typedef char StockCode[12];

enum class StockStatus : uint8_t {
  NotSet,  // 还没在设置页面填写自选股
  Loading,
  Ok,
  Error,   // 网络等错误，稍后自动重试
};

struct StockQuote {
  StockCode code;   // 交易所的代码，如 600519、00700、AAPL；没查到时是用户填写的原文
  char name[32];    // UTF-8，过长会在字符边界截断
  float price;
  float change;
  float change_pct;
  bool valid;       // false 表示代码不存在或停牌
};

struct StockData {
  StockStatus status;
  int error;        // 状态为 Error 时的错误码（HTTP 状态码或负数），显示在屏幕上方便排查
  uint8_t count;
  StockQuote quotes[kMaxStocks];
  char updated[6];  // 最后更新时间 HH:MM
};

// 启动后台任务，交易时段每 30 秒刷新一次行情（数据来自东方财富）
void stock_begin();

// 设置变更后调用，立即重新拉取
void stock_request_refresh();

// 有新数据时返回 true 并写入 out，只在主循环中调用
bool stock_take_update(StockData &out);

// 以下在 stock_parse.cpp 中实现，不依赖网络，可在电脑上测试

// 拆分设置里的代码列表（逗号、空格、顿号分隔），最多 max 个，返回个数
int stock_split_codes(const char *input, StockCode *codes, int max);

// 拼出请求用的 secids 参数（如 "1.600519,0.000001,116.00700"），美股会同时查三个交易所
// 返回 false 表示缓冲区不够
bool stock_build_secids(const StockCode *codes, int n, char *buf, size_t len);

// 按 codes 的顺序解析行情响应
StockStatus stock_parse(JsonDocument &doc, const StockCode *codes, int n, StockData &out);

// 根据北京时间决定下次刷新间隔：A 股/港股、美股交易时段 30 秒，其余 10 分钟
uint32_t stock_refresh_ms(const struct tm &now);
