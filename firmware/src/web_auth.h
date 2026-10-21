#pragma once

#include <stddef.h>
#include <stdint.h>

// 设置网页的配对登录，不依赖硬件，可在电脑上测试。
// 手机第一次打开设置网页时，屏幕上显示 6 位配对码；网页里输对了，设备发给这台手机一个随机令牌
// （存在浏览器 Cookie 里），以后带着令牌就不用再配对。设备最多记住 kWebAuthMaxTokens 台手机，
// 多了挤掉最早的那台。能看到屏幕的人才能配对，同一个 WiFi 里的其他人改不了设置、也传不了固件
static constexpr int kWebAuthMaxTokens = 8;
static constexpr int kWebAuthTokenLen = 32;          // 16 字节随机数的十六进制
static constexpr int kWebAuthMaxTries = 5;           // 一个配对码最多输错几次，之后要重新显示新码
static constexpr uint32_t kWebAuthCodeMs = 120000;   // 配对码有效 2 分钟

struct WebAuth {
  char code[7] = "";       // 当前配对码，空表示没有
  uint32_t code_at = 0;    // 显示配对码的时间（毫秒）
  int tries = 0;
  char tokens[kWebAuthMaxTokens][kWebAuthTokenLen + 1] = {};
  int count = 0;
};

// rnd 返回 32 位随机数（板子上用硬件随机数 esp_random）
typedef uint32_t (*WebAuthRandom)();

// 开始配对：还有有效配对码就继续用它（反复刷新网页不会换码），否则生成新的。返回配对码
const char *web_auth_start(WebAuth &a, uint32_t now_ms, WebAuthRandom rnd);

// 配对码是否正在显示（没过期、没输错太多次）
bool web_auth_code_active(const WebAuth &a, uint32_t now_ms);

// 输入配对码。对了返回 true，生成新令牌写进 token_out（kWebAuthTokenLen+1 字节）并作废配对码；
// 错了累计次数，错满 kWebAuthMaxTries 次作废配对码
bool web_auth_pair(WebAuth &a, const char *code, uint32_t now_ms, WebAuthRandom rnd, char *token_out);

// 令牌是否有效（逐字节比较完，不因提前结束泄露信息）
bool web_auth_check(const WebAuth &a, const char *token);

// 忘掉所有已配对的手机
void web_auth_forget_all(WebAuth &a);

// 令牌存到 NVS 的格式：用逗号连接。load 会跳过格式不对的令牌
void web_auth_save_tokens(const WebAuth &a, char *out, size_t n);
void web_auth_load_tokens(WebAuth &a, const char *s);

// 从 Cookie 头里取出 name 的值（如 "a=1; ds_auth=xxx"）。没有或放不下时返回 false
bool web_auth_cookie(const char *header, const char *name, char *out, size_t n);
