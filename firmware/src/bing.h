#pragma once

#include <ArduinoJson.h>
#include <stddef.h>
#include <stdint.h>

// 必应每日一图：后台任务从 cn.bing.com 拉最近几天的壁纸（320x240 的版本，正好是屏幕大小，每张十几 KB），
// 存到 TF 卡的 bing 文件夹，和自己的照片（photos 文件夹）分开放。没插卡时只把当天这一张放在 PSRAM。
// 每天零点多拉一次（必应北京时间零点换图），WiFi 刚连上、插上卡、在设置里打开时也会拉一次

// 接口一次最多给 8 张（idx=0&n=8）
static constexpr int kBingApiImages = 8;
// TF 卡上最多存这么多张，多了先删最早的
static constexpr int kBingMaxImages = 60;

// 启动后台任务
void bing_begin();

// 设置改了或者 WiFi 刚连上时调用，立即看看要不要拉
void bing_kick();

// 以下在 bing_parse.cpp 中实现，不依赖网络，可在电脑上测试

struct BingImage {
  char date[9];       // 这张图是哪天的，如 20261020，也用作文件名
  char path[128];     // 320x240 版本的地址（不含域名），如 /th?id=OHR.Xxx_ZH-CN123_320x240.jpg
  char caption[160];  // 屏幕底部显示的说明，如「极光下的教堂 · 冰岛维克」
};

// 接口返回的 JSON 只保留需要的字段
void bing_build_filter(JsonDocument &filter);

// 解析过滤后的响应，最新的在前。格式不对的条目跳过，返回有效条数
int bing_parse(JsonDocument &doc, BingImage *out, int max);

// 说明文字：标题 + 拍摄地点（copyright 去掉后面的「(© 摄影师)」）。按 UTF-8 字符截断，不会切半个汉字
void bing_caption(const char *title, const char *copyright, char *out, size_t size);

// 8 位数字的日期才算合法（也防止拿它拼出别的路径）
bool bing_is_date(const char *s);

// TF 卡上的文件名是不是必应图片：8 位日期 + .jpg
bool bing_is_file(const char *name);

// 下一张从哪里放：0 是自己的照片，1 是必应，-1 表示两边都没有。两边都有时交替
int album_next_source(int last, bool has_local, bool has_bing);
