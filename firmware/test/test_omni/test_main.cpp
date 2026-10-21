// AI 语音聊天协议（百炼实时接口、WebSocket）的单元测试：在电脑上运行：pio test -e native
#include <ArduinoJson.h>
#include <unity.h>

#include <string>
#include <vector>

#include "audio_sync.h"
#include "omni.h"
#include "realtime.h"
#include "ws.h"

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

struct Pcm {
  std::vector<int16_t> samples;
};

static void collect(const int16_t *pcm, size_t n, void *ctx) {
  auto *p = (Pcm *)ctx;
  p->samples.insert(p->samples.end(), pcm, pcm + n);
}

static void test_ws_handshake() {
  std::string r = ws_handshake("dashscope.aliyuncs.com", "/api-ws/v1/realtime?model=m", "dGhlIHNhbXBsZSBub25jZQ==",
                               "Authorization: Bearer sk\r\n");
  TEST_ASSERT_EQUAL_STRING(
      "GET /api-ws/v1/realtime?model=m HTTP/1.1\r\nHost: dashscope.aliyuncs.com\r\nUpgrade: websocket\r\n"
      "Connection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n"
      "Authorization: Bearer sk\r\n\r\n",
      r.c_str());
}

// 把客户端发的帧当成服务器收到的来解，验证帧头和掩码
struct Msgs {
  std::vector<std::pair<uint8_t, std::string>> list;
};

static void on_msg(uint8_t op, std::string &payload, void *ctx) {
  ((Msgs *)ctx)->list.push_back({op, payload});
}

static std::string client_frame(uint8_t op, const std::string &payload) {
  const uint8_t mask[4] = {0x12, 0x34, 0x56, 0x78};
  uint8_t head[14];
  size_t hn = ws_frame_header(op, payload.size(), mask, head);
  std::string body = payload;
  // 分两段加掩码，和固件里分块发送一样
  size_t half = body.size() / 2;
  ws_mask((uint8_t *)&body[0], half, mask, 0);
  ws_mask((uint8_t *)&body[half], body.size() - half, mask, half);
  return std::string((const char *)head, hn) + body;
}

static void test_ws_frame_sizes() {
  for (size_t len : {0u, 5u, 125u, 126u, 4268u, 65535u, 65536u, 200000u}) {
    std::string payload(len, 'a');
    for (size_t i = 0; i < len; i++) payload[i] = (char)('a' + i % 26);
    std::string f = client_frame(kWsText, payload);
    size_t expect_head = (len < 126 ? 2 : len < 65536 ? 4 : 10) + 4;
    TEST_ASSERT_EQUAL((int)(expect_head + len), (int)f.size());
    TEST_ASSERT_EQUAL(0x81, (uint8_t)f[0]);
    Msgs m;
    WsReader r(1 << 20);
    // 一个字节一个字节地喂也能拼回来（大的就整段喂）
    if (len < 300) {
      for (char c : f) TEST_ASSERT_TRUE(r.feed((const uint8_t *)&c, 1, on_msg, &m));
    } else {
      TEST_ASSERT_TRUE(r.feed((const uint8_t *)f.data(), f.size(), on_msg, &m));
    }
    TEST_ASSERT_EQUAL(1, (int)m.list.size());
    TEST_ASSERT_EQUAL(kWsText, m.list[0].first);
    TEST_ASSERT_TRUE(m.list[0].second == payload);
  }
}

static void test_ws_server_frames() {
  // 服务器的帧没有掩码：一条分成两片的文本，中间夹一个 ping，后面再来一条短消息
  std::string s;
  s += std::string("\x01\x03" "abc", 5);       // 文本，未完
  s += std::string("\x89\x02" "hi", 4);        // ping
  s += std::string("\x80\x02" "de", 4);        // 续片，完
  s += std::string("\x81\x7e\x00\x80", 4) + std::string(128, 'x');  // 128 字节，用 2 字节长度
  s += std::string("\x88\x02\x03\xe8", 4);     // close 1000
  Msgs m;
  WsReader r(1024);
  for (size_t i = 0; i < s.size(); i += 3) {
    TEST_ASSERT_TRUE(r.feed((const uint8_t *)s.data() + i, s.size() - i < 3 ? s.size() - i : 3, on_msg, &m));
  }
  TEST_ASSERT_EQUAL(4, (int)m.list.size());
  TEST_ASSERT_EQUAL(kWsPing, m.list[0].first);
  TEST_ASSERT_EQUAL_STRING("hi", m.list[0].second.c_str());
  TEST_ASSERT_EQUAL(kWsText, m.list[1].first);
  TEST_ASSERT_EQUAL_STRING("abcde", m.list[1].second.c_str());
  TEST_ASSERT_EQUAL(128, (int)m.list[2].second.size());
  TEST_ASSERT_EQUAL(kWsClose, m.list[3].first);
}

