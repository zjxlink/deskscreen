#include "song_decoder.h"

#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
static void *big_malloc(size_t n) {
  void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return p ? p : malloc(n);
}
static void *big_realloc(void *p, size_t n) {
  void *q = heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return q ? q : realloc(p, n);
}
#else
static void *big_malloc(size_t n) { return malloc(n); }
static void *big_realloc(void *p, size_t n) { return realloc(p, n); }
#endif

// minimp3 解一帧要十几 KB 的临时内存，改成放在这里（PSRAM），见 lib/minimp3/minimp3.h 里的改动
static void *mp3_scratch = nullptr;
#define MINIMP3_SCRATCH mp3_scratch
#define MINIMP3_NO_SIMD
#define MINIMP3_IMPLEMENTATION
#include <minimp3.h>

#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_STDIO
#define DR_FLAC_NO_OGG
#define DR_FLAC_NO_SIMD
#define DR_FLAC_NO_WCHAR
#include <dr_flac.h>

static constexpr size_t kInBuf = 16 * 1024;  // MP3 一次从卡上读这么多
static constexpr int kMaxBadFrames = 64;      // 连着这么多次解不出东西就当放完了

struct SongDecoderFlacIo {
  static size_t read(void *user, void *buf, size_t n) {
    auto *d = (SongDecoder *)user;
    if (d->flac_pos_ >= d->size_) return 0;
    if (n > d->size_ - d->flac_pos_) n = d->size_ - d->flac_pos_;
    size_t got = d->read_(d->ctx_, d->flac_pos_, (uint8_t *)buf, n);
    d->flac_pos_ += got;
    return got;
  }
  static drflac_bool32 seek(void *user, int offset, drflac_seek_origin origin) {
    auto *d = (SongDecoder *)user;
    int64_t base = origin == DRFLAC_SEEK_SET ? 0 : origin == DRFLAC_SEEK_CUR ? d->flac_pos_ : d->size_;
    int64_t p = base + offset;
    if (p < 0 || p > d->size_) return DRFLAC_FALSE;
    d->flac_pos_ = (uint32_t)p;
    return DRFLAC_TRUE;
  }
  static drflac_bool32 tell(void *user, drflac_int64 *pos) {
    *pos = ((SongDecoder *)user)->flac_pos_;
    return DRFLAC_TRUE;
  }
  static void *alloc(size_t n, void *) { return big_malloc(n); }
  static void *realloc_(void *p, size_t n, void *) { return big_realloc(p, n); }
  static void free_(void *p, void *) { free(p); }
};

bool SongDecoder::open(TrackRead read, void *ctx, uint32_t size, const TrackProbe &p) {
  close();
  read_ = read;
  ctx_ = ctx;
  size_ = size;
  probe_ = p;
  rate_ = p.rate;
  samples_ = 0;
  bad_ = 0;
  file_pos_ = p.data_off;
  data_end_ = p.data_off + p.data_len;
  if (data_end_ > size || data_end_ < p.data_off) data_end_ = size;
  switch (p.format) {
    case TrackFormat::Mp3: {
      if (!mp3_scratch) mp3_scratch = big_malloc(sizeof(mp3dec_scratch_t));
      mp3_ = big_malloc(sizeof(mp3dec_t));
      in_ = (uint8_t *)big_malloc(kInBuf);
      pcm_ = (int16_t *)big_malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t));
      if (!mp3_scratch || !mp3_ || !in_ || !pcm_) break;
      mp3dec_init((mp3dec_t *)mp3_);
      in_len_ = pcm_len_ = pcm_at_ = 0;
      format_ = TrackFormat::Mp3;
      return true;
    }
    case TrackFormat::Flac: {
      drflac_allocation_callbacks cb = {nullptr, SongDecoderFlacIo::alloc, SongDecoderFlacIo::realloc_,
                                        SongDecoderFlacIo::free_};
      flac_pos_ = 0;
      flac_ = drflac_open(SongDecoderFlacIo::read, SongDecoderFlacIo::seek, SongDecoderFlacIo::tell, this, &cb);
      pcm_ = (int16_t *)big_malloc(4096 * 2 * sizeof(int16_t));
      if (!flac_ || !pcm_) break;
      rate_ = ((drflac *)flac_)->sampleRate;
      format_ = TrackFormat::Flac;
      return true;
    }
    case TrackFormat::Wav:
      pcm_ = (int16_t *)big_malloc(4096 * 2 * 3);
      if (!pcm_) break;
      format_ = TrackFormat::Wav;
      return true;
    default:
      break;
  }
  close();
  return false;
}

void SongDecoder::close() {
  if (flac_) drflac_close((drflac *)flac_);
  flac_ = nullptr;
  free(mp3_);
  mp3_ = nullptr;
  free(in_);
  in_ = nullptr;
  free(pcm_);
  pcm_ = nullptr;
  format_ = TrackFormat::None;
}

