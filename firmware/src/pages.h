#pragma once

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
  PAGE_COUNT,
};
