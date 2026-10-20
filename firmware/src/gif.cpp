#include "gif.h"

#include <string.h>

static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)((r & 0xF8) << 8 | (g & 0xFC) << 3 | b >> 3);
}

int GifDecoder::byte() {
  return pos_ < len_ ? data_[pos_++] : -1;
}

bool GifDecoder::read_palette(uint16_t *pal, int count) {
  if (pos_ + count * 3 > len_) return false;
  for (int i = 0; i < count; i++, pos_ += 3) pal[i] = rgb565(data_[pos_], data_[pos_ + 1], data_[pos_ + 2]);
  return true;
}

// 跳过一串子块（每块开头一个字节是长度，长度 0 结束）
bool GifDecoder::skip_blocks() {
  for (;;) {
    int n = byte();
    if (n < 0) return false;
    if (n == 0) return true;
    pos_ += n;
    if (pos_ > len_) return false;
  }
}

bool GifDecoder::open(const uint8_t *data, size_t len) {
  data_ = data;
  len_ = len;
  width_ = height_ = 0;
  if (!data || len < 13 || memcmp(data, "GIF8", 4) != 0 || (data[4] != '7' && data[4] != '9') || data[5] != 'a')
    return false;
  int w = data[6] | data[7] << 8;
  int h = data[8] | data[9] << 8;
  if (w <= 0 || h <= 0 || (long)w * h > kGifMaxPixels) return false;
  uint8_t packed = data[10];
  pos_ = 13;
  global_size_ = 0;
  if (packed & 0x80) {
    global_size_ = 2 << (packed & 7);
    if (!read_palette(global_, global_size_)) return false;
  }
  width_ = w;
  height_ = h;
  first_ = pos_;
  rewind();
  return true;
}

bool GifDecoder::has_more() const {
  size_t p = pos_;
  while (p < len_ && data_[p] == 0x21) {  // 跳过扩展块
    p += 2;
    while (p < len_ && data_[p] != 0) p += data_[p] + 1;
    p++;
  }
  return p < len_ && data_[p] == 0x2C;
}

void GifDecoder::rewind() {
  pos_ = first_;
  frame_ = 0;
  clear_ = true;
  dispose_ = 0;
}

int GifDecoder::next_frame(uint16_t *canvas, uint16_t *prev) {
  if (!data_ || width_ == 0) return -1;
  int n = width_ * height_;
  if (clear_) {
    for (int i = 0; i < n; i++) canvas[i] = 0;
    clear_ = false;
  } else if (dispose_ == 2) {  // 清成背景（黑色）
    for (int y = dy_; y < dy_ + dh_; y++) memset(canvas + y * width_ + dx_, 0, dw_ * sizeof(uint16_t));
  } else if (dispose_ == 3 && prev) {  // 恢复成画这一帧之前的样子
    for (int y = dy_; y < dy_ + dh_; y++)
      memcpy(canvas + y * width_ + dx_, prev + y * width_ + dx_, dw_ * sizeof(uint16_t));
  }
  dispose_ = 0;

  int delay_cs = 0, gce_dispose = 0, transparent = -1;
  for (;;) {
    int b = byte();
    if (b == 0x21) {  // 扩展块
      int label = byte();
      if (label == 0xF9 && pos_ + 5 <= len_ && data_[pos_] >= 4) {  // 图形控制：处理方式、延时、透明色
        uint8_t packed = data_[pos_ + 1];
        delay_cs = data_[pos_ + 2] | data_[pos_ + 3] << 8;
        transparent = packed & 1 ? data_[pos_ + 4] : -1;
        gce_dispose = packed >> 2 & 7;
      }
      if (label < 0 || !skip_blocks()) return -1;
    } else if (b == 0x2C) {  // 一帧图像
      if (pos_ + 9 > len_) return -1;
      const uint8_t *d = data_ + pos_;
      int fx = d[0] | d[1] << 8, fy = d[2] | d[3] << 8;
      int fw = d[4] | d[5] << 8, fh = d[6] | d[7] << 8;
      uint8_t packed = d[8];
      pos_ += 9;
      const uint16_t *pal = global_;
      int pal_size = global_size_;
      if (packed & 0x80) {
        pal_size = 2 << (packed & 7);
        if (!read_palette(local_, pal_size)) return -1;
        pal = local_;
      }
      // 处理方式只作用在这一帧和画布重叠的部分
      int cx = fx < width_ ? fx : width_, cy = fy < height_ ? fy : height_;
      int cw = fx + fw < width_ ? fw : width_ - cx, ch = fy + fh < height_ ? fh : height_ - cy;
      if (gce_dispose == 3 && prev) {
        for (int y = cy; y < cy + ch; y++)
          memcpy(prev + y * width_ + cx, canvas + y * width_ + cx, cw * sizeof(uint16_t));
      }
      bool ok = decode_image(canvas, fx, fy, fw, fh, packed & 0x40, pal, pal_size, transparent);
      dispose_ = gce_dispose == 3 && !prev ? 1 : gce_dispose;
      dx_ = cx;
      dy_ = cy;
      dw_ = cw;
      dh_ = ch;
      frame_++;
      if (!ok) return -1;
      // 和浏览器一样，0 或 1（10 毫秒）当成 100 毫秒，不然有的动图会快得看不清
      return delay_cs < 2 ? 100 : delay_cs * 10;
    } else {
      return -1;  // 0x3B 结束，或者文件坏了
    }
  }
}

