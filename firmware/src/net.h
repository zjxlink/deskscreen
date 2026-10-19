#pragma once

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

// 请求失败时打印日志：HTTP 状态码或 HTTPClient 的错误、TLS 层的错误、内部 RAM 剩余，方便看串口定位问题
void net_log_failure(const char *what, int code, WiFiClientSecure &client);
