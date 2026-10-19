#pragma once

#include <stddef.h>

// 像素小猫：根据时间、GitHub 提交、自选股涨跌和番茄钟换心情。
// 不依赖硬件，可在电脑上测试。图案是 16x16 的像素画，界面放大后画到屏幕上

enum class PetMood {
  Normal,
  Happy,  // 今天有提交或完成了番茄
  Sad,    // 自选股平均跌超过 2%
  Bored,  // 下午了今天还没提交
  Sleep,  // 夜间时段
};

struct PetInputs {
  bool night = false;
  int minutes = -1;        // 当天第几分钟，未对时为 -1
  bool github_ok = false;  // GitHub 数据是否可用
  int github_today = 0;
  bool stocks_ok = false;  // 自选股数据是否可用
  float stock_avg_pct = 0;  // 自选股平均涨跌幅（%）
  int pomo_today = 0;
};

PetMood pet_mood(const PetInputs &in);

// 小猫说的话，写进 buf
void pet_line(PetMood mood, const PetInputs &in, char *buf, size_t len);

static constexpr int kPetSize = 16;

// 像素颜色编号：0 透明，其余见 kPetPalette
enum PetColor { PET_NONE, PET_LINE, PET_FUR, PET_WHITE, PET_PINK, PET_TEAR };

// 取 (x, y) 处的颜色编号。blink 为 true 时闭眼（眨眼动画的一帧）
int pet_pixel(PetMood mood, bool blink, int x, int y);
