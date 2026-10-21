#include "avi.h"

#include <string.h>

static uint32_t le32(const uint8_t *p) {
  return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint16_t le16(const uint8_t *p) {
  return p[0] | p[1] << 8;
}

bool AviReader::read_at(uint32_t off, uint8_t *buf, size_t len) {
  return off <= size_ && len <= size_ - off && read_(ctx_, off, buf, len) == len;
}

// 一个流的头：strh 说是视频还是声音，strf 是格式，indx 是 OpenDML 的总索引
void AviReader::parse_strl(uint32_t start, uint32_t end, int stream) {
  enum { Other, Video, Audio } type = Other;
  uint8_t h[8];
  for (uint32_t off = start; off + 8 <= end;) {
    if (!read_at(off, h, 8)) return;
    uint32_t len = le32(h + 4), body = off + 8;
    if (len > end - body) return;
    if (memcmp(h, "strh", 4) == 0 && len >= 36) {
      uint8_t s[36];
      if (!read_at(body, s, 36)) return;
      if (memcmp(s, "vids", 4) == 0 && video_stream_ < 0) {
        type = Video;
        video_stream_ = stream;
        uint32_t scale = le32(s + 20), rate = le32(s + 24), length = le32(s + 32);
        // 帧率按 rate/scale 算更准（avih 里的微秒数是取整过的）
        if (scale && rate) {
          uint64_t us = (uint64_t)scale * 1000000 / rate;
          if (us >= 10000 && us <= 1000000) frame_us_ = (uint32_t)us;
        }
        if (length > total_frames_) total_frames_ = length;
      } else if (memcmp(s, "auds", 4) == 0 && audio_stream_ < 0) {
        type = Audio;
        audio_stream_ = stream;
      }
    } else if (memcmp(h, "strf", 4) == 0 && type == Audio && len >= 16) {
      uint8_t f[16];
      if (!read_at(body, f, 16)) return;
      uint16_t tag = le16(f), ch = le16(f + 2), bits = le16(f + 14);
      uint32_t rate = le32(f + 4);
      // 1 是 PCM；0xFFFE 是扩展格式，ffmpeg 多声道时用，这里也当 PCM（不是的话放出来是噪音，所以只认 1～2 声道）
      audio_ok_ = (tag == 1 || tag == 0xFFFE) && bits == 16 && ch >= 1 && ch <= 2 && rate >= 4000 && rate <= 96000;
      audio_rate_ = rate;
      audio_channels_ = ch;
    } else if (memcmp(h, "indx", 4) == 0 && type == Video && len >= 24) {
      uint8_t x[24];
      if (!read_at(body, x, 24)) return;
      // 总索引：每条 16 字节（4 个 32 位数），bIndexType 为 0（AVI_INDEX_OF_INDEXES）
      if (le16(x) == 4 && x[3] == 0) {
        uint32_t n = le32(x + 4);
        if (n > (len - 24) / 16) n = (len - 24) / 16;
        indx_off_ = body + 24;
        indx_count_ = n;
      }
    }
    off = body + len + (len & 1);
  }
}

// 文件头列表：avih 里的帧间隔、尺寸和帧数，各个流的头，OpenDML 的总帧数
void AviReader::parse_hdrl(uint32_t start, uint32_t end) {
  uint8_t h[12];
  for (uint32_t off = start; off + 8 <= end;) {
    if (!read_at(off, h, 8)) return;
    uint32_t len = le32(h + 4), body = off + 8;
    if (len > end - body) len = end - body;
    if (memcmp(h, "avih", 4) == 0 && len >= 40) {
      uint8_t avih[40];
      if (!read_at(body, avih, 40)) return;
      uint32_t us = le32(avih);
      if (us >= 10000 && us <= 1000000) frame_us_ = us;  // 每秒 1～100 帧
      if (le32(avih + 16) > total_frames_) total_frames_ = le32(avih + 16);
      width_ = (int)le32(avih + 32);
      height_ = (int)le32(avih + 36);
    } else if (memcmp(h, "LIST", 4) == 0 && len >= 4 && read_at(body, h + 8, 4)) {
      if (memcmp(h + 8, "strl", 4) == 0) {
        parse_strl(body + 4, body + len, streams_++);
      } else if (memcmp(h + 8, "odml", 4) == 0 && len >= 16) {
        uint8_t d[12];
        // dmlh：整个文件（所有段）的帧数，avih 里的只是第一段的
        if (read_at(body + 4, d, 12) && memcmp(d, "dmlh", 4) == 0 && le32(d + 8) > total_frames_)
          total_frames_ = le32(d + 8);
      }
    }
    off = body + len + (len & 1);
  }
}

// OpenDML：第一个 RIFF 后面接着 RIFF AVIX 段，每段里有自己的 movi 列表
void AviReader::find_extra_segments(uint32_t off) {
  uint8_t h[12];
  while (seg_count_ < kMaxSegments && off + 12 <= size_ && read_at(off, h, 12) && memcmp(h, "RIFF", 4) == 0 &&
         memcmp(h + 8, "AVIX", 4) == 0) {
    uint32_t len = le32(h + 4);
    uint32_t end = len > size_ - off - 8 ? size_ : off + 8 + len;
    for (uint32_t p = off + 12; p + 12 <= end;) {
      if (!read_at(p, h, 12)) return;
      uint32_t n = le32(h + 4);
      if (memcmp(h, "LIST", 4) == 0 && memcmp(h + 8, "movi", 4) == 0) {
        segs_[seg_count_++] = {p + 12, n > end - p - 8 ? end : p + 8 + n};
        break;
      }
      if (n > end - p - 8) break;
      p += 8 + n + (n & 1);
    }
    if (end >= size_) return;
    off = end + (len & 1);
  }
}

bool AviReader::open(ReadFn read, void *ctx, uint32_t file_size) {
  read_ = read;
  ctx_ = ctx;
  size_ = file_size;
  seg_count_ = seg_ = 0;
  pos_ = frame_pos_ = 0;
  frame_us_ = 66666;
  total_frames_ = 0;
  width_ = height_ = 0;
  streams_ = 0;
  video_stream_ = audio_stream_ = -1;
  audio_ok_ = false;
  audio_rate_ = 0;
  audio_channels_ = 0;
  indx_off_ = indx_count_ = idx1_off_ = idx1_count_ = 0;
  uint8_t h[12];
  if (!read || !read_at(0, h, 12) || memcmp(h, "RIFF", 4) != 0 || memcmp(h + 8, "AVI ", 4) != 0) return false;
  uint32_t riff_len = le32(h + 4);
  uint32_t end = riff_len > size_ - 8 ? size_ : riff_len + 8;  // 没写完的文件也尽量放
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
      } else if (memcmp(type, "movi", 4) == 0 && seg_count_ == 0) {
        segs_[seg_count_++] = {body + 4, list_end};
      }
    } else if (memcmp(h, "idx1", 4) == 0 && seg_count_ > 0) {
      idx1_off_ = body;
      idx1_count_ = (len > end - body ? end - body : len) / 16;
    }
    if (len > end - body) break;
    off = body + len + (len & 1);
  }
  if (seg_count_ == 0) return false;
  if (riff_len <= size_ - 8) find_extra_segments(riff_len + 8 + (riff_len & 1));
  rewind();
  return true;
}

