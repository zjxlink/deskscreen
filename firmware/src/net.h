#pragma once

#include <Arduino.h>
#include <WiFiClientSecure.h>

// 让 mbedTLS 先从 PSRAM 分配内存，PSRAM 不够（或没有 PSRAM）时再用内部 RAM。
// Arduino 自带的 mbedTLS 默认只用内部 RAM，每个连接要 40~50KB，内部 RAM 碎片多了就握手失败。
// 要在第一次 HTTPS 连接之前调用
void net_tls_use_psram();

// 同一时间只允许一个 HTTPS 连接，免得几个后台任务同时握手，内存一下子不够
// 用法：在函数里建一个 HttpsLock 对象，函数返回时自动释放
class HttpsLock {
 public:
  HttpsLock();
  ~HttpsLock();
  HttpsLock(const HttpsLock &) = delete;
  HttpsLock &operator=(const HttpsLock &) = delete;
};

// TLS 握手超时（秒）。库里默认 120 秒：连不上的服务器（比如国内网络访问 GitHub 不稳定）
// 会占着 HttpsLock 两分钟，其他服务全部排队等着，屏幕上一直「获取中」
static constexpr unsigned long kTlsHandshakeTimeoutS = 15;

// Stock 是东方财富，StockBackup 是它失败时用的腾讯行情
enum class NetService : uint8_t { Weather, Github, Stock, StockBackup, Ota, Voice, Bing, Count };

// 返回内容不是预期的格式（JSON 解析失败等）
static constexpr int kNetBadData = -100;

// 记录一次请求的结果，设置页面的「网络诊断」会显示出来。
// code 为 HTTP 状态码（200 表示成功）、HTTPClient 的负数错误码或 kNetBadData；
// detail 是补充说明（如 JSON 解析错误），没有就传 nullptr。失败时同时打印日志
void net_report(NetService s, int code, WiFiClientSecure &client, const char *detail = nullptr);
// 不走 TLS 的请求用这个
void net_report(NetService s, int code, const char *detail = nullptr);

// 设置页面用的诊断信息（HTML 片段）：每项服务最近一次的结果、内部 RAM 和 PSRAM 剩余
String net_status_html();

// 开机后还没成功过时，重试间隔从 10 秒开始翻倍，最多到 max_ms，免得刚开机等太久
uint32_t net_retry_ms(int failures, uint32_t max_ms);
