#include "avi.h"

#include <string.h>

static uint32_t le32(const uint8_t *p) {
  return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

bool AviReader::read_at(uint32_t off, uint8_t *buf, size_t len) {
  return off <= size_ && len <= size_ - off && read_(ctx_, off, buf, len) == len;
}

// 文件头列表：只要 avih 里的帧间隔和尺寸
bool AviReader::parse_hdrl(uint32_t start, uint32_t end) {
  uint8_t h[8];
  if (!read_at(start, h, 8) || memcmp(h, "avih", 4) != 0 || le32(h + 4) < 40) return false;
  uint8_t avih[40];
  if (start + 8 + 40 > end || !read_at(start + 8, avih, 40)) return false;
  uint32_t us = le32(avih);
  if (us >= 10000 && us <= 1000000) frame_us_ = us;  // 每秒 1～100 帧
  width_ = (int)le32(avih + 32);
  height_ = (int)le32(avih + 36);
  return true;
}

bool AviReader::open(ReadFn read, void *ctx, uint32_t file_size) {
  read_ = read;
  ctx_ = ctx;
  size_ = file_size;
  movi_start_ = movi_end_ = pos_ = 0;
  frame_us_ = 66666;
  width_ = height_ = 0;
  uint8_t h[12];
  if (!read || !read_at(0, h, 12) || memcmp(h, "RIFF", 4) != 0 || memcmp(h + 8, "AVI ", 4) != 0) return false;
  uint32_t end = le32(h + 4);
  end = end > size_ - 8 ? size_ : end + 8;  // 没写完的文件也尽量放
  for (uint32_t off = 12; off + 8 <= end;) {
    if (!read_at(off, h, 8)) return false;
    uint32_t len = le32(h + 4);
    uint32_t body = off + 8;
    if (memcmp(h, "LIST", 4) == 0 && len >= 4 && body + 4 <= end) {
      uint8_t type[4];
      if (!read_at(body, type, 4)) return false;
      uint32_t list_end = len - 4 > end - body - 4 ? end : body + len;
      if (memcmp(type, "hdrl", 4) == 0) {
        parse_hdrl(body + 4, list_end);
      } else if (memcmp(type, "movi", 4) == 0) {
        movi_start_ = pos_ = body + 4;
        movi_end_ = list_end;
        return true;
      }
    }
    if (len > end - body) break;
    off = body + len + (len & 1);
  }
  return false;
}

bool AviReader::next(uint32_t &off, uint32_t &len) {
  uint8_t h[8];
  while (pos_ + 8 <= movi_end_) {
    if (!read_at(pos_, h, 8)) return false;
    uint32_t n = le32(h + 4);
    if (memcmp(h, "LIST", 4) == 0) {  // rec 列表：里面是一帧的视频和声音，进去接着找
      pos_ += 12;
      continue;
    }
    uint32_t body = pos_ + 8;
    if (n > movi_end_ - body) return false;  // 最后一块没写完
    pos_ = body + n + (n & 1);
    // 视频块的名字是 00dc（压缩的）或 00db，声音是 01wb，索引是 ix00
    if (h[2] == 'd' && (h[3] == 'c' || h[3] == 'b') && n > 0) {
      off = body;
      len = n;
      return true;
    }
  }
  return false;
}