void AviReader::rewind() {
  seg_ = 0;
  pos_ = seg_count_ ? segs_[0].start : 0;
  frame_pos_ = 0;
}

// 块名的前两个字是流的编号（00、01……）
int AviReader::stream_of(const uint8_t *id) const {
  if (id[0] < '0' || id[0] > '9' || id[1] < '0' || id[1] > '9') return -1;
  return (id[0] - '0') * 10 + (id[1] - '0');
}

AviChunk AviReader::next_chunk(uint32_t &off, uint32_t &len) {
  uint8_t h[8];
  while (seg_ < seg_count_) {
    uint32_t end = segs_[seg_].end;
    if (pos_ + 8 > end) {
      if (++seg_ < seg_count_) pos_ = segs_[seg_].start;
      continue;
    }
    if (!read_at(pos_, h, 8)) return AviChunk::End;
    uint32_t n = le32(h + 4);
    if (memcmp(h, "LIST", 4) == 0) {  // rec 列表：里面是一帧的视频和声音，进去接着找
      pos_ += 12;
      continue;
    }
    uint32_t body = pos_ + 8;
    if (n > end - body) return AviChunk::End;  // 最后一块没写完
    pos_ = body + n + (n & 1);
    if (n == 0) continue;
    int stream = stream_of(h);
    // 视频块的名字是 00dc（压缩的）或 00db，声音是 01wb，索引是 ix00
    if (h[2] == 'd' && (h[3] == 'c' || h[3] == 'b') && (video_stream_ < 0 || stream == video_stream_)) {
      off = body;
      len = n;
      frame_pos_++;
      return AviChunk::Video;
    }
    if (h[2] == 'w' && h[3] == 'b' && has_audio() && stream == audio_stream_) {
      off = body;
      len = n;
      return AviChunk::Audio;
    }
  }
  return AviChunk::End;
}