bool GifDecoder::decode_image(uint16_t *canvas, int fx, int fy, int fw, int fh, bool interlaced,
                              const uint16_t *pal, int pal_size, int transparent) {
  int min = byte();
  if (min < 2 || min > 11) return false;
  const int clear = 1 << min, eoi = clear + 1;
  int code_size = min + 1, next = eoi + 1, old = -1, first = 0;
  uint32_t bits = 0;
  int nbits = 0, left = 0;  // left：当前子块还剩几个字节
  bool end = false;         // 读到了长度为 0 的结束子块

  // 写像素：x、row 是这一帧里的位置，隔行扫描时 row 按 8、8、4、2 的步长分四遍
  int x = 0, row = 0, pass = 0;
  static const uint8_t kStart[4] = {0, 4, 2, 1}, kStep[4] = {8, 8, 4, 2};
  int step = interlaced ? kStep[0] : 1;
  auto emit = [&](int idx) {
    if (row >= fh) return;
    int cx = fx + x, cy = fy + row;
    if (idx != transparent && idx < pal_size && cx < width_ && cy < height_) canvas[cy * width_ + cx] = pal[idx];
    if (++x < fw) return;
    x = 0;
    row += step;
    while (interlaced && row >= fh && pass < 3) {
      pass++;
      row = kStart[pass];
      step = kStep[pass];
    }
  };

  for (;;) {
    while (nbits < code_size) {
      if (left == 0) {
        left = byte();
        if (left <= 0) {
          end = true;
          break;
        }
      }
      int c = byte();
      if (c < 0) return false;
      bits |= (uint32_t)c << nbits;
      nbits += 8;
      left--;
    }
    if (end) break;
    int code = bits & ((1 << code_size) - 1);
    bits >>= code_size;
    nbits -= code_size;

    if (code == clear) {
      code_size = min + 1;
      next = eoi + 1;
      old = -1;
      continue;
    }
    if (code == eoi) break;
    if (old < 0) {
      if (code >= clear) return false;
      emit(code);
      old = first = code;
      continue;
    }
    int in = code, sp = 0;
    if (code >= next) {
      if (code > next) return false;  // 坏数据
      stack_[sp++] = first;
      code = old;
    }
    while (code >= clear) {
      if (sp >= 4096) return false;
      stack_[sp++] = suffix_[code];
      code = prefix_[code];
    }
    first = code;
    stack_[sp++] = code;
    while (sp > 0) emit(stack_[--sp]);
    if (next < 4096) {
      prefix_[next] = old;
      suffix_[next] = first;
      next++;
      if (next == 1 << code_size && code_size < 12) code_size++;
    }
    old = in;
  }
  if (end) return true;
  pos_ += left;  // 结束码后面还有剩下的子块
  return pos_ <= len_ && skip_blocks();
}

void media_blit_fit(const uint16_t *src, int sw, int sh, uint16_t *dst, int dw, int dh) {
  int tw, th;
  if (sw <= dw && sh <= dh) {
    int k = dw / sw < dh / sh ? dw / sw : dh / sh;
    tw = sw * k;
    th = sh * k;
  } else if ((long)sw * dh > (long)sh * dw) {  // 比屏幕宽
    tw = dw;
    th = (int)((long)sh * dw / sw);
  } else {
    th = dh;
    tw = (int)((long)sw * dh / sh);
  }
  if (tw < 1) tw = 1;
  if (th < 1) th = 1;
  int ox = (dw - tw) / 2, oy = (dh - th) / 2;
  for (int i = 0; i < dw * dh; i++) dst[i] = 0;
  for (int y = 0; y < th; y++) {
    const uint16_t *s = src + (long)y * sh / th * sw;
    uint16_t *d = dst + (y + oy) * dw + ox;
    for (int x = 0; x < tw; x++) d[x] = s[(long)x * sw / tw];
  }
}
