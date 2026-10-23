// GIF 动图和 MJPEG 视频的单元测试：在电脑上运行：pio test -e native
#include <string.h>
#include <unity.h>

#include <algorithm>
#include <string>
#include <vector>

#include "avi.h"
#include "gif.h"
#include "gif_fixtures.h"

void setUp() {}
void tearDown() {}

static GifDecoder gif;

// 一帧一帧解码，和 Pillow 合成的结果逐像素比较；放完一遍后返回 -1，rewind 后第一帧一样
template <size_t N, size_t P>
static void check_gif(const uint8_t *data, size_t len, const uint16_t (&frames)[N][P]) {
  TEST_ASSERT_TRUE(gif.open(data, len));
  TEST_ASSERT_EQUAL(P, gif.width() * gif.height());
  std::vector<uint16_t> canvas(P), prev(P);
  for (size_t f = 0; f < N; f++) {
    TEST_ASSERT_TRUE(gif.next_frame(canvas.data(), prev.data()) > 0);
    TEST_ASSERT_EQUAL_HEX16_ARRAY(frames[f], canvas.data(), P);
    TEST_ASSERT_EQUAL(f + 1 < N, gif.has_more());
  }
  TEST_ASSERT_EQUAL(-1, gif.next_frame(canvas.data(), prev.data()));
  gif.rewind();
  TEST_ASSERT_EQUAL(0, gif.frame_index());
  TEST_ASSERT_TRUE(gif.next_frame(canvas.data(), prev.data()) > 0);
  TEST_ASSERT_EQUAL_HEX16_ARRAY(frames[0], canvas.data(), P);
}

static void test_gif_plain() { check_gif(kGifPlain, sizeof(kGifPlain), kGifPlainFrames); }
static void test_gif_dispose3() { check_gif(kGifDispose3, sizeof(kGifDispose3), kGifDispose3Frames); }
static void test_gif_interlaced() { check_gif(kGifInterlaced, sizeof(kGifInterlaced), kGifInterlacedFrames); }
static void test_gif_dispose2() { check_gif(kGifDispose2, sizeof(kGifDispose2), kGifDispose2Frames); }
static void test_gif_ffmpeg() { check_gif(kGifFfmpeg, sizeof(kGifFfmpeg), kGifFfmpegFrames); }

static void test_gif_delay() {
  uint16_t canvas[24], prev[24];
  TEST_ASSERT_TRUE(gif.open(kGifPlain, sizeof(kGifPlain)));
  TEST_ASSERT_EQUAL(70, gif.next_frame(canvas, prev));
  TEST_ASSERT_EQUAL(30, gif.next_frame(canvas, prev));
  TEST_ASSERT_EQUAL(2, gif.frame_index());
  // ffmpeg 生成的每帧 100 毫秒
  std::vector<uint16_t> c(24 * 16), p(24 * 16);
  TEST_ASSERT_TRUE(gif.open(kGifFfmpeg, sizeof(kGifFfmpeg)));
  TEST_ASSERT_EQUAL(100, gif.next_frame(c.data(), p.data()));
  // 不给 prev 也能放（处理方式 3 当成保留），不会崩
  TEST_ASSERT_TRUE(gif.open(kGifDispose3, sizeof(kGifDispose3)));
  std::vector<uint16_t> c3(90);
  for (int i = 0; i < 3; i++) TEST_ASSERT_TRUE(gif.next_frame(c3.data(), nullptr) > 0);
}

static void test_gif_reject() {
  const uint8_t png[] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10, 0, 0, 0, 13, 'I', 'H', 'D', 'R'};
  TEST_ASSERT_FALSE(gif.open(png, sizeof(png)));
  TEST_ASSERT_FALSE(gif.open(nullptr, 0));
  std::vector<uint8_t> big(kGifPlain, kGifPlain + sizeof(kGifPlain));
  big[6] = 0xE8, big[7] = 0x03, big[8] = 0xE8, big[9] = 0x03;  // 1000x1000
  TEST_ASSERT_FALSE(gif.open(big.data(), big.size()));
  big[6] = 0, big[7] = 0;  // 宽度 0
  TEST_ASSERT_FALSE(gif.open(big.data(), big.size()));
}