bool AviReader::next(uint32_t &off, uint32_t &len) {
  for (;;) {
    AviChunk c = next_chunk(off, len);
    if (c == AviChunk::End) return false;
    if (c == AviChunk::Video) return true;
  }
}

// 把读的位置放到 chunk_off（一块的开头），找到它在哪一段
void AviReader::set_pos(uint32_t chunk_off) {
  for (int i = 0; i < seg_count_; i++) {
    if (chunk_off >= segs_[i].start && chunk_off < segs_[i].end) {
      seg_ = i;
      pos_ = chunk_off;
      return;
    }
  }
}

// OpenDML 索引：总索引的每一条指向一个 ix00 小索引，并写着它管多少帧
bool AviReader::seek_indx(uint32_t frame, uint32_t &chunk_off, uint32_t &found) {
  uint32_t first = 0;
  uint8_t e[16];
  for (uint32_t i = 0; i < indx_count_; i++) {
    if (!read_at(indx_off_ + i * 16, e, 16)) return false;
    uint32_t ix = le32(e), ix_hi = le32(e + 4), dur = le32(e + 12);
    if (ix_hi != 0) return false;  // 超过 4GB，FAT32 上不会有
    if (frame >= first + dur && i + 1 < indx_count_) {
      first += dur;
      continue;
    }
    uint8_t h[32];
    // ix00 的头：每条 2 个 32 位数，bIndexType 为 1（AVI_INDEX_OF_CHUNKS），条目从第 32 字节开始
    if (!read_at(ix, h, 32) || memcmp(h, "ix", 2) != 0 || le16(h + 8) != 2 || h[11] != 1) return false;
    uint32_t n = le32(h + 12), base = le32(h + 20);
    if (le32(h + 24) != 0 || n == 0) return false;
    uint32_t k = frame - first < n ? frame - first : n - 1;
    uint8_t ent[8];
    if (!read_at(ix + 32 + k * 8, ent, 8)) return false;
    uint32_t data = base + le32(ent);
    if (data < 8) return false;
    chunk_off = data - 8;
    found = first + k;
    return true;
  }
  return false;
}

// 老的 idx1 索引：所有流的块按顺序排着，一条一条数到第 frame 个视频块
bool AviReader::seek_idx1(uint32_t frame, uint32_t &chunk_off, uint32_t &found) {
  if (seg_count_ == 0) return false;
  uint8_t buf[16 * 32];
  uint32_t count = 0, last = 0;
  bool have = false;
  int32_t base = -1;  // idx1 里的位置有的从 movi 那四个字算起，有的从文件开头算起
  for (uint32_t i = 0; i < idx1_count_; i += 32) {
    uint32_t n = idx1_count_ - i < 32 ? idx1_count_ - i : 32;
    if (!read_at(idx1_off_ + i * 16, buf, n * 16)) return false;
    for (uint32_t j = 0; j < n; j++) {
      const uint8_t *e = buf + j * 16;
      if (!(e[2] == 'd' && (e[3] == 'c' || e[3] == 'b')) || le32(e + 12) == 0) continue;
      int s = stream_of(e);
      if (video_stream_ >= 0 && s != video_stream_) continue;
      uint32_t off = le32(e + 8);
      if (base < 0) {
        uint8_t id[4];
        uint32_t rel = segs_[0].start - 4 + off;
        base = read_at(rel, id, 4) && memcmp(id, e, 4) == 0 ? (int32_t)(segs_[0].start - 4) : 0;
      }
      last = (uint32_t)base + off;
      have = true;
      if (count == frame) {
        chunk_off = last;
        found = count;
        return true;
      }
      count++;
    }
  }
  if (!have) return false;
  chunk_off = last;  // 超过结尾：最后一帧
  found = count - 1;
  return true;
}

