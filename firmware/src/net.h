#pragma once

#include <Arduino.h>
#include <WiFiClientSecure.h>

// 同一时间只允许一个 HTTPS 连接。
// Arduino 自带的 mbedTLS 只从内部 RAM 分配内存（不用 PSRAM），每个连接要 40~50KB，
// 天气、GitHub、自选股、OTA 几个后台任务同时连接会把内部 RAM 耗尽，结果全部握手失败。
// 用法：在函数里建一个 HttpsLock 对象，函数返回时自动释放
class HttpsLock {
 public:
  HttpsLock();
  ~HttpsLock();
  HttpsLock(const HttpsLock &) = delete;
  HttpsLock &operator=(const HttpsLock &) = delete;
};

enum class NetService : uint8_t { Weather, Github, Stock, Ota, Count };

// 返回内容不是预期的格式（JSON 解析失败等）
static constexpr int kNetBadData = -100;

// 记录一次请求的结果，设置页面的「网络诊断」会显示出来。
// code 为 HTTP 状态码（200 表示成功）、HTTPClient 的负数错误码或 kNetBadData；失败时同时打印日志
void net_report(NetService s, int code, WiFiClientSecure &client);

// 设置页面用的诊断信息（HTML 片段）：每项服务最近一次的结果、内部 RAM 剩余
String net_status_html();

// 开机后还没成功过时，重试间隔从 10 秒开始翻倍，最多到 max_ms，免得刚开机等太久
uint32_t net_retry_ms(int failures, uint32_t max_ms);
