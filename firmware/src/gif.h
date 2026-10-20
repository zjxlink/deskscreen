#pragma once

#include <stddef.h>
#include <stdint.h>

// GIF 动图解码，不依赖硬件，可在电脑上测试。
// 整个文件放在内存里（相册从 TF 卡读进 PSRAM），一帧一帧画到调用者给的画布上。
// 画布是 GIF 原尺寸的 RGB565（正常字节序，和 LVGL 一样），透明的地方保持上一帧的内容，第一帧之前是黑色。
// 支持全局/局部调色板、透明色、隔行扫描和三种处理方式（保留、清成背景、恢复上一帧）

static constexpr int kGifMaxPixels = 640 * 480;  // 再大的动图不放，免得 PSRAM 不够（自己画的板子只有 2MB）

class GifDecoder {
 public:
  // 读文件头。不是 GIF、尺寸为 0 或太大时返回 false
  bool open(const uint8_t *data, size_t len);

  int width() const { return width_; }
  int height() const { return height_; }

  // 画下一帧，返回这一帧要显示多少毫秒；放完一遍或文件坏了时返回 -1，可以 rewind() 再放一遍。
  // canvas 有 width()*height() 个像素，每帧之间不要改；prev 同样大小，处理方式 3（恢复上一帧）要用，
  // 传 nullptr 时把它当成保留
  int next_frame(uint16_t *canvas, uint16_t *prev);

  // 后面还有没有帧（不移动读取位置）。只有一帧的 GIF 可以当照片放
  bool has_more() const;

  // 回到第一帧，下一次 next_frame 会先把画布清成黑色
  void rewind();

  // 已经放过的帧数（rewind 后从 0 开始）
  int frame_index() const { return frame_; }

 private:
  bool read_palette(uint16_t *pal, int count);
  bool decode_image(uint16_t *canvas, int fx, int fy, int fw, int fh, bool interlaced, const uint16_t *pal,
                    int pal_size, int transparent);
  bool skip_blocks();
  int byte();

  const uint8_t *data_ = nullptr;
  size_t len_ = 0;
  size_t pos_ = 0;
  size_t first_ = 0;  // 第一块的位置
  int width_ = 0;
  int height_ = 0;
  int frame_ = 0;
  bool clear_ = true;  // 下一帧之前要先清画布
  uint16_t global_[256];
  int global_size_ = 0;
  uint16_t local_[256];

  // 上一帧画完后要做的处理
  int dispose_ = 0;
  int dx_ = 0, dy_ = 0, dw_ = 0, dh_ = 0;

  // LZW 解码表
  uint16_t prefix_[4096];
  uint8_t suffix_[4096];
  uint8_t stack_[4097];
};

// 把 sw x sh 的画面缩放后居中放进 dw x dh：比屏幕大时缩小到刚好放下，
// 比屏幕小时放大整数倍（像素画不会糊），四周填黑色。都按最近点取样
void media_blit_fit(const uint16_t *src, int sw, int sh, uint16_t *dst, int dw, int dh);
