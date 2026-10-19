#include "weather.h"

// https://open-meteo.com/en/docs 中的 WMO 天气代码
const char *weather_text(int code) {
  switch (code) {
    case 0: return "晴";
    case 1: return "少云";
    case 2: return "多云";
    case 3: return "阴";
    case 45:
    case 48: return "雾";
    case 51:
    case 53:
    case 55: return "毛毛雨";
    case 56:
    case 57:
    case 66:
    case 67: return "冻雨";
    case 61: return "小雨";
    case 63: return "中雨";
    case 65: return "大雨";
    case 71: return "小雪";
    case 73: return "中雪";
    case 75: return "大雪";
    case 77: return "雪粒";
    case 80:
    case 81: return "阵雨";
    case 82: return "强阵雨";
    case 85:
    case 86: return "阵雪";
    case 95: return "雷阵雨";
    case 96:
    case 99: return "雷雨冰雹";
    default: return "未知";
  }
}

uint32_t weather_color(int code) {
  if (code <= 1) return 0xFFB74D;                  // 晴：橙
  if (code <= 3) return 0x90A4AE;                  // 云：灰蓝
  if (code == 45 || code == 48) return 0xB0BEC5;   // 雾
  if ((code >= 71 && code <= 77) || code == 85 || code == 86) return 0xE0F7FA;  // 雪
  if (code >= 95) return 0xCE93D8;                 // 雷：紫
  return 0x4FC3F7;                                 // 雨：蓝
}