static void test_ws_too_big() {
  std::string s = std::string("\x81\x7e\x01\x00", 4) + std::string(256, 'x');
  Msgs m;
  WsReader r(100);
  TEST_ASSERT_FALSE(r.feed((const uint8_t *)s.data(), s.size(), on_msg, &m));
}

static void test_rt_model() {
  TEST_ASSERT_EQUAL_STRING("qwen3.8-omni-flash-realtime", rt_model("qwen3-omni-flash").c_str());
  TEST_ASSERT_EQUAL_STRING("qwen-omni-turbo-realtime", rt_model("qwen-omni-turbo-realtime").c_str());
  TEST_ASSERT_EQUAL_STRING("qwen3.5-omni-flash-realtime", rt_model("qwen3.5-omni-flash").c_str());
  TEST_ASSERT_EQUAL_STRING("qwen3.8-omni-flash-realtime", rt_model("").c_str());
}

static void test_rt_session_update() {
  std::string j = rt_session_update("Cherry", "你是\"小橘\"", 700, "gummy-realtime-v1");
  JsonDocument doc;
  TEST_ASSERT_FALSE(deserializeJson(doc, j));
  TEST_ASSERT_EQUAL_STRING("session.update", doc["type"]);
  JsonObject s = doc["session"];
  TEST_ASSERT_EQUAL_STRING("text", s["modalities"][0]);
  TEST_ASSERT_EQUAL_STRING("audio", s["modalities"][1]);
  TEST_ASSERT_EQUAL_STRING("Cherry", s["voice"]);
  TEST_ASSERT_EQUAL_STRING("你是\"小橘\"", s["instructions"]);
  TEST_ASSERT_EQUAL_STRING("pcm16", s["input_audio_format"]);
  TEST_ASSERT_EQUAL_STRING("pcm24", s["output_audio_format"]);
  TEST_ASSERT_EQUAL_STRING("server_vad", s["turn_detection"]["type"]);
  TEST_ASSERT_EQUAL(700, s["turn_detection"]["silence_duration_ms"].as<int>());
  TEST_ASSERT_EQUAL_STRING("gummy-realtime-v1", s["input_audio_transcription"]["model"]);
  // 不要文字时整个字段都不发
  JsonDocument doc2;
  TEST_ASSERT_FALSE(deserializeJson(doc2, rt_session_update("Cherry", "x", 700, nullptr)));
  TEST_ASSERT_FALSE(doc2["session"]["input_audio_transcription"].is<JsonObject>());
  // 音色留空时不发，让服务器用模型默认的
  JsonDocument doc3;
  TEST_ASSERT_FALSE(deserializeJson(doc3, rt_session_update("", "x", 700, nullptr)));
  TEST_ASSERT_FALSE(doc3["session"]["voice"].is<const char *>());
  TEST_ASSERT_EQUAL_STRING("{\"type\":\"response.cancel\"}", rt_event("response.cancel").c_str());
}

static void test_rt_append() {
  std::string j = std::string(kRtAppendHead) + enc("\x01\x02") + kRtAppendTail;
  JsonDocument doc;
  TEST_ASSERT_FALSE(deserializeJson(doc, j));
  TEST_ASSERT_EQUAL_STRING("input_audio_buffer.append", doc["type"]);
  TEST_ASSERT_EQUAL_STRING("AQI=", doc["audio"]);
}

static RtEvent feed(RtParser &p, std::string msg) { return p.feed(msg); }

