#include "album.h"

#include <string.h>

static bool ends_with_ci(const char *s, const char *suffix) {
  size_t n = strlen(s), m = strlen(suffix);
  if (n < m) return false;
  for (size_t i = 0; i < m; i++) {
    char c = s[n - m + i];
    if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    if (c != suffix[i]) return false;
  }
  return true;
}

bool album_is_photo(const char *name) {
  if (!name || name[0] == '.' || name[0] == '\0') return false;
  return ends_with_ci(name, ".jpg") || ends_with_ci(name, ".jpeg") || ends_with_ci(name, ".png");
}

size_t album_safe_name(const char *in, const char *fallback, char *out, size_t out_size) {
  if (out_size < 8) {
    if (out_size) out[0] = '\0';
    return 0;
  }
  // 扩展名不要，统一存成 .jpg（网页上传前已经转成 JPEG）
  const char *dot = in ? strrchr(in, '.') : nullptr;
  size_t end = in ? (dot ? (size_t)(dot - in) : strlen(in)) : 0;
  size_t max = out_size - 5;  // 留出 ".jpg" 和结尾的 0
  if (max > 40) max = 40;
  size_t len = 0;
  for (size_t i = 0; i < end && len < max; i++) {
    char c = in[i];
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_')
      out[len++] = c;
  }
  if (len == 0 && fallback) {
    for (const char *p = fallback; *p && len < max; p++) out[len++] = *p;
  }
  if (len == 0) out[len++] = 'p';
  memcpy(out + len, ".jpg", 5);
  return len + 4;
}

uint32_t PhotoOrder::rand() {
  // xorshift32
  seed_ ^= seed_ << 13;
  seed_ ^= seed_ >> 17;
  seed_ ^= seed_ << 5;
  return seed_;
}

void PhotoOrder::reset(int count, bool shuffle, uint32_t seed) {
  count_ = count < 0 ? 0 : count > kAlbumMaxPhotos ? kAlbumMaxPhotos : count;
  shuffle_ = shuffle;
  seed_ = seed ? seed : 1;
  pos_ = count_;  // 下一次 next() 开始新的一轮
  if (last_ >= count_) last_ = -1;
}

void PhotoOrder::shuffle_round() {
  for (int i = 0; i < count_; i++) order_[i] = i;
  if (!shuffle_) return;
  for (int i = count_ - 1; i > 0; i--) {
    int j = rand() % (i + 1);
    uint16_t t = order_[i];
    order_[i] = order_[j];
    order_[j] = t;
  }
  // 新一轮的第一张别和上一轮最后一张一样
  if (count_ > 1 && order_[0] == last_) {
    int j = 1 + rand() % (count_ - 1);
    order_[0] = order_[j];
    order_[j] = last_;
  }
}

int PhotoOrder::next(bool *new_round) {
  if (new_round) *new_round = false;
  if (count_ == 0) return -1;
  if (pos_ >= count_) {
    shuffle_round();
    pos_ = 0;
    if (new_round) *new_round = true;
  }
  last_ = order_[pos_++];
  return last_;
}
