#include "ws.h"

#include <string.h>

std::string ws_handshake(const char *host, const char *path, const char *key, const std::string &extra) {
  std::string s = "GET ";
  s += path;
  s += " HTTP/1.1\r\nHost: ";
  s += host;
  s += "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ";
  s += key;
  s += "\r\nSec-WebSocket-Version: 13\r\n";
  s += extra;
  s += "\r\n";
  return s;
}

size_t ws_frame_header(uint8_t opcode, uint64_t len, const uint8_t mask[4], uint8_t out[14]) {
  size_t n = 0;
  out[n++] = 0x80 | (opcode & 0x0F);
  if (len < 126) {
    out[n++] = 0x80 | (uint8_t)len;
  } else if (len < 65536) {
    out[n++] = 0x80 | 126;
    out[n++] = (uint8_t)(len >> 8);
    out[n++] = (uint8_t)len;
  } else {
    out[n++] = 0x80 | 127;
    for (int i = 7; i >= 0; i--) out[n++] = (uint8_t)(len >> (8 * i));
  }
  memcpy(out + n, mask, 4);
  return n + 4;
}

void ws_mask(uint8_t *data, size_t n, const uint8_t mask[4], uint64_t offset) {
  for (size_t i = 0; i < n; i++) data[i] ^= mask[(offset + i) & 3];
}

bool WsReader::feed(const uint8_t *in, size_t n, MessageFn fn, void *ctx) {
  size_t i = 0;
  while (i < n) {
    if (state_ == State::Head) {
      head_[head_len_++] = in[i++];
      if (head_len_ == 2) {
        int len7 = head_[1] & 0x7F;
        head_need_ = 2 + (len7 == 126 ? 2 : len7 == 127 ? 8 : 0) + (head_[1] & 0x80 ? 4 : 0);
      }
      if (head_len_ < head_need_) continue;
      fin_ = head_[0] & 0x80;
      opcode_ = head_[0] & 0x0F;
      masked_ = head_[1] & 0x80;
      int len7 = head_[1] & 0x7F;
      size_t at = 2;
      if (len7 == 126) {
        left_ = (uint64_t)head_[2] << 8 | head_[3];
        at = 4;
      } else if (len7 == 127) {
        left_ = 0;
        for (int k = 0; k < 8; k++) left_ = left_ << 8 | head_[2 + k];
        at = 10;
      } else {
        left_ = len7;
      }
      if (masked_) memcpy(mask_, head_ + at, 4);
      head_len_ = 0;
      head_need_ = 2;
      done_ = 0;
      bool control = opcode_ & 0x08;
      if (control) {
        if (!fin_ || left_ > 125) return false;  // 控制帧不能分片，也不能超过 125 字节
        ctrl_.clear();
      } else if (opcode_ == kWsContinuation) {
        if (msg_opcode_ == 0) return false;  // 没有开头的分片
      } else {
        if (msg_opcode_ != 0) return false;  // 上一条还没收完又来一条新的
        msg_opcode_ = opcode_;
        msg_.clear();
      }
      if (!control && msg_.size() + left_ > max_) return false;
      state_ = State::Payload;
    } else {
      size_t take = n - i < left_ ? n - i : (size_t)left_;
      bool control = opcode_ & 0x08;
      std::string &dst = control ? ctrl_ : msg_;
      size_t at = dst.size();
      dst.append((const char *)in + i, take);
      if (masked_) ws_mask((uint8_t *)&dst[at], take, mask_, done_);
      i += take;
      left_ -= take;
      done_ += take;
    }
    if (state_ == State::Payload && left_ == 0) {
      state_ = State::Head;
      if (opcode_ & 0x08) {
        fn(opcode_, ctrl_, ctx);
      } else if (fin_) {
        uint8_t op = msg_opcode_;
        msg_opcode_ = 0;
        fn(op, msg_, ctx);
      }
    }
  }
  return true;
}