static void test_rt_conversation() {
  Pcm pcm;
  RtParser p(collect, &pcm);
  TEST_ASSERT_EQUAL((int)RtEvent::None, (int)feed(p, "{\"event_id\":\"e1\",\"type\":\"session.created\",\"session\":{\"id\":\"s\"}}"));
  TEST_ASSERT_EQUAL((int)RtEvent::Ready, (int)feed(p, "{\"event_id\":\"e2\",\"type\":\"session.updated\",\"session\":{\"modalities\":[\"text\",\"audio\"]}}"));
  TEST_ASSERT_EQUAL((int)RtEvent::SpeechStarted, (int)feed(p, "{\"type\":\"input_audio_buffer.speech_started\",\"audio_start_ms\":120}"));
  TEST_ASSERT_EQUAL((int)RtEvent::SpeechStopped, (int)feed(p, "{\"type\":\"input_audio_buffer.speech_stopped\",\"audio_end_ms\":980}"));
  TEST_ASSERT_EQUAL((int)RtEvent::None, (int)feed(p, "{\"type\":\"input_audio_buffer.committed\"}"));
  TEST_ASSERT_EQUAL((int)RtEvent::Response, (int)feed(p, "{\"type\":\"response.created\",\"response\":{\"id\":\"r1\",\"status\":\"in_progress\"}}"));
  TEST_ASSERT_EQUAL_STRING("r1", p.response_id().c_str());
  TEST_ASSERT_EQUAL((int)RtEvent::Heard, (int)feed(p, "{\"type\":\"conversation.item.input_audio_transcription.completed\",\"item_id\":\"i\",\"transcript\":\"今天天气怎么样\"}"));
  TEST_ASSERT_EQUAL_STRING("今天天气怎么样", p.heard().c_str());
  TEST_ASSERT_EQUAL((int)RtEvent::Text, (int)feed(p, "{\"type\":\"response.audio_transcript.delta\",\"response_id\":\"r1\",\"delta\":\"晴天\"}"));
  // 一个采样被拆在两块里
  TEST_ASSERT_EQUAL((int)RtEvent::Audio, (int)feed(p, "{\"event_id\":\"e9\",\"type\":\"response.audio.delta\",\"response_id\":\"r1\",\"item_id\":\"i2\",\"output_index\":0,\"content_index\":0,\"delta\":\"" + enc(std::string("\x01\x00\x02", 3)) + "\"}"));
  TEST_ASSERT_EQUAL_STRING("r1", p.response_id().c_str());
  TEST_ASSERT_EQUAL((int)RtEvent::Audio, (int)feed(p, "{\"type\":\"response.audio.delta\",\"delta\":\"" + enc(std::string("\x00\xff\xff", 3)) + "\"}"));
  TEST_ASSERT_TRUE(p.response_id().empty());
  TEST_ASSERT_EQUAL((int)RtEvent::Text, (int)feed(p, "{\"type\":\"response.audio_transcript.delta\",\"delta\":\"，出门不用带伞\"}"));
  TEST_ASSERT_EQUAL_STRING("晴天，出门不用带伞", p.text().c_str());
  TEST_ASSERT_EQUAL((int)RtEvent::Done, (int)feed(p, "{\"type\":\"response.done\",\"response\":{\"id\":\"r1\",\"status\":\"completed\",\"output\":[{\"type\":\"message\"}]}}"));
  TEST_ASSERT_TRUE(p.error().empty());
  TEST_ASSERT_EQUAL(3, (int)pcm.samples.size());
  TEST_ASSERT_EQUAL(1, pcm.samples[0]);
  TEST_ASSERT_EQUAL(2, pcm.samples[1]);
  TEST_ASSERT_EQUAL(-1, pcm.samples[2]);
  // 下一次回答的文字重新开始
  feed(p, "{\"type\":\"response.created\",\"response\":{\"id\":\"r2\"}}");
  TEST_ASSERT_TRUE(p.text().empty());
  TEST_ASSERT_EQUAL_STRING("r2", p.response_id().c_str());
  // 被打断的旧回答还在陆续送来的数据，靠 response_id 认出来扔掉
  feed(p, "{\"type\":\"response.audio_transcript.delta\",\"response_id\":\"r1\",\"delta\":\"x\"}");
  TEST_ASSERT_EQUAL_STRING("r1", p.response_id().c_str());
}

static void test_ref_aligner() {
  RefAligner a;
  TEST_ASSERT_FALSE(a.ready());
  TEST_ASSERT_EQUAL(0, a.offset());
  // 每次量到的偏移在 1000 上下抖动一个 DMA 缓冲，平均下来接近 1000
  for (int i = 0; i < 200; i++) a.add(1000 + (i % 2 ? 128 : -128));
  TEST_ASSERT_TRUE(a.ready());
  TEST_ASSERT_INT_WITHIN(2, 1000, a.offset());
  // 只看最近 64 次
  for (int i = 0; i < 64; i++) a.add(-500);
  TEST_ASSERT_EQUAL(-500, a.offset());
}

static void test_ring_at() {
  int16_t ring[8] = {};
  uint32_t written = 0;
  for (int i = 0; i < 11; i++) ring[written++ & 7] = (int16_t)(100 + i);  // 写了 11 个，前 3 个已被覆盖
  TEST_ASSERT_EQUAL(0, ring_at(ring, 8, written, 2));
  TEST_ASSERT_EQUAL(103, ring_at(ring, 8, written, 3));
  TEST_ASSERT_EQUAL(110, ring_at(ring, 8, written, 10));
  TEST_ASSERT_EQUAL(0, ring_at(ring, 8, written, 11));  // 还没写到
  TEST_ASSERT_EQUAL(0, ring_at(ring, 8, written, 500));
  // 计数溢出回绕
  written = 2;
  ring[0xFFFFFFFFu & 7] = 7;
  ring[0] = 8;
  ring[1] = 9;
  TEST_ASSERT_EQUAL(7, ring_at(ring, 8, written, 0xFFFFFFFFu));
  TEST_ASSERT_EQUAL(9, ring_at(ring, 8, written, 1));
}