// 文件被截断在任何位置都不能读越界，只是少几帧
static void test_gif_truncated() {
  std::vector<uint16_t> c(24 * 16), p(24 * 16);
  for (size_t n = 13; n < sizeof(kGifFfmpeg); n++) {
    std::vector<uint8_t> part(kGifFfmpeg, kGifFfmpeg + n);  // 单独分配，越界能被检查工具发现
    if (!gif.open(part.data(), part.size())) continue;
    int frames = 0;
    while (gif.next_frame(c.data(), p.data()) > 0 && frames < 10) frames++;
    TEST_ASSERT_TRUE(frames <= 4);
  }
}

static void test_blit_fit() {
  // 小图放大整数倍：2x2 放进 8x6，放大 3 倍成 6x6，左右各留 1 列
  const uint16_t small[4] = {1, 2, 3, 4};
  uint16_t dst[8 * 6];
  media_blit_fit(small, 2, 2, dst, 8, 6);
  const uint16_t want[8 * 6] = {
      0, 1, 1, 1, 2, 2, 2, 0,  //
      0, 1, 1, 1, 2, 2, 2, 0,  //
      0, 1, 1, 1, 2, 2, 2, 0,  //
      0, 3, 3, 3, 4, 4, 4, 0,  //
      0, 3, 3, 3, 4, 4, 4, 0,  //
      0, 3, 3, 3, 4, 4, 4, 0,  //
  };
  TEST_ASSERT_EQUAL_HEX16_ARRAY(want, dst, 8 * 6);

  // 大图缩小：8x2 放进 4x4，缩成 4x1，上下居中
  const uint16_t wide[16] = {1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8};
  uint16_t d2[16];
  media_blit_fit(wide, 8, 2, d2, 4, 4);
  const uint16_t want2[16] = {0, 0, 0, 0, 1, 2, 3, 4, 0, 0, 0, 0, 0, 0, 0, 0};
  TEST_ASSERT_EQUAL_HEX16_ARRAY(want2, d2, 16);

  // 一样大时原样复制
  uint16_t d3[4];
  media_blit_fit(small, 2, 2, d3, 2, 2);
  TEST_ASSERT_EQUAL_HEX16_ARRAY(small, d3, 4);
}

// ---- AVI ----

static void put32(std::string &s, uint32_t v) {
  for (int i = 0; i < 4; i++) s += (char)(v >> (8 * i) & 0xFF);
}

static std::string chunk(const char *id, const std::string &body) {
  std::string s(id, 4);
  put32(s, body.size());
  s += body;
  if (body.size() & 1) s += '\0';
  return s;
}

static std::string list(const char *type, const std::string &body) {
  return chunk("LIST", std::string(type, 4) + body);
}

static std::string avih(uint32_t us, uint32_t w, uint32_t h) {
  std::string s;
  put32(s, us);
  for (int i = 0; i < 7; i++) put32(s, 0);
  put32(s, w);
  put32(s, h);
  for (int i = 0; i < 4; i++) put32(s, 0);
  return chunk("avih", s);
}

static std::string riff(const std::string &body) {
  std::string s = "RIFF";
  put32(s, body.size() + 4);
  return s + "AVI " + body;
}

static std::string file;
static size_t read_file(void *, uint32_t off, uint8_t *buf, size_t len) {
  if (off >= file.size()) return 0;
  size_t n = std::min(len, file.size() - off);
  memcpy(buf, file.data() + off, n);
  return n;
}

static std::string frame_at(uint32_t off, uint32_t len) { return file.substr(off, len); }

static void test_avi_frames() {
  std::string hdrl = list("hdrl", avih(50000, 320, 240) + list("strl", chunk("strh", std::string(56, '\0'))));
  std::string movi = list("movi", chunk("00dc", "JPEG-A") + chunk("01wb", "sound") +
                                      list("rec ", chunk("00dc", "JPG-B") + chunk("01wb", "x")) +
                                      chunk("JUNK", "....") + chunk("00db", "C") + chunk("00dc", ""));
  file = riff(hdrl + chunk("JUNK", std::string(10, ' ')) + movi + chunk("idx1", std::string(32, '\0')));
  AviReader avi;
  TEST_ASSERT_TRUE(avi.open(read_file, nullptr, file.size()));
  TEST_ASSERT_EQUAL(50000, avi.frame_us());
  TEST_ASSERT_EQUAL(320, avi.width());
  TEST_ASSERT_EQUAL(240, avi.height());
  uint32_t off, len;
  const char *want[] = {"JPEG-A", "JPG-B", "C"};  // 声音、JUNK 和空的帧都跳过，奇数长度后面有补齐的字节
  for (int round = 0; round < 2; round++) {
    for (const char *w : want) {
      TEST_ASSERT_TRUE(avi.next(off, len));
      TEST_ASSERT_EQUAL_STRING(w, frame_at(off, len).c_str());
    }
    TEST_ASSERT_FALSE(avi.next(off, len));  // 不会把 idx1 当成帧
    avi.rewind();
  }
}

