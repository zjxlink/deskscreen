#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>

// AI 聊天页的对话记录：最近几句主人说的和小橘说的，画成左右两边的气泡。
// 不依赖硬件，可在电脑上测试

static constexpr int kChatLines = 6;          // 最多记这么多句，再多就把最早的挤掉
static constexpr size_t kChatLineLen = 288;   // 每句最多这么多字节（约 95 个汉字）

struct ChatLine {
  bool me;       // 主人说的（右边）；false 是小橘说的（左边）
  bool pending;  // 主人开口了，还没识别出说了什么，先显示「……」
  char text[kChatLineLen];
};

class ChatLog {
 public:
  void clear();
  // 服务器听到主人开口：先放一个「……」占位，等识别出来再填上。紧挨着的占位不重复加
  void user_started();
  // 识别出主人说的话：填进最早的占位；识别结果为空就去掉那个占位；没有占位时直接加一句
  void user_heard(const std::string &text);
  // 小橘开始一次新的回答
  void reply_started();
  // 小橘这次回答到目前为止的全部文字
  void reply_text(const std::string &text);
  // 小橘被打断了：只留下已经念出来的前 chars 个字，后面接「……」；一个字都没念就整句去掉
  void cut_reply(size_t chars);
  // 聊天结束：还没等到识别结果的占位都去掉
  void drop_pending();

  int count() const { return count_; }
  const ChatLine &line(int i) const { return lines_[i]; }
  // 内容每变一次加一，界面据此判断要不要重画
  uint32_t rev() const { return rev_; }

 private:
  ChatLine &push(bool me);
  int last_reply() const;
  void remove(int i);

  ChatLine lines_[kChatLines] = {};
  int count_ = 0;
  uint32_t rev_ = 0;
};

// UTF-8 字符数（不是字节数）
size_t utf8_chars(const char *s);
// 前 n 个字符占多少字节
size_t utf8_prefix(const char *s, size_t n);
