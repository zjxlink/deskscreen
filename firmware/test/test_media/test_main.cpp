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
  return UNITY_END();
}