static void test_avi_defaults_and_truncated() {
  // 没有 avih 时按每秒 15 帧
  std::string movi = list("movi", chunk("00dc", "AAAA") + chunk("00dc", "BBBBBBBB"));
  file = riff(list("hdrl", "") + movi);
  AviReader avi;
  TEST_ASSERT_TRUE(avi.open(read_file, nullptr, file.size()));
  TEST_ASSERT_EQUAL(66666, avi.frame_us());
  // 不合理的帧间隔也不用
  file = riff(list("hdrl", avih(0, 320, 240)) + movi);
  TEST_ASSERT_TRUE(avi.open(read_file, nullptr, file.size()));
  TEST_ASSERT_EQUAL(66666, avi.frame_us());

  // 没传完的文件：放到最后一个完整的帧
  std::string full = file;
  file = full.substr(0, full.size() - 3);
  TEST_ASSERT_TRUE(avi.open(read_file, nullptr, file.size()));
  uint32_t off, len;
  TEST_ASSERT_TRUE(avi.next(off, len));
  TEST_ASSERT_EQUAL_STRING("AAAA", frame_at(off, len).c_str());
  TEST_ASSERT_FALSE(avi.next(off, len));

  // 截断在任何位置都不能出错
  for (size_t n = 0; n < full.size(); n++) {
    file = full.substr(0, n);
    if (!avi.open(read_file, nullptr, file.size())) continue;
    int frames = 0;
    while (avi.next(off, len)) {
      TEST_ASSERT_TRUE(off + len <= file.size());
      frames++;
    }
    TEST_ASSERT_TRUE(frames <= 2);
  }
}

static void test_avi_reject() {
  AviReader avi;
  file = "RIFF\x04\0\0\0WAVE";
  TEST_ASSERT_FALSE(avi.open(read_file, nullptr, file.size()));
  file = riff(list("hdrl", avih(50000, 320, 240)));  // 没有视频数据
  TEST_ASSERT_FALSE(avi.open(read_file, nullptr, file.size()));
  file = "";
  TEST_ASSERT_FALSE(avi.open(read_file, nullptr, 0));
}


static std::string strh(const char *type, uint32_t scale, uint32_t rate, uint32_t length) {
  std::string s(type, 4);
  s += "MJPG";
  for (int i = 0; i < 3; i++) put32(s, 0);  // dwFlags、wPriority + wLanguage、dwInitialFrames
  put32(s, scale);
  put32(s, rate);
  put32(s, 0);
  put32(s, length);
  for (int i = 0; i < 3; i++) put32(s, 0);
  return chunk("strh", s + std::string(8, '\0'));
}

static void put16(std::string &s, uint16_t v) {
  s += (char)(v & 0xFF);
  s += (char)(v >> 8);
}

static std::string wavefmt(uint16_t tag, uint16_t ch, uint32_t rate, uint16_t bits) {
  std::string s;
  put16(s, tag);
  put16(s, ch);
  put32(s, rate);
  put32(s, rate * ch * bits / 8);
  put16(s, ch * bits / 8);
  put16(s, bits);
  put16(s, 0);
  return chunk("strf", s);
}

// 视频 + 声音两个流，每帧后面跟一块声音，最后是 idx1（位置从 movi 那四个字算起），和 ffmpeg、网页转的一样
static std::string av_file(uint16_t audio_tag, bool with_idx1, int frames) {
  std::string hdrl = list("hdrl", avih(66666, 320, 240) + list("strl", strh("vids", 1, 20, frames)) +
                                      list("strl", strh("auds", 1, 16000, 0) + wavefmt(audio_tag, 1, 16000, 16)));
  std::string body, idx;
  for (int i = 0; i < frames; i++) {
    uint32_t at = 4 + body.size();
    std::string jpg = "F" + std::to_string(i);
    body += chunk("00dc", jpg);
    idx += "00dc";
    put32(idx, 0x10);
    put32(idx, at);
    put32(idx, jpg.size());
    at = 4 + body.size();
    body += chunk("01wb", std::string(4, (char)i));
    idx += "01wb";
    put32(idx, 0);
    put32(idx, at);
    put32(idx, 4);
  }
  return riff(hdrl + list("movi", body) + (with_idx1 ? chunk("idx1", idx) : ""));
}

