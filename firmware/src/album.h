#pragma once

#include <stddef.h>
#include <stdint.h>

// 电子相册里不依赖硬件的部分：认哪些文件是照片、网页上传的文件名、轮播顺序。可在电脑上测试

static constexpr int kAlbumMaxPhotos = 1000;  // photos 文件夹里最多认这么多张

// 文件名是不是能显示的照片：.jpg、.jpeg、.png（不分大小写）；跳过隐藏文件和 macOS 生成的 ._ 文件
bool album_is_photo(const char *name);

// 网页上传的文件名：只保留字母、数字、- 和 _（汉字等去掉），最长 40 个字符，加上 .jpg。
// 去掉后什么都不剩时用 fallback（如时间戳）。返回写入 out 的长度
size_t album_safe_name(const char *in, const char *fallback, char *out, size_t out_size);

// 轮播顺序：每一轮把所有照片放一遍。随机时每轮重新打乱，并且不会和上一轮最后一张连着重复
class PhotoOrder {
 public:
  // 照片数量或随机设置变了时调用，从新的一轮开始
  void reset(int count, bool shuffle, uint32_t seed);

  // 下一张的序号，没有照片时返回 -1。new_round 为 true 表示这一张开始了新的一轮（适合重新扫描文件夹）
  int next(bool *new_round = nullptr);

  int count() const { return count_; }

 private:
  void shuffle_round();
  uint32_t rand();

  int count_ = 0;
  bool shuffle_ = false;
  uint32_t seed_ = 1;
  int pos_ = 0;
  int last_ = -1;
  uint16_t order_[kAlbumMaxPhotos];
};
