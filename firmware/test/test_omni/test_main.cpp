// AI 语音聊天协议的单元测试：在电脑上运行：pio test -e native
#include <ArduinoJson.h>
#include <unity.h>

#include <string>
#include <vector>

#include "omni.h"

void setUp() {}
void tearDown() {}

static std::string enc(const std::string &s) {
  std::string out(s.size() * 4 / 3 + 4, '\0');
  out.resize(b64_encode((const uint8_t *)s.data(), s.size(), &out[0]));
  return out;
}

static std::string dec(B64Decoder &d, const std::string &s) {
  std::vector<uint8_t> out(s.size() + 4);
  size_t n = d.feed(s.data(), s.size(), out.data());
  return std::string((const char *)out.data(), n);
}

static void test_base64_roundtrip() {
  TEST_ASSERT_EQUAL_STRING("", enc("").c_str());
  TEST_ASSERT_EQUAL_STRING("Zg==", enc("f").c_str());
  TEST_ASSERT_EQUAL_STRING("Zm8=", enc("fo").c_str());
  TEST_ASSERT_EQUAL_STRING("Zm9v", enc("foo").c_str());
  TEST_ASSERT_EQUAL_STRING("Zm9vYmFy", enc("foobar").c_str());
  std::string bin;
  for (int i = 0; i < 300; i++) bin += (char)(i * 37);
  B64Decoder d;
  TEST_ASSERT_TRUE(dec(d, enc(bin)) == bin);
}

static void test_base64_split_and_padded_chunks() {
  // 一段 base64 拆成任意几块喂，结果一样
  std::string data = "hello, xiaoju! 0123456789";
  std::string b = enc(data);
  B64Decoder d;
  std::string got;
  for (size_t i = 0; i < b.size(); i += 5) got += dec(d, b.substr(i, 5));
  TEST_ASSERT_TRUE(got == data);
  // 每块各自编码、带 '=' 再拼起来，也能正确解出来
  B64Decoder d2;
  got = dec(d2, enc("ab") + enc("cde") + enc("f"));
  TEST_ASSERT_EQUAL_STRING("abcdef", got.c_str());
}

static void test_wav_header() {
  uint8_t h[44];
  wav_header(h, 16000, 16000);
  TEST_ASSERT_EQUAL_MEMORY("RIFF", h, 4);
  TEST_ASSERT_EQUAL_MEMORY("WAVEfmt ", h + 8, 8);
  TEST_ASSERT_EQUAL_MEMORY("data", h + 36, 4);
  uint32_t data = h[40] | h[41] << 8 | h[42] << 16 | (uint32_t)h[43] << 24;
  TEST_ASSERT_EQUAL_UINT32(32000, data);
  uint32_t rate = h[24] | h[25] << 8 | h[26] << 16;
  TEST_ASSERT_EQUAL_UINT32(16000, rate);
  TEST_ASSERT_EQUAL(1, h[22]);  // 单声道
  TEST_ASSERT_EQUAL(16, h[34]);
}

static void test_normalize() {
  int16_t quiet[4] = {100, -200, 50, 0};
  pcm_normalize(quiet, 4, 8);
  TEST_ASSERT_EQUAL(800, quiet[0]);  // 最多放大 8 倍
  TEST_ASSERT_EQUAL(-1600, quiet[1]);
  int16_t mid[2] = {5000, -2000};
  pcm_normalize(mid, 2, 8);
  TEST_ASSERT_INT_WITHIN(150, 23000, mid[0]);
  int16_t loud[2] = {30000, -32768};
  pcm_normalize(loud, 2, 8);
  TEST_ASSERT_EQUAL(30000, loud[0]);  // 已经够响就不动
  int16_t silent[3] = {0, 0, 0};
  pcm_normalize(silent, 3, 8);
  TEST_ASSERT_EQUAL(0, silent[1]);
}