static void test_avi_audio_and_seek() {
  for (int with_idx1 = 0; with_idx1 < 2; with_idx1++) {
    file = av_file(1, with_idx1, 10);
    AviReader avi;
    TEST_ASSERT_TRUE(avi.open(read_file, nullptr, file.size()));
    TEST_ASSERT_EQUAL(50000, avi.frame_us());  // strh 里的每秒 20 帧比 avih 准
    TEST_ASSERT_EQUAL(10, avi.frames());
    TEST_ASSERT_EQUAL(500, avi.duration_ms());
    TEST_ASSERT_TRUE(avi.has_audio());
    TEST_ASSERT_EQUAL(16000, avi.audio_rate());
    TEST_ASSERT_EQUAL(1, avi.audio_channels());
    uint32_t off, len;
    for (int i = 0; i < 10; i++) {
      TEST_ASSERT_EQUAL(i, avi.frame_pos());
      TEST_ASSERT_TRUE(avi.next_chunk(off, len) == AviChunk::Video);
      TEST_ASSERT_EQUAL_STRING(("F" + std::to_string(i)).c_str(), frame_at(off, len).c_str());
      TEST_ASSERT_TRUE(avi.next_chunk(off, len) == AviChunk::Audio);
      TEST_ASSERT_EQUAL(4, len);
      TEST_ASSERT_EQUAL(i, file[off]);
    }
    TEST_ASSERT_TRUE(avi.next_chunk(off, len) == AviChunk::End);

    // 跳到第 7 帧：接着给第 7 帧和它后面的声音；有没有 idx1 结果都一样
    TEST_ASSERT_TRUE(avi.seek_frame(7));
    TEST_ASSERT_EQUAL(7, avi.frame_pos());
    TEST_ASSERT_TRUE(avi.next_chunk(off, len) == AviChunk::Video);
    TEST_ASSERT_EQUAL_STRING("F7", frame_at(off, len).c_str());
    TEST_ASSERT_TRUE(avi.next_chunk(off, len) == AviChunk::Audio);
    TEST_ASSERT_EQUAL(7, file[off]);
    // 往回跳、跳过结尾（停在最后一帧）、跳回开头
    TEST_ASSERT_TRUE(avi.seek_frame(2));
    TEST_ASSERT_TRUE(avi.next(off, len));
    TEST_ASSERT_EQUAL_STRING("F2", frame_at(off, len).c_str());
    TEST_ASSERT_TRUE(avi.seek_frame(500));
    TEST_ASSERT_EQUAL(9, avi.frame_pos());
    TEST_ASSERT_TRUE(avi.next(off, len));
    TEST_ASSERT_EQUAL_STRING("F9", frame_at(off, len).c_str());
    TEST_ASSERT_TRUE(avi.seek_frame(0));
    TEST_ASSERT_TRUE(avi.next(off, len));
    TEST_ASSERT_EQUAL_STRING("F0", frame_at(off, len).c_str());
  }
}

static void test_avi_unsupported_audio() {
  // MP3 声音（0x55）放不了：当成没有声音，只给视频
  file = av_file(0x55, true, 3);
  AviReader avi;
  TEST_ASSERT_TRUE(avi.open(read_file, nullptr, file.size()));
  TEST_ASSERT_FALSE(avi.has_audio());
  uint32_t off, len;
  for (int i = 0; i < 3; i++) TEST_ASSERT_TRUE(avi.next_chunk(off, len) == AviChunk::Video);
  TEST_ASSERT_TRUE(avi.next_chunk(off, len) == AviChunk::End);
}

// 读卡出错时给 ReadError（不是 End），位置不变，卡好了接着读同一块
static bool card_broken = false;
static size_t read_flaky(void *ctx, uint32_t off, uint8_t *buf, size_t len) {
  return card_broken ? 0 : read_file(ctx, off, buf, len);
}