static void test_rt_errors() {
  RtParser p(nullptr, nullptr);
  TEST_ASSERT_EQUAL((int)RtEvent::Error, (int)feed(p, "{\"type\":\"error\",\"error\":{\"type\":\"invalid_request_error\",\"code\":\"Arrearage\",\"message\":\"Access denied\"}}"));
  TEST_ASSERT_EQUAL_STRING("Arrearage", p.error_code().c_str());
  TEST_ASSERT_EQUAL_STRING("Access denied", p.error().c_str());
  feed(p, "{\"type\":\"response.created\",\"response\":{\"id\":\"r3\"}}");
  TEST_ASSERT_TRUE(p.error().empty());
  TEST_ASSERT_EQUAL((int)RtEvent::Done, (int)feed(p, "{\"type\":\"response.done\",\"response\":{\"status\":\"failed\",\"status_details\":{\"type\":\"failed\",\"error\":{\"code\":\"x\",\"message\":\"internal\"}}}}"));
  TEST_ASSERT_EQUAL_STRING("internal", p.error().c_str());
  TEST_ASSERT_EQUAL((int)RtEvent::None, (int)feed(p, "not json"));
}

static void test_agc() {
  Agc agc(8);
  std::vector<int16_t> quiet(1600, 1000);
  for (int i = 0; i < 200; i++) {
    std::vector<int16_t> b = quiet;
    agc.process(b.data(), b.size());
  }
  TEST_ASSERT_EQUAL(8 * 256, agc.gain_x256());  // 一直很小声就放大到上限
  std::vector<int16_t> loud(1600, 20000);
  int peak = agc.process(loud.data(), loud.size());
  TEST_ASSERT_EQUAL(20000, peak);
  TEST_ASSERT_EQUAL(20000, loud[0]);  // 突然变响马上降下来，不削顶
  std::vector<int16_t> mid(1600, 2500);
  agc.process(mid.data(), mid.size());
  TEST_ASSERT_TRUE(mid[0] < 2600);  // 刚才很响，不会一下子又放大
}

static void test_error_message() {
  std::string m = omni_error_message(401, "{\"error\":{\"message\":\"Incorrect API key provided.\",\"code\":\"invalid_api_key\"}}");
  TEST_ASSERT_EQUAL_STRING("API Key 不对或已失效（Incorrect API key provided.）", m.c_str());
  TEST_ASSERT_EQUAL_STRING("服务器返回 500", omni_error_message(500, "oops").c_str());
  TEST_ASSERT_EQUAL_STRING("model not found", omni_error_message(400, "{\"code\":\"x\",\"message\":\"model not found\"}").c_str());
}

static void test_rt_is_goodbye() {
  TEST_ASSERT_TRUE(rt_is_goodbye("拜拜"));
  TEST_ASSERT_TRUE(rt_is_goodbye("好的，再见！"));
  TEST_ASSERT_TRUE(rt_is_goodbye("先这样吧。"));
  TEST_ASSERT_TRUE(rt_is_goodbye("Bye bye."));
  TEST_ASSERT_TRUE(rt_is_goodbye("小橘退下吧"));
  TEST_ASSERT_FALSE(rt_is_goodbye(""));
  TEST_ASSERT_FALSE(rt_is_goodbye("今天天气怎么样"));
  // 长句里提到「再见」不算道别
  TEST_ASSERT_FALSE(rt_is_goodbye("你知道再见这个词用英语怎么说吗"));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_base64_roundtrip);
  RUN_TEST(test_base64_split_and_padded_chunks);
  RUN_TEST(test_resample_24k_to_16k);
  RUN_TEST(test_http_head);
  RUN_TEST(test_ws_handshake);
  RUN_TEST(test_ws_frame_sizes);
  RUN_TEST(test_ws_server_frames);
  RUN_TEST(test_ws_too_big);
  RUN_TEST(test_rt_model);
  RUN_TEST(test_rt_session_update);
  RUN_TEST(test_rt_append);
  RUN_TEST(test_rt_conversation);
  RUN_TEST(test_rt_errors);
  RUN_TEST(test_agc);
  RUN_TEST(test_ref_aligner);
  RUN_TEST(test_ring_at);
  RUN_TEST(test_error_message);
  RUN_TEST(test_rt_is_goodbye);
  return UNITY_END();
}
