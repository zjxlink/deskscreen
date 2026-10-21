#pragma once

#include <stddef.h>
#include <stdint.h>

enum Page {
  PAGE_CLOCK,
  PAGE_WEATHER,  // 未来 3 天和 12 小时降水
  PAGE_NEWS,     // 资讯（没选来源时跳过）
  PAGE_GITHUB,
  PAGE_STOCKS,
  PAGE_PET,  // 像素小猫
  PAGE_POMODORO,
  PAGE_CHAT,  // AI 语音聊天
  PAGE_ALBUM,  // 电子相册（TF 卡）
  PAGE_MOVIE,  // 影片（TF 卡 movies 文件夹）
  PAGE_COUNT,
};

// 每页的英文名（存设置、网页表单用，顺序变了也不影响已保存的设置）和中文名（快捷设置、网页上显示）。
// 加新页面时在这里加一行
struct PageInfo {
  const char *key;
  const char *name;
};
extern const PageInfo kPageInfo[PAGE_COUNT];

// 隐藏的页面存成英文名列表，逗号分隔，如 "github,album"。时钟页不能隐藏
uint32_t pages_parse_hidden(const char *list);
void pages_format_hidden(uint32_t mask, char *out, size_t n);
int page_by_key(const char *key);  // 找不到返回 -1