static void test_avi_read_error() {
  file = av_file(1, true, 5);
  AviReader avi;
  card_broken = false;
  TEST_ASSERT_TRUE(avi.open(read_flaky, nullptr, file.size()));
  uint32_t off, len;
  TEST_ASSERT_TRUE(avi.next_chunk(off, len) == AviChunk::Video);
  TEST_ASSERT_TRUE(avi.next_chunk(off, len) == AviChunk::Audio);
  card_broken = true;
  TEST_ASSERT_TRUE(avi.next_chunk(off, len) == AviChunk::ReadError);
  TEST_ASSERT_TRUE(avi.next_chunk(off, len) == AviChunk::ReadError);
  TEST_ASSERT_EQUAL(1, avi.frame_pos());
  TEST_ASSERT_FALSE(avi.next(off, len));
  card_broken = false;
  TEST_ASSERT_TRUE(avi.next_chunk(off, len) == AviChunk::Video);
  TEST_ASSERT_EQUAL_STRING("F1", frame_at(off, len).c_str());
}

// 段比文件长（没写完的文件）：读到文件末尾算放完了，不算读卡出错
static void test_avi_truncated_end() {
  file = av_file(1, false, 4);
  file.resize(file.size() - 6);  // 最后一块声音只剩块头
  AviReader avi;
  TEST_ASSERT_TRUE(avi.open(read_file, nullptr, file.size()));
  uint32_t off, len;
  AviChunk c;
  int frames = 0;
  while ((c = avi.next_chunk(off, len)) != AviChunk::End) {
    TEST_ASSERT_TRUE(c != AviChunk::ReadError);
    if (c == AviChunk::Video) frames++;
  }
  TEST_ASSERT_EQUAL(4, frames);
}

// OpenDML（超过 1GB 的文件）：RIFF AVI 段后面接一个 RIFF AVIX 段，视频的 indx 指向每段的 ix00
static void test_avi_opendml() {
  const int per_seg = 4;
  // 先算出两段 movi 的内容，再回填 ix00 里的位置
  auto movi_body = [](int first) {
    std::string b;
    for (int i = first; i < first + per_seg; i++) b += chunk("01wb", "ss") + chunk("00dc", "V" + std::to_string(i));
    return b;
  };
  auto ix00 = [](uint32_t base, uint32_t movi_data) {
    std::string s;
    put16(s, 2);
    s += (char)0;
    s += (char)1;
    put32(s, per_seg);
    s += "00dc";
    put32(s, base);
    put32(s, 0);
    put32(s, 0);
    // 每段里的第 i 帧：前面有 i 对（声音 10 字节 + 视频 10 字节），再加这一帧前面那块声音
    for (int i = 0; i < per_seg; i++) {
      put32(s, movi_data - base + i * 20 + 10 + 8);
      put32(s, 2);
    }
    return chunk("ix00", s);
  };
  for (int pass = 0; pass < 2; pass++) {
    static uint32_t ix_at[2], movi_at[2];
    std::string indx;
    put16(indx, 4);
    indx += (char)0;
    indx += (char)0;
    put32(indx, 2);
    indx += "00dc";
    for (int i = 0; i < 3; i++) put32(indx, 0);
    for (int i = 0; i < 2; i++) {
      put32(indx, ix_at[i]);
      put32(indx, 0);
      put32(indx, 40);
      put32(indx, per_seg);
    }
    std::string odml = list("odml", chunk("dmlh", std::string("\x08\0\0\0", 4) + std::string(244, '\0')));
    std::string hdrl = list("hdrl", avih(50000, 320, 240) +
                                        list("strl", strh("vids", 1, 20, per_seg) + chunk("indx", indx)) +
                                        list("strl", strh("auds", 1, 16000, 0) + wavefmt(1, 1, 16000, 16)) + odml);
    std::string head = "RIFF....AVI " + hdrl;
    movi_at[0] = head.size() + 12;
    std::string seg0 = list("movi", movi_body(0) + std::string());
    ix_at[0] = head.size() + seg0.size();
    std::string riff0 = head + seg0 + ix00(0, movi_at[0]);
    std::string r0 = riff0.substr(0, 4);
    put32(r0, riff0.size() - 8);
    riff0 = r0 + riff0.substr(8);
    movi_at[1] = riff0.size() + 12 + 12;
    std::string seg1 = list("movi", movi_body(per_seg));
    ix_at[1] = riff0.size() + 12 + seg1.size();
    std::string avix = "AVIX" + seg1 + ix00(riff0.size(), movi_at[1]);
    std::string riff1 = "RIFF";
    put32(riff1, avix.size());
    file = riff0 + riff1 + avix;
  }
  AviReader avi;
  TEST_ASSERT_TRUE(avi.open(read_file, nullptr, file.size()));
  TEST_ASSERT_EQUAL(8, avi.frames());  // dmlh 里的总帧数
  uint32_t off, len;
  for (int i = 0; i < 8; i++) {
    TEST_ASSERT_TRUE(avi.next(off, len));
    TEST_ASSERT_EQUAL_STRING(("V" + std::to_string(i)).c_str(), frame_at(off, len).c_str());
  }
  TEST_ASSERT_FALSE(avi.next(off, len));
  for (int f : {6, 1, 4, 3, 7}) {
    TEST_ASSERT_TRUE(avi.seek_frame(f));
    TEST_ASSERT_EQUAL(f, avi.frame_pos());
    TEST_ASSERT_TRUE(avi.next(off, len));
    TEST_ASSERT_EQUAL_STRING(("V" + std::to_string(f)).c_str(), frame_at(off, len).c_str());
  }
}

