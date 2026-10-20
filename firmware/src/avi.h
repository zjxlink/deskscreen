#pragma once

#include <stddef.h>
#include <stdint.h>

// MJPEG 视频（.avi）的文件解析，不依赖硬件，可在电脑上测试。
// 网页上传视频时浏览器会转成这种格式；电脑上也可以用 ffmpeg 转：
//   ffmpeg -i in.mp4 -vf "scale=320:240:force_original_aspect_ratio=decrease" -r 15 -c:v mjpeg -q:v 5 -an out.avi
// 只找视频帧的位置和大小，每帧是一张完整的 JPG，交给 LovyanGFX 解码。声音、索引等其它块都跳过。
// 文件一边放一边从 TF 卡读，所以通过 read 回调随机读取，不用整个读进内存

class AviReader {
 public:
  // 从 off 开始读 len 个字节到 buf，返回读到的字节数
  using ReadFn = size_t (*)(void *ctx, uint32_t off, uint8_t *buf, size_t len);

  // 读文件头，找到视频数据。不是 AVI 或者没有视频数据时返回 false
  bool open(ReadFn read, void *ctx, uint32_t file_size);

  // 每帧多少微秒（文件头里写的，没写或不合理时按每秒 15 帧）
  uint32_t frame_us() const { return frame_us_; }
  int width() const { return width_; }
  int height() const { return height_; }

  // 下一帧 JPG 数据的位置和长度；放完了或文件坏了时返回 false
  bool next(uint32_t &off, uint32_t &len);

  // 回到第一帧
  void rewind() { pos_ = movi_start_; }

 private:
  bool read_at(uint32_t off, uint8_t *buf, size_t len);
  bool parse_hdrl(uint32_t start, uint32_t end);

  ReadFn read_ = nullptr;
  void *ctx_ = nullptr;
  uint32_t size_ = 0;
  uint32_t movi_start_ = 0;
  uint32_t movi_end_ = 0;
  uint32_t pos_ = 0;
  uint32_t frame_us_ = 66666;
  int width_ = 0;
  int height_ = 0;
};
