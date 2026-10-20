#include "pet.h"

#include <math.h>
#include <stdio.h>

PetMood pet_mood(const PetInputs &in) {
  if (in.night) return PetMood::Sleep;
  if (in.stocks_ok && in.stock_avg_pct <= -2.0f) return PetMood::Sad;
  if ((in.github_ok && in.github_today > 0) || in.pomo_today > 0) return PetMood::Happy;
  if (in.github_ok && in.minutes >= 15 * 60) return PetMood::Bored;
  return PetMood::Normal;
}

void pet_line(PetMood mood, const PetInputs &in, char *buf, size_t len) {
  switch (mood) {
    case PetMood::Sleep:
      snprintf(buf, len, "呼……睡着了");
      break;
    case PetMood::Sad:
      snprintf(buf, len, "自选股跌了 %.1f%%，有点难过", fabsf(in.stock_avg_pct));
      break;
    case PetMood::Happy:
      if (in.github_ok && in.github_today > 0) {
        snprintf(buf, len, "今天提交了 %d 次，真棒！", in.github_today);
      } else {
        snprintf(buf, len, "今天专注了 %d 个番茄，真棒！", in.pomo_today);
      }
      break;
    case PetMood::Bored:
      snprintf(buf, len, "今天还没提交代码哦");
      break;
    case PetMood::Normal:
      snprintf(buf, len, "陪你一起写代码");
      break;
    case PetMood::Dizzy:
      snprintf(buf, len, "晕……别摇啦");
      break;
    case PetMood::Startled:
      snprintf(buf, len, "哇！吓我一跳");
      break;
    case PetMood::Flipped:
      snprintf(buf, len, "我倒过来了！快把我放正");
      break;
  }
}

// 左半边，右半边左右镜像。. 透明 k 轮廓 o 橘色毛 w 白色 p 粉色 b 眼泪
static const char *const kBody[kPetSize] = {
    "........",  //
    "..k.....",  // 耳朵
    "..kk....",  //
    "..kok...",  //
    "..kookkk",  // 头顶
    ".koooooo",  //
    nullptr,     // 眼睛（按心情替换）
    nullptr,     //
    nullptr,     // 眼泪
    ".kooooop",  // 鼻子
    nullptr,     // 嘴
    "...kkkkk",  // 下巴
    "..kooooo",  // 身体
    ".kooooww",  //
    ".kooooww",  //
    "..kkkkkk",  //
};

static const char *eye_row(PetMood mood, bool blink, int y) {
  bool closed = blink || mood == PetMood::Sleep;
  switch (mood) {
    case PetMood::Dizzy:
      return y == 6 ? ".kkokooo" : ".kokoooo";  // x x（第三行在 row() 里）
    case PetMood::Startled:
    case PetMood::Flipped:
      return y == 6 ? ".kokkkoo" : ".kokwkoo";  // 瞪大眼睛（第三行在 row() 里）
    case PetMood::Happy:
      return y == 6 ? ".kookooo" : ".kokokoo";  // ^ ^
    case PetMood::Bored:
      if (!closed) return y == 6 ? ".kokkkoo" : ".kookkoo";  // 眼皮耷拉着
      break;
    default:
      break;
  }
  if (closed) return y == 6 ? ".koooooo" : ".kookkoo";
  return ".kookkoo";
}

static const char *row(PetMood mood, bool blink, int y) {
  if (y == 6 || y == 7) return eye_row(mood, blink, y);
  if (y == 8) {
    switch (mood) {
      case PetMood::Sad: return ".koobooo";
      case PetMood::Dizzy: return ".kkokooo";
      case PetMood::Startled:
      case PetMood::Flipped: return ".kokkkoo";
      default: return ".koooooo";
    }
  }
  if (y == 10) {
    switch (mood) {
      case PetMood::Happy: return "..koowkw";  // 咧嘴笑
      case PetMood::Sad: return "..kooowk";    // 嘴角向下
      case PetMood::Dizzy: return "..kookok";  // 波浪嘴
      case PetMood::Startled:
      case PetMood::Flipped: return "..koookk";  // 张大嘴
      default: return "..koooww";
    }
  }
  return kBody[y];
}

int pet_pixel(PetMood mood, bool blink, int x, int y) {
  if (x < 0 || y < 0 || x >= kPetSize || y >= kPetSize) return PET_NONE;
  int half = kPetSize / 2;
  char c = row(mood, blink, y)[x < half ? x : kPetSize - 1 - x];
  switch (c) {
    case 'k': return PET_LINE;
    case 'o': return PET_FUR;
    case 'w': return PET_WHITE;
    case 'p': return PET_PINK;
    case 'b': return PET_TEAR;
    default: return PET_NONE;
  }
}