bool AviReader::seek_frame(uint32_t frame) {
  uint32_t off = 0, found = 0;
  uint8_t h[8];
  if (frame == 0) {
    rewind();
    return seg_count_ > 0;
  }
  bool ok = (indx_count_ && seek_indx(frame, off, found)) || (idx1_count_ && seek_idx1(frame, off, found));
  // 索引指的位置要真是一个视频块，不然就不信它
  if (ok && !(read_at(off, h, 8) && h[2] == 'd' && (h[3] == 'c' || h[3] == 'b'))) ok = false;
  if (ok) {
    set_pos(off);
    frame_pos_ = found;
    return true;
  }
  // 没有索引：从头一块一块数
  int seg = seg_;
  uint32_t pos = pos_, fp = frame_pos_;
  rewind();
  uint32_t o, n, prev_seg = 0, prev_pos = 0, prev_fp = 0;
  bool any = false;
  for (;;) {
    int s = seg_;
    uint32_t p = pos_, f = frame_pos_;
    AviChunk c = next_chunk(o, n);
    if (c == AviChunk::End) break;
    if (c != AviChunk::Video) continue;
    any = true;
    prev_seg = s;
    prev_pos = p;
    prev_fp = f;
    if (f == frame) break;
  }
  if (!any) {
    seg_ = seg;
    pos_ = pos;
    frame_pos_ = fp;
    return false;
  }
  seg_ = (int)prev_seg;
  pos_ = prev_pos;
  frame_pos_ = prev_fp;
  return true;
}

// ---- 声音 ----

void PcmTo16k::reset(uint32_t rate, int channels) {
  step_ = rate ? (uint32_t)(((uint64_t)rate << 16) / 16000) : 65536;
  if (step_ == 0) step_ = 1;
  channels_ = channels == 2 ? 2 : 1;
  phase_ = 0;
  has_prev_ = false;
  prev_ = 0;
  carry_len_ = 0;
}

size_t PcmTo16k::max_out(size_t len) const {
  size_t in = (len + carry_len_) / (2 * channels_) + 1;
  return (size_t)(((uint64_t)in << 16) / step_) + 2;
}

size_t PcmTo16k::convert(const uint8_t *in, size_t len, int16_t *out, size_t out_max) {
  size_t frame = 2 * channels_, n = 0;
  auto feed = [&](const uint8_t *p) {
    int v = (int16_t)(p[0] | p[1] << 8);
    if (channels_ == 2) v = (v + (int16_t)(p[2] | p[3] << 8)) / 2;
    if (!has_prev_) {
      prev_ = (int16_t)v;
      has_prev_ = true;
      return;
    }
    // 线性插值：输出采样落在 prev_ 和这个采样之间
    while (phase_ < 65536) {
      int32_t y = prev_ + (int32_t)(((int64_t)(v - prev_) * phase_) >> 16);
      if (n < out_max) out[n++] = (int16_t)y;
      phase_ += step_;
    }
    phase_ -= 65536;
    prev_ = (int16_t)v;
  };
  // 上一块剩下的半个采样先拼起来
  while (carry_len_ > 0 && len > 0) {
    carry_[carry_len_++] = *in++;
    len--;
    if (carry_len_ == frame) {
      feed(carry_);
      carry_len_ = 0;
    }
  }
  for (; len >= frame; in += frame, len -= frame) feed(in);
  for (; len > 0; len--) carry_[carry_len_++] = *in++;
  return n;
}
