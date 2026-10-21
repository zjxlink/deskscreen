#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>

// WebSocket（RFC 6455）客户端里和网络无关的部分：握手请求、发送帧的帧头和掩码、接收帧的拆包。
// 收发本身在 voice.cpp 里用 WiFiClientSecure 做，这里可以在电脑上测试

enum WsOpcode : uint8_t {
  kWsContinuation = 0x0,
  kWsText = 0x1,
  kWsBinary = 0x2,
  kWsClose = 0x8,
  kWsPing = 0x9,
  kWsPong = 0xA,
};

// 升级请求。key 是 16 个随机字节的 base64；extra 是额外的请求头，每行以 \r\n 结尾
std::string ws_handshake(const char *host, const char *path, const char *key, const std::string &extra);

// 客户端发的帧必须加掩码。把帧头（FIN + opcode + 长度 + 掩码，最多 14 字节）写进 out，返回字节数
size_t ws_frame_header(uint8_t opcode, uint64_t len, const uint8_t mask[4], uint8_t out[14]);

// 给负载加掩码（再调用一次就是去掉掩码）。offset 是这一段在整个负载里的位置，方便分段处理
void ws_mask(uint8_t *data, size_t n, const uint8_t mask[4], uint64_t offset);

// 拆收到的帧：可以随意分段喂。数据消息（分片的会拼起来）和控制帧都整条交给回调
class WsReader {
 public:
  using MessageFn = void (*)(uint8_t opcode, std::string &payload, void *ctx);
  explicit WsReader(size_t max_message) : max_(max_message) {}

  // 返回 false 表示数据不对或消息太长，连接应该断开
  bool feed(const uint8_t *in, size_t n, MessageFn fn, void *ctx);

 private:
  enum class State { Head, Payload };
  State state_ = State::Head;
  uint8_t head_[14];
  size_t head_len_ = 0;
  size_t head_need_ = 2;
  uint64_t left_ = 0;
  uint64_t done_ = 0;  // 这一帧已经收到的负载字节数，解掩码用
  uint8_t opcode_ = 0;
  bool fin_ = false;
  bool masked_ = false;
  uint8_t mask_[4] = {};
  uint8_t msg_opcode_ = 0;  // 正在拼的数据消息的类型
  std::string msg_;
  std::string ctrl_;
  size_t max_;
};
