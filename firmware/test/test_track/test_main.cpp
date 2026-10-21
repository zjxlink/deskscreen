#include <math.h>
#include <string.h>
#include <unity.h>

#include <string>
#include <vector>

#include "resample.h"
#include "text_conv.h"
#include "track.h"

void setUp() {}
void tearDown() {}

using Bytes = std::vector<uint8_t>;

static size_t read_vec(void *ctx, uint32_t off, uint8_t *buf, size_t len) {
  const Bytes &b = *(const Bytes *)ctx;
  if (off >= b.size()) return 0;
  size_t n = b.size() - off < len ? b.size() - off : len;
  memcpy(buf, b.data() + off, n);
  return n;
}

static void put(Bytes &b, const void *p, size_t n) { b.insert(b.end(), (const uint8_t *)p, (const uint8_t *)p + n); }
static void put(Bytes &b, const char *s) { put(b, s, strlen(s)); }
static void be32(Bytes &b, uint32_t v) {
  uint8_t x[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
  put(b, x, 4);
}
static void le32(Bytes &b, uint32_t v) {
  uint8_t x[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
  put(b, x, 4);
}
static void le16(Bytes &b, uint16_t v) {
  uint8_t x[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
  put(b, x, 2);
}

// ID3v2.3 的一帧
static void id3_frame(Bytes &b, const char *id, const Bytes &body) {
  put(b, id, 4);
  be32(b, body.size());
  uint8_t flags[2] = {0, 0};
  put(b, flags, 2);
  put(b, body.data(), body.size());
}

static Bytes text_body(uint8_t enc, const Bytes &text) {
  Bytes b = {enc};
  put(b, text.data(), text.size());
  return b;
}

// MPEG1 Layer3 128kbps 44.1kHz 立体声，每帧 417 字节
static void mp3_frame(Bytes &b, const uint8_t *payload = nullptr, size_t n = 0) {
  size_t start = b.size();
  uint8_t h[4] = {0xFF, 0xFB, 0x90, 0x00};
  put(b, h, 4);
  b.resize(start + 417, 0);
  if (payload) memcpy(b.data() + start + 4 + 32, payload, n);
}

static Bytes make_mp3(bool xing, int frames) {
  Bytes tag;
  // 歌名 UTF-16（带 BOM）：「晴天」
  Bytes t16 = {0xFF, 0xFE, 0x74, 0x66, 0x29, 0x59, 0, 0};
  id3_frame(tag, "TIT2", text_body(1, t16));
  // 歌手 GBK（编码写的是 0）：「周杰伦」
  id3_frame(tag, "TPE1", text_body(0, {0xD6, 0xDC, 0xBD, 0xDC, 0xC2, 0xD7}));
  // 专辑 UTF-8
  Bytes alb;
  put(alb, "叶惠美");
  id3_frame(tag, "TALB", text_body(3, alb));
  // 封面：编码 0、mime、类型 3、说明「」、JPG 数据
  Bytes pic = {0};
  put(pic, "image/jpeg");
  pic.push_back(0);
  pic.push_back(3);
  pic.push_back(0);
  uint8_t jpg[] = {0xFF, 0xD8, 0xFF, 0xE0, 1, 2, 3, 0xFF, 0xD9};
  put(pic, jpg, sizeof(jpg));
  id3_frame(tag, "APIC", pic);
  tag.resize(tag.size() + 20, 0);  // 填充

  Bytes f;
  put(f, "ID3");
  uint8_t ver[3] = {3, 0, 0};
  put(f, ver, 3);
  uint32_t s = tag.size();
  uint8_t ss[4] = {(uint8_t)(s >> 21 & 0x7F), (uint8_t)(s >> 14 & 0x7F), (uint8_t)(s >> 7 & 0x7F), (uint8_t)(s & 0x7F)};
  put(f, ss, 4);
  put(f, tag.data(), tag.size());
  if (xing) {
    Bytes x;
    put(x, "Xing");
    be32(x, 1 | 4);  // 帧数、目录
    be32(x, frames);
    for (int i = 0; i < 100; i++) x.push_back((uint8_t)(i * 256 / 100));
    mp3_frame(f, x.data(), x.size());
  }
  for (int i = 0; i < frames; i++) mp3_frame(f);
  return f;
}

static void test_format() {
  TEST_ASSERT_EQUAL(TrackFormat::Mp3, track_format("晴天.MP3"));
  TEST_ASSERT_EQUAL(TrackFormat::Flac, track_format("华语/a.flac"));
  TEST_ASSERT_EQUAL(TrackFormat::Wav, track_format("a.wav"));
  TEST_ASSERT_EQUAL(TrackFormat::None, track_format("a.m4a"));
  TEST_ASSERT_EQUAL(TrackFormat::None, track_format("._晴天.mp3"));
  TEST_ASSERT_EQUAL(TrackFormat::None, track_format("华语/.mp3"));
  TEST_ASSERT_EQUAL(TrackFormat::None, track_format("mp3"));
  TEST_ASSERT_EQUAL(TrackFormat::None, track_format("dir.mp3/x"));
  char lrc[64];
  TEST_ASSERT_TRUE(track_lrc_path("华语/晴天.mp3", lrc, sizeof(lrc)));
  TEST_ASSERT_EQUAL_STRING("华语/晴天.lrc", lrc);
}

static void test_text() {
  char out[64];
  const uint8_t gbk[] = {0xC4, 0xE3, 0xBA, 0xC3, 'A'};  // 你好A
  text_guess(gbk, sizeof(gbk), out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("你好A", out);
  const uint8_t utf8[] = "你好";
  text_guess(utf8, sizeof(utf8) - 1, out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("你好", out);
  const uint8_t latin[] = {'C', 'a', 'f', 0xE9};  // Café，不是合法 GBK（0xE9 后面没有字节了）
  text_guess(latin, sizeof(latin), out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("Caf\xC3\xA9", out);
  const uint8_t be[] = {0xFE, 0xFF, 0x4F, 0x60, 0xD8, 0x3D, 0xDE, 0x00};  // 你😀
  text_from_utf16(be, sizeof(be), false, out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("你\xF0\x9F\x98\x80", out);
  // 放不下时在整字处截断
  text_guess(utf8, sizeof(utf8) - 1, out, 5);
  TEST_ASSERT_EQUAL_STRING("你", out);
  char s[] = "晴天晴天";
  utf8_truncate(s, 7);
  TEST_ASSERT_EQUAL_STRING("晴天", s);
  char t[] = "\xE3\x80\x80  晴天 \r\n";
  text_trim(t);
  TEST_ASSERT_EQUAL_STRING("晴天", t);
}

static void test_mp3_xing() {
  Bytes f = make_mp3(true, 100);
  TrackProbe p;
  TEST_ASSERT_TRUE(track_probe(read_vec, &f, f.size(), TrackFormat::Mp3, p));
  TEST_ASSERT_EQUAL_STRING("晴天", p.title);
  TEST_ASSERT_EQUAL_STRING("周杰伦", p.artist);
  TEST_ASSERT_EQUAL_STRING("叶惠美", p.album);
  TEST_ASSERT_EQUAL(44100, p.rate);
  TEST_ASSERT_EQUAL(2, p.channels);
  TEST_ASSERT_EQUAL(1152, p.mp3_frame_samples);
  TEST_ASSERT_EQUAL(100 * 1152 * 1000 / 44100, p.dur_ms);
  TEST_ASSERT_TRUE(p.mp3_has_toc);
  TEST_ASSERT_EQUAL(9, p.cover_len);
  TEST_ASSERT_EQUAL_HEX8(0xFF, f[p.cover_off]);
  TEST_ASSERT_EQUAL_HEX8(0xD8, f[p.cover_off + 1]);
  TEST_ASSERT_EQUAL_HEX8(0xFF, f[p.data_off]);
  // 定位：一半时间大约在一半的位置
  uint32_t mid = track_mp3_seek_offset(p, p.dur_ms / 2);
  TEST_ASSERT_UINT32_WITHIN(p.data_len / 50, p.data_off + p.data_len / 2, mid);
  TEST_ASSERT_EQUAL(p.data_off, track_mp3_seek_offset(p, 0));
}

static void test_mp3_cbr_and_v1() {
  Bytes f;
  uint8_t junk[] = {0xFF, 0xFB, 0x12, 0x34, 0xFF};  // 像帧头的垃圾
  put(f, junk, sizeof(junk));
  for (int i = 0; i < 50; i++) mp3_frame(f);
  Bytes v1(128, 0);
  memcpy(v1.data(), "TAG", 3);
  const uint8_t title[] = {0xC7, 0xE7, 0xCC, 0xEC};  // GBK「晴天」
  memcpy(v1.data() + 3, title, 4);
  memcpy(v1.data() + 33, "Jay", 3);
  put(f, v1.data(), v1.size());
  TrackProbe p;
  TEST_ASSERT_TRUE(track_probe(read_vec, &f, f.size(), TrackFormat::Mp3, p));
  TEST_ASSERT_EQUAL(5, p.data_off);
  TEST_ASSERT_EQUAL(50 * 417, p.data_len);
  TEST_ASSERT_EQUAL_STRING("晴天", p.title);
  TEST_ASSERT_EQUAL_STRING("Jay", p.artist);
  TEST_ASSERT_EQUAL_STRING("", p.album);
  TEST_ASSERT_UINT32_WITHIN(5, 50 * 417 * 8000 / 128000, p.dur_ms);
  TEST_ASSERT_FALSE(p.mp3_has_toc);
  TEST_ASSERT_EQUAL(0, p.cover_len);
  TEST_ASSERT_EQUAL(5 + 16000, track_mp3_seek_offset(p, 1000));
  Bytes bad = {'h', 'e', 'l', 'l', 'o'};
  TEST_ASSERT_FALSE(track_probe(read_vec, &bad, bad.size(), TrackFormat::Mp3, p));
}

static void test_flac() {
  Bytes f;
  put(f, "fLaC");
  // STREAMINFO：44.1kHz、2 声道、16 位、441000 个采样（10 秒）
  f.push_back(0x00);
  f.push_back(0);
  f.push_back(0);
  f.push_back(34);
  Bytes si(34, 0);
  uint32_t rate = 44100;
  uint64_t total = 441000;
  si[10] = rate >> 12;
  si[11] = rate >> 4;
  si[12] = (uint8_t)((rate & 0xF) << 4 | (2 - 1) << 1 | ((16 - 1) >> 4));
  si[13] = (uint8_t)(((16 - 1) & 0xF) << 4 | (total >> 32 & 0xF));
  si[14] = total >> 24;
  si[15] = total >> 16;
  si[16] = total >> 8;
  si[17] = total;
  put(f, si.data(), si.size());
  // VORBIS_COMMENT
  Bytes vc;
  le32(vc, 3);
  put(vc, "abc");
  le32(vc, 2);
  const char *c1 = "TITLE=稻香", *c2 = "artist=周杰伦";
  le32(vc, strlen(c1));
  put(vc, c1);
  le32(vc, strlen(c2));
  put(vc, c2);
  f.push_back(0x04);
  f.push_back(0);
  f.push_back(vc.size() >> 8);
  f.push_back(vc.size());
  put(f, vc.data(), vc.size());
  // PICTURE（最后一块）
  Bytes pic;
  be32(pic, 3);
  be32(pic, 10);
  put(pic, "image/jpeg");
  be32(pic, 0);
  for (int i = 0; i < 4; i++) be32(pic, 0);
  be32(pic, 4);
  uint8_t jpg[] = {0xFF, 0xD8, 0xFF, 0xD9};
  put(pic, jpg, 4);
  f.push_back(0x86);
  f.push_back(0);
  f.push_back(pic.size() >> 8);
  f.push_back(pic.size());
  put(f, pic.data(), pic.size());
  size_t audio = f.size();
  f.resize(f.size() + 100, 0);
  TrackProbe p;
  TEST_ASSERT_TRUE(track_probe(read_vec, &f, f.size(), TrackFormat::Flac, p));
  TEST_ASSERT_EQUAL(44100, p.rate);
  TEST_ASSERT_EQUAL(2, p.channels);
  TEST_ASSERT_EQUAL(16, p.bits);
  TEST_ASSERT_EQUAL(10000, p.dur_ms);
  TEST_ASSERT_EQUAL_STRING("稻香", p.title);
  TEST_ASSERT_EQUAL_STRING("周杰伦", p.artist);
  TEST_ASSERT_EQUAL(4, p.cover_len);
  TEST_ASSERT_EQUAL_HEX8(0xD8, f[p.cover_off + 1]);
  TEST_ASSERT_EQUAL(audio, p.data_off);
}

static void test_wav() {
  Bytes f;
  put(f, "RIFF");
  le32(f, 0);
  put(f, "WAVE");
  put(f, "LIST");
  le32(f, 3);
  put(f, "abc");
  f.push_back(0);  // 奇数长度的块后面补一个字节
  put(f, "fmt ");
  le32(f, 16);
  le16(f, 1);
  le16(f, 2);
  le32(f, 48000);
  le32(f, 48000 * 4);
  le16(f, 4);
  le16(f, 16);
  put(f, "data");
  le32(f, 0);  // 长度没写（边录边存的文件）
  size_t data = f.size();
  f.resize(data + 48000 * 4 * 2 + 3, 0);
  TrackProbe p;
  TEST_ASSERT_TRUE(track_probe(read_vec, &f, f.size(), TrackFormat::Wav, p));
  TEST_ASSERT_EQUAL(48000, p.rate);
  TEST_ASSERT_EQUAL(2, p.channels);
  TEST_ASSERT_EQUAL(16, p.bits);
  TEST_ASSERT_EQUAL(data, p.data_off);
  TEST_ASSERT_EQUAL(48000 * 4 * 2, p.data_len);
  TEST_ASSERT_EQUAL(2000, p.dur_ms);
  // 8 位的不支持
  f[data - 8 - 2] = 8;
  f[data - 8 - 4] = 2;
  TEST_ASSERT_FALSE(track_probe(read_vec, &f, f.size(), TrackFormat::Wav, p));
}

static void test_name_from_file() {
  char title[kTagMax], artist[kTagMax];
  track_name_from_file("华语/周杰伦 - 晴天.mp3", title, artist, sizeof(title));
  TEST_ASSERT_EQUAL_STRING("晴天", title);
  TEST_ASSERT_EQUAL_STRING("周杰伦", artist);
  track_name_from_file("小星星.flac", title, artist, sizeof(title));
  TEST_ASSERT_EQUAL_STRING("小星星", title);
  TEST_ASSERT_EQUAL_STRING("", artist);
  track_name_from_file("a-b.mp3", title, artist, sizeof(title));
  TEST_ASSERT_EQUAL_STRING("a-b", title);
}

static void test_safe_name() {
  char out[64];
  TEST_ASSERT_TRUE(track_safe_name("周杰伦 - 晴天.MP3", out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("周杰伦 - 晴天.mp3", out);
  TEST_ASSERT_TRUE(track_safe_name("../a:b?.flac", out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("ab.flac", out);
  TEST_ASSERT_TRUE(track_safe_name("歌词 .lrc", out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("歌词.lrc", out);
  TEST_ASSERT_FALSE(track_safe_name("a.m4a", out, sizeof(out)));
  TEST_ASSERT_FALSE(track_safe_name(".mp3", out, sizeof(out)));
  TEST_ASSERT_FALSE(track_safe_name("mp3", out, sizeof(out)));
  // 太长：截短时不切断汉字，扩展名留着
  TEST_ASSERT_TRUE(track_safe_name("一二三四五六七八九十.mp3", out, 16));
  TEST_ASSERT_EQUAL_STRING("一二三.mp3", out);
  TEST_ASSERT_TRUE(track_safe_folder(" 华语/老歌. ", out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("华语老歌", out);
  TEST_ASSERT_FALSE(track_safe_folder("..", out, sizeof(out)));
}

static void test_lrc() {
  const char *text =
      "[ti:晴天]\r\n[ar:周杰伦]\r\n[offset:500]\r\n"
      "[00:10.50]第一句\r\n"
      "[00:20.00][01:20.00]副歌\r\n"
      "[00:05]  前奏  \r\n"
      "[00:30.00]\r\n"
      "坏的一行\r\n"
      "[00:40.123]最后";
  LrcLine lines[16];
  char pool[512];
  Lyrics l = {lines, 16, pool, sizeof(pool), 0};
  lrc_parse((const uint8_t *)text, strlen(text), l);
  TEST_ASSERT_EQUAL(6, l.count);
  TEST_ASSERT_EQUAL(4500, l.lines[0].ms);
  TEST_ASSERT_EQUAL_STRING("前奏", lrc_text(l, 0));
  TEST_ASSERT_EQUAL(10000, l.lines[1].ms);
  TEST_ASSERT_EQUAL_STRING("第一句", lrc_text(l, 1));
  TEST_ASSERT_EQUAL_STRING("副歌", lrc_text(l, 2));
  TEST_ASSERT_EQUAL_STRING("", lrc_text(l, 3));  // 间奏
  TEST_ASSERT_EQUAL(39623, l.lines[4].ms);
  TEST_ASSERT_EQUAL(79500, l.lines[5].ms);
  TEST_ASSERT_EQUAL_STRING("副歌", lrc_text(l, 5));
  TEST_ASSERT_EQUAL(-1, lrc_find(l, 1000));
  TEST_ASSERT_EQUAL(0, lrc_find(l, 4500));
  TEST_ASSERT_EQUAL(1, lrc_find(l, 15000));
  TEST_ASSERT_EQUAL(5, lrc_find(l, 999999));

  // GBK 的歌词
  const uint8_t gbk[] = "[00:01.00]\xC4\xE3\xBA\xC3";
  lrc_parse(gbk, sizeof(gbk) - 1, l);
  TEST_ASSERT_EQUAL(1, l.count);
  TEST_ASSERT_EQUAL_STRING("你好", lrc_text(l, 0));
}

static void test_index_line() {
  TrackInfo t = {};
  strcpy(t.path, "华语/晴天.mp3");
  strcpy(t.title, "晴\t天");
  strcpy(t.artist, "周杰伦");
  t.size = 4123456;
  t.dur_ms = 269000;
  char line[512];
  size_t n = track_index_line(t, line, sizeof(line));
  TEST_ASSERT_TRUE(n > 0);
  TEST_ASSERT_EQUAL('\n', line[n - 1]);
  TrackInfo u;
  TEST_ASSERT_TRUE(track_index_parse(line, n - 1, u));
  TEST_ASSERT_EQUAL_STRING("华语/晴天.mp3", u.path);
  TEST_ASSERT_EQUAL_STRING("晴 天", u.title);
  TEST_ASSERT_EQUAL_STRING("周杰伦", u.artist);
  TEST_ASSERT_EQUAL_STRING("", u.album);
  TEST_ASSERT_EQUAL(4123456, u.size);
  TEST_ASSERT_EQUAL(269000, u.dur_ms);
  TEST_ASSERT_FALSE(track_index_parse("1\t2\tx.txt\ta\tb\tc", 15, u));
  TEST_ASSERT_FALSE(track_index_parse("garbage", 7, u));
  char folder[32];
  track_folder("华语/晴天.mp3", folder, sizeof(folder));
  TEST_ASSERT_EQUAL_STRING("华语", folder);
  track_folder("晴天.mp3", folder, sizeof(folder));
  TEST_ASSERT_EQUAL_STRING("", folder);
}

static PlayOrder order;

static void test_order() {
  order.set(5, 2, PlayMode::Loop, 1);
  TEST_ASSERT_EQUAL(2, order.current());
  TEST_ASSERT_EQUAL(3, order.next(false));
  TEST_ASSERT_EQUAL(4, order.next(true));
  TEST_ASSERT_EQUAL(0, order.next(false));  // 放到头接着从第一首放
  TEST_ASSERT_EQUAL(4, order.prev());
  order.set_mode(PlayMode::One, 1);
  TEST_ASSERT_EQUAL(4, order.next(false));  // 单曲循环：放完还是这首
  TEST_ASSERT_EQUAL(0, order.next(true));   // 点下一首照样换
  // 随机：当前这首在最前面，每首都放一遍才重复
  order.set(10, 7, PlayMode::Shuffle, 12345);
  TEST_ASSERT_EQUAL(7, order.current());
  bool seen[10] = {};
  seen[7] = true;
  for (int i = 0; i < 9; i++) {
    int k = order.next(false);
    TEST_ASSERT_FALSE(seen[k]);
    seen[k] = true;
  }
  order.jump(3);
  TEST_ASSERT_EQUAL(3, order.current());
  order.set(0, 0, PlayMode::Loop, 0);
  TEST_ASSERT_EQUAL(-1, order.current());
  TEST_ASSERT_EQUAL(-1, order.next(true));
  TEST_ASSERT_EQUAL(15, sleep_next_choice(0));
  TEST_ASSERT_EQUAL(30, sleep_next_choice(15));
  TEST_ASSERT_EQUAL(60, sleep_next_choice(30));
  TEST_ASSERT_EQUAL(0, sleep_next_choice(60));
}

// 44.1kHz 的 1kHz 正弦波转成 16kHz：个数对、频率还是 1kHz；7kHz 以上的被压下去
static double tone_power(uint32_t rate, double freq, size_t &out_n) {
  Resampler16k r;
  r.reset(rate);
  std::vector<int16_t> in(rate), out(16000 + 16);
  for (size_t i = 0; i < in.size(); i++) in[i] = (int16_t)(10000 * sin(2 * M_PI * freq * i / rate));
  out_n = 0;
  for (size_t at = 0; at < in.size(); at += 441) {
    size_t n = in.size() - at < 441 ? in.size() - at : 441;
    out_n += r.process(in.data() + at, n, out.data() + out_n, out.size() - out_n);
  }
  double p = 0;
  for (size_t i = 100; i < out_n; i++) p += (double)out[i] * out[i];
  return p / (out_n - 100);
}

static void test_resample() {
  size_t n;
  double pass = tone_power(44100, 1000, n);
  TEST_ASSERT_UINT32_WITHIN(2, 16000, n);
  TEST_ASSERT_TRUE(pass > 0.8 * 10000.0 * 10000 / 2);
  double stop = tone_power(44100, 12000, n);  // 不滤掉会混叠成 4kHz
  TEST_ASSERT_TRUE(stop < pass * 0.1);
  tone_power(48000, 1000, n);
  TEST_ASSERT_UINT32_WITHIN(2, 16000, n);
  Resampler16k same;
  same.reset(16000);
  int16_t a[3] = {1, 2, 3}, b[3];
  TEST_ASSERT_EQUAL(3, same.process(a, 3, b, 3));
  TEST_ASSERT_EQUAL(3, b[2]);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_format);
  RUN_TEST(test_text);
  RUN_TEST(test_mp3_xing);
  RUN_TEST(test_mp3_cbr_and_v1);
  RUN_TEST(test_flac);
  RUN_TEST(test_wav);
  RUN_TEST(test_name_from_file);
  RUN_TEST(test_safe_name);
  RUN_TEST(test_lrc);
  RUN_TEST(test_index_line);
  RUN_TEST(test_order);
  RUN_TEST(test_resample);
  return UNITY_END();
}
