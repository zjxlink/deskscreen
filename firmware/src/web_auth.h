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
// 配对码被输错作废后，要等一会儿才能出新码，每作废一个等待加倍（30 秒起，最多 1 小时），
// 免得同一个网络里的程序不停要新码来猜。配对成功后清零
static constexpr uint32_t kWebAuthLockMs = 30000;
static constexpr uint32_t kWebAuthLockMaxMs = 3600000;

struct WebAuth {
  char code[7] = "";       // 当前配对码，空表示没有
  uint32_t code_at = 0;    // 显示配对码的时间（毫秒）
  int tries = 0;
  int burned = 0;          // 连续输错作废了几个配对码
  uint32_t lock_at = 0;    // 最近一次作废的时间
  char tokens[kWebAuthMaxTokens][kWebAuthTokenLen + 1] = {};
  int count = 0;
};

// rnd 返回 32 位随机数（板子上用硬件随机数 esp_random）
typedef uint32_t (*WebAuthRandom)();

// 开始配对：还有有效配对码就继续用它（反复刷新网页不会换码），否则生成新的。返回配对码；
// 输错太多次还在等待中时返回 nullptr（web_auth_locked_ms 是还要等多久）
const char *web_auth_start(WebAuth &a, uint32_t now_ms, WebAuthRandom rnd);

// 输错作废配对码后还要等多少毫秒才能出新码，0 表示不用等
uint32_t web_auth_locked_ms(const WebAuth &a, uint32_t now_ms);

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

// 浏览器发来的 Host 是不是局域网里的地址（IP、deskscreen.local、路由器给的 xxx.lan 这类）。
// 外网域名一律不认：防止恶意网页用 DNS 重绑定，借访客的浏览器来猜配对码、改设置
bool web_auth_host_ok(const char *host);