// 把读进来的缓冲补满
bool SongDecoder::fill_mp3() {
  if (file_pos_ >= data_end_) return false;
  size_t want = kInBuf - in_len_;
  if (want > data_end_ - file_pos_) want = data_end_ - file_pos_;
  if (want == 0) return false;
  size_t got = read_(ctx_, file_pos_, in_ + in_len_, want);
  file_pos_ += got;
  in_len_ += got;
  return got > 0;
}

size_t SongDecoder::read_mp3(int16_t *out, size_t max) {
  size_t done = 0;
  while (done < max) {
    if (pcm_at_ < pcm_len_) {
      size_t n = pcm_len_ - pcm_at_ < max - done ? pcm_len_ - pcm_at_ : max - done;
      memcpy(out + done, pcm_ + pcm_at_, n * sizeof(int16_t));
      pcm_at_ += n;
      done += n;
      continue;
    }
    if (in_len_ < kInBuf / 2) fill_mp3();
    if (in_len_ == 0 || bad_ >= kMaxBadFrames) break;
    mp3dec_frame_info_t info;
    int n = mp3dec_decode_frame((mp3dec_t *)mp3_, in_, (int)in_len_, pcm_, &info);
    size_t used = info.frame_bytes;
    if (used == 0) {
      // 缓冲里找不到完整的一帧：读到结尾了就算放完，否则扔掉这段接着找
      if (file_pos_ >= data_end_) break;
      used = in_len_ > 1 ? in_len_ - 1 : in_len_;
      bad_++;
    }
    memmove(in_, in_ + used, in_len_ - used);
    in_len_ -= used;
    if (n <= 0) {
      if (info.frame_bytes) bad_++;
      continue;
    }
    bad_ = 0;
    if (info.hz > 0 && (uint32_t)info.hz != rate_ && samples_ == 0 && done == 0) rate_ = info.hz;
    if (info.channels == 2) {
      for (int i = 0; i < n; i++) pcm_[i] = (int16_t)(((int32_t)pcm_[2 * i] + pcm_[2 * i + 1]) >> 1);
    }
    pcm_len_ = n;
    pcm_at_ = 0;
  }
  return done;
}

size_t SongDecoder::read_wav(int16_t *out, size_t max) {
  const int ch = probe_.channels, bytes = probe_.bits / 8, frame = ch * bytes;
  if (max > 4096) max = 4096;
  size_t avail = (data_end_ - file_pos_) / frame;
  size_t n = max < avail ? max : avail;
  if (n == 0) return 0;
  uint8_t *raw = (uint8_t *)pcm_;
  size_t got = read_(ctx_, file_pos_, raw, n * frame) / frame;
  file_pos_ += got * frame;
  for (size_t i = 0; i < got; i++) {
    int32_t sum = 0;
    for (int c = 0; c < ch; c++) {
      const uint8_t *s = raw + i * frame + c * bytes;
      sum += bytes == 2 ? (int16_t)(s[0] | s[1] << 8) : (int16_t)(s[1] | s[2] << 8);  // 24 位只要高 16 位
    }
    out[i] = (int16_t)(sum / ch);
  }
  return got;
}

size_t SongDecoder::read_flac(int16_t *out, size_t max) {
  auto *f = (drflac *)flac_;
  const int ch = f->channels;
  if (max > 4096) max = 4096;
  size_t got = (size_t)drflac_read_pcm_frames_s16(f, max, ch <= 2 ? pcm_ : nullptr);
  if (ch > 2) return 0;  // 多声道的不放
  for (size_t i = 0; i < got; i++) {
    out[i] = ch == 2 ? (int16_t)(((int32_t)pcm_[2 * i] + pcm_[2 * i + 1]) >> 1) : pcm_[i];
  }
  return got;
}

size_t SongDecoder::read(int16_t *out, size_t max) {
  size_t n = 0;
  switch (format_) {
    case TrackFormat::Mp3: n = read_mp3(out, max); break;
    case TrackFormat::Flac: n = read_flac(out, max); break;
    case TrackFormat::Wav: n = read_wav(out, max); break;
    default: break;
  }
  samples_ += n;
  return n;
}

bool SongDecoder::seek(uint32_t ms) {
  if (!rate_) return false;
  uint64_t target = (uint64_t)ms * rate_ / 1000;
  switch (format_) {
    case TrackFormat::Mp3:
      file_pos_ = track_mp3_seek_offset(probe_, ms);
      in_len_ = pcm_len_ = pcm_at_ = 0;
      bad_ = 0;
      mp3dec_init((mp3dec_t *)mp3_);
      samples_ = target;
      return true;
    case TrackFormat::Flac:
      if (!drflac_seek_to_pcm_frame((drflac *)flac_, target)) return false;
      samples_ = target;
      return true;
    case TrackFormat::Wav: {
      uint32_t frame = probe_.channels * probe_.bits / 8;
      uint64_t off = target * frame;
      if (off > data_end_ - probe_.data_off) off = data_end_ - probe_.data_off;
      file_pos_ = probe_.data_off + (uint32_t)off;
      samples_ = off / frame;
      return true;
    }
    default:
      return false;
  }
}

uint32_t SongDecoder::pos_ms() const {
  return rate_ ? (uint32_t)(samples_ * 1000 / rate_) : 0;
}