static void test_resample_24k_to_16k() {
  // 一个 24kHz 的斜坡，分几段喂，输出个数是 2/3，数值仍在斜坡上
  std::vector<int16_t> in(2400);
  for (size_t i = 0; i < in.size(); i++) in[i] = (int16_t)(i * 10);
  Resampler r(24000, 16000);
  std::vector<int16_t> out(2000);
  size_t n = 0;
  for (size_t i = 0; i < in.size(); i += 333) {
    size_t take = in.size() - i < 333 ? in.size() - i : 333;
    n += r.feed(&in[i], take, &out[n]);
  }
  TEST_ASSERT_INT_WITHIN(2, 1600, (int)n);
  for (size_t k = 2; k < n; k++) {
    // 第 k 个输出对应输入的第 1.5k - 1 个位置（最开始从 0 插值过来）
    float expect = (k * 1.5f - 1) * 10;
    TEST_ASSERT_INT_WITHIN(2, (int)expect, out[k]);
  }
}

static void test_request_body() {
  OmniRequest r = omni_request("qwen3-omni-flash", "Cherry", "你是\"小橘\"");
  std::string json = r.head + enc("RIFFxx") + r.tail;
  JsonDocument doc;
  TEST_ASSERT_FALSE(deserializeJson(doc, json));
  TEST_ASSERT_EQUAL_STRING("qwen3-omni-flash", doc["model"]);
  TEST_ASSERT_TRUE(doc["stream"].as<bool>());
  TEST_ASSERT_EQUAL_STRING("audio", doc["modalities"][1]);
  TEST_ASSERT_EQUAL_STRING("Cherry", doc["audio"]["voice"]);
  TEST_ASSERT_EQUAL_STRING("你是\"小橘\"", doc["messages"][0]["content"]);
  JsonObject a = doc["messages"][1]["content"][0];
  TEST_ASSERT_EQUAL_STRING("input_audio", a["type"]);
  TEST_ASSERT_EQUAL_STRING("wav", a["input_audio"]["format"]);
  std::string data = a["input_audio"]["data"].as<std::string>();
  TEST_ASSERT_TRUE(data == "data:;base64," + enc("RIFFxx"));
}

static void test_http_head() {
  const char resp[] = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nTransfer-Encoding: chunked\r\n\r\nBODY";
  HttpHead h;
  size_t used = 0;
  // 分两段喂
  TEST_ASSERT_FALSE(h.feed(resp, 20, used));
  TEST_ASSERT_EQUAL(20, used);
  TEST_ASSERT_TRUE(h.feed(resp + 20, sizeof(resp) - 1 - 20, used));
  TEST_ASSERT_EQUAL_STRING("BODY", resp + 20 + used);
  TEST_ASSERT_EQUAL(200, h.status());
  TEST_ASSERT_TRUE(h.chunked());

  HttpHead e;
  const char bad[] = "HTTP/1.1 401 Unauthorized\r\ncontent-length: 10\r\n\r\n";
  TEST_ASSERT_TRUE(e.feed(bad, sizeof(bad) - 1, used));
  TEST_ASSERT_EQUAL(401, e.status());
  TEST_ASSERT_FALSE(e.chunked());
}

static void test_chunked() {
  std::string body = "5\r\nhello\r\nb;ext=1\r\n, xiaoju!!!\r\n0\r\n\r\n";
  // 一个字节一个字节地喂，也能拼对
  ChunkedDecoder d;
  std::string out;
  for (char c : body) {
    char buf[2];
    size_t n = d.feed(&c, 1, buf);
    out.append(buf, n);
  }
  TEST_ASSERT_EQUAL_STRING("hello, xiaoju!!!", out.c_str());
  TEST_ASSERT_TRUE(d.done());
  // 整段一次喂
  ChunkedDecoder d2;
  std::vector<char> buf(body.size());
  size_t n = d2.feed(body.data(), body.size(), buf.data());
  TEST_ASSERT_EQUAL_STRING("hello, xiaoju!!!", std::string(buf.data(), n).c_str());
}

struct Pcm {
  std::vector<int16_t> samples;
};

static void collect(const int16_t *pcm, size_t n, void *ctx) {
  auto *p = (Pcm *)ctx;
  p->samples.insert(p->samples.end(), pcm, pcm + n);
}

static std::string sse_audio(const std::string &bytes) {
  return "data: {\"choices\":[{\"delta\":{\"audio\":{\"data\":\"" + enc(bytes) + "\"}}}]}\n\n";
}