// ---- 声音转换 ----

static std::string pcm(std::initializer_list<int> v) {
  std::string s;
  for (int x : v) put16(s, (uint16_t)(int16_t)x);
  return s;
}

static void test_pcm_16k() {
  PcmTo16k c;
  int16_t out[64];
  // 16kHz 单声道：原样输出（晚一个采样）
  c.reset(16000, 1);
  std::string in = pcm({100, 200, 300, 400});
  size_t n = c.convert((const uint8_t *)in.data(), in.size(), out, 64);
  TEST_ASSERT_EQUAL(3, n);
  TEST_ASSERT_EQUAL(100, out[0]);
  TEST_ASSERT_EQUAL(300, out[2]);
  // 半个采样留到下一块
  in = pcm({500, 600});
  n = c.convert((const uint8_t *)in.data(), 3, out, 64);
  TEST_ASSERT_EQUAL(1, n);
  TEST_ASSERT_EQUAL(400, out[0]);
  n = c.convert((const uint8_t *)in.data() + 3, 1, out, 64);
  TEST_ASSERT_EQUAL(1, n);
  TEST_ASSERT_EQUAL(500, out[0]);

  // 32kHz 立体声：两个声道取平均，每两个采样取一个
  c.reset(32000, 2);
  in = pcm({0, 100, 1000, 1000, 2000, 2200, 3000, 3000, 4000, 4000});
  n = c.convert((const uint8_t *)in.data(), in.size(), out, 64);
  TEST_ASSERT_EQUAL(2, n);
  TEST_ASSERT_EQUAL(50, out[0]);
  TEST_ASSERT_EQUAL(2100, out[1]);

  // 8kHz：中间插一个
  c.reset(8000, 1);
  in = pcm({0, 1000, -1000});
  n = c.convert((const uint8_t *)in.data(), in.size(), out, 64);
  TEST_ASSERT_EQUAL(4, n);
  const int16_t want[] = {0, 500, 1000, 0};
  TEST_ASSERT_EQUAL_INT16_ARRAY(want, out, 4);

  // 44.1kHz 的一秒转出来差不多 16000 个，不超过 max_out
  c.reset(44100, 2);
  std::string sec(44100 * 4, '\0');
  static int16_t big[20000];
  size_t cap = c.max_out(sec.size());
  TEST_ASSERT_TRUE(cap <= 20000);
  n = c.convert((const uint8_t *)sec.data(), sec.size(), big, cap);
  TEST_ASSERT_TRUE(n >= 15999 && n <= 16001);
  TEST_ASSERT_TRUE(n <= cap);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_gif_plain);
  RUN_TEST(test_gif_dispose3);
  RUN_TEST(test_gif_interlaced);
  RUN_TEST(test_gif_dispose2);
  RUN_TEST(test_gif_ffmpeg);
  RUN_TEST(test_gif_delay);
  RUN_TEST(test_gif_reject);
  RUN_TEST(test_gif_truncated);
  RUN_TEST(test_blit_fit);
  RUN_TEST(test_avi_frames);
  RUN_TEST(test_avi_defaults_and_truncated);
  RUN_TEST(test_avi_reject);
  RUN_TEST(test_avi_audio_and_seek);
  RUN_TEST(test_avi_unsupported_audio);
  RUN_TEST(test_avi_opendml);
  RUN_TEST(test_avi_read_error);
  RUN_TEST(test_avi_truncated_end);
  RUN_TEST(test_pcm_16k);
  return UNITY_END();
}
