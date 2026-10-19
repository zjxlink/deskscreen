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

// 东方财富负载高时会 302 跳转到 push2delay 等备用域名，Location 不带协议（//push2delay.eastmoney.com/...）。
// scheme 是当前请求用的协议（"http" 或 "https"），跳转后沿用同一个协议，因为连接对象是按协议建的。
// 返回拼在 location 前面得到完整地址的前缀（如 "http:"、"" 或带主机名的前缀），
// 不是东方财富的地址或者协议不同时返回 nullptr，不跟随
const char *stock_redirect_prefix(const char *location, const char *scheme);

// 按 codes 的顺序解析行情响应
StockStatus stock_parse(JsonDocument &doc, const StockCode *codes, int n, StockData &out);

// 腾讯行情（备用数据源）的查询参数，如 "sh600519,bj430047,hk00700,usAAPL"；格式不对的代码跳过。
// 返回 false 表示缓冲区不够
bool stock_build_tencent_query(const StockCode *codes, int n, char *buf, size_t len);

// 解析腾讯行情的响应（每只一行 v_sh600519="1~贵州茅台~600519~价格~...";），按 codes 的顺序填入 out。
// 没查到的代码和东方财富一样显示用户填写的原文。body 里一行行情都没有时返回 Error
StockStatus stock_parse_tencent(const char *body, const StockCode *codes, int n, StockData &out);

// 东方财富连续失败 failures 次后，隔多久再试：5 分钟起翻倍，最多 30 分钟
uint32_t stock_primary_cooldown_ms(int failures);

// 根据北京时间决定下次刷新间隔：A 股/港股、美股交易时段 1 分钟，其余 10 分钟
uint32_t stock_refresh_ms(const struct tm &now);