static void test_stream_audio_and_text() {
  std::string s;
  s += "data: {\"choices\":[{\"delta\":{\"role\":\"assistant\",\"content\":\"\"}}]}\n\n";
  s += "data: {\"choices\":[{\"delta\":{\"audio\":{\"transcript\":\"你好\"}}}]}\n\n";
  // 一个采样被拆在两块里
  s += sse_audio(std::string("\x01\x00\x02", 3));
  s += sse_audio(std::string("\x00\xff\xff", 3));
  s += "data: {\"choices\":[{\"delta\":{\"audio\":{\"transcript\":\"，我是小橘\"}}}]}\r\n\r\n";
  s += "data: {\"choices\":[],\"usage\":{\"total_tokens\":10}}\n\n";
  s += "data: [DONE]\n\n";
  Pcm pcm;
  OmniStream st(collect, &pcm);
  // 随便切成几段喂
  for (size_t i = 0; i < s.size(); i += 7) st.feed(s.data() + i, s.size() - i < 7 ? s.size() - i : 7);
  TEST_ASSERT_TRUE(st.done());
  TEST_ASSERT_EQUAL_STRING("你好，我是小橘", st.text().c_str());
  TEST_ASSERT_EQUAL(3, (int)pcm.samples.size());
  TEST_ASSERT_EQUAL(1, pcm.samples[0]);
  TEST_ASSERT_EQUAL(2, pcm.samples[1]);
  TEST_ASSERT_EQUAL(-1, pcm.samples[2]);
  TEST_ASSERT_TRUE(st.error().empty());
}

static void test_stream_skips_wav_header() {
  uint8_t h[44];
  wav_header(h, 2, 24000);
  std::string bytes((const char *)h, 44);
  bytes += std::string("\x10\x00\x20\x00", 4);
  Pcm pcm;
  OmniStream st(collect, &pcm);
  std::string s = sse_audio(bytes.substr(0, 10)) + sse_audio(bytes.substr(10));
  st.feed(s.data(), s.size());
  TEST_ASSERT_EQUAL(2, (int)pcm.samples.size());
  TEST_ASSERT_EQUAL(16, pcm.samples[0]);
  TEST_ASSERT_EQUAL(32, pcm.samples[1]);
  TEST_ASSERT_EQUAL(4, (int)st.audio_bytes());
}

static void test_stream_prefers_content_text() {
  std::string s = "data: {\"choices\":[{\"delta\":{\"content\":\"今天\"}}]}\n";
  s += "data: {\"choices\":[{\"delta\":{\"audio\":{\"transcript\":\"今天\"}}}]}\n";
  s += "data: {\"choices\":[{\"delta\":{\"content\":\"晴\"}}]}\n";
  OmniStream st(nullptr, nullptr);
  st.feed(s.data(), s.size());
  TEST_ASSERT_EQUAL_STRING("今天晴", st.text().c_str());  // 两种文字都有时不重复
}

static void test_stream_error() {
  std::string s = "data: {\"error\":{\"message\":\"Arrearage\",\"code\":\"Arrearage\"}}\n\n";
  OmniStream st(nullptr, nullptr);
  st.feed(s.data(), s.size());
  TEST_ASSERT_TRUE(st.done());
  TEST_ASSERT_EQUAL_STRING("Arrearage", st.error().c_str());
}

static void test_error_message() {
  std::string m = omni_error_message(401, "{\"error\":{\"message\":\"Incorrect API key provided.\",\"code\":\"invalid_api_key\"}}");
  TEST_ASSERT_EQUAL_STRING("API Key 不对或已失效（Incorrect API key provided.）", m.c_str());
  TEST_ASSERT_EQUAL_STRING("服务器返回 500", omni_error_message(500, "oops").c_str());
  TEST_ASSERT_EQUAL_STRING("model not found", omni_error_message(400, "{\"code\":\"x\",\"message\":\"model not found\"}").c_str());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_base64_roundtrip);
  RUN_TEST(test_base64_split_and_padded_chunks);
  RUN_TEST(test_wav_header);
  RUN_TEST(test_normalize);
  RUN_TEST(test_resample_24k_to_16k);
  RUN_TEST(test_request_body);
  RUN_TEST(test_http_head);
  RUN_TEST(test_chunked);
  RUN_TEST(test_stream_audio_and_text);
  RUN_TEST(test_stream_skips_wav_header);
  RUN_TEST(test_stream_prefers_content_text);
  RUN_TEST(test_stream_error);
  RUN_TEST(test_error_message);
  return UNITY_END();
}
