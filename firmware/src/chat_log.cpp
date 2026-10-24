#include "chat_log.h"

#include <string.h>

static const char kPending[] = "……";

// 截断时不能把一个汉字切成两半。屏幕字库里没有全角波浪号，换成普通的
static void copy_line(char *dst, std::string src) {
  for (size_t at; (at = src.find("\xEF\xBD\x9E")) != std::string::npos;) src.replace(at, 3, "~");
  size_t n = src.size() < kChatLineLen - 1 ? src.size() : kChatLineLen - 1;
  while (n > 0 && n < src.size() && ((uint8_t)src[n] & 0xC0) == 0x80) n--;
  memcpy(dst, src.data(), n);
  dst[n] = 0;
}

void ChatLog::clear() {
  count_ = 0;
  rev_++;
}

ChatLine &ChatLog::push(bool me) {
  if (count_ == kChatLines) remove(0);
  ChatLine &l = lines_[count_++];
  l.me = me;
  l.pending = false;
  l.text[0] = 0;
  rev_++;
  return l;
}

void ChatLog::remove(int i) {
  for (int j = i; j + 1 < count_; j++) lines_[j] = lines_[j + 1];
  count_--;
  rev_++;
}

void ChatLog::user_started() {
  if (count_ > 0 && lines_[count_ - 1].pending) return;
  ChatLine &l = push(true);
  l.pending = true;
  strcpy(l.text, kPending);
}

void ChatLog::user_heard(const std::string &text) {
  int at = -1;
  for (int i = 0; i < count_; i++) {
    if (lines_[i].pending) {
      at = i;
      break;
    }
  }
  if (text.empty()) {
    if (at >= 0) remove(at);
    return;
  }
  ChatLine &l = at >= 0 ? lines_[at] : push(true);
  l.pending = false;
  copy_line(l.text, text);
  rev_++;
}

void ChatLog::reply_started() { push(false); }

int ChatLog::last_reply() const {
  for (int i = count_ - 1; i >= 0; i--) {
    if (!lines_[i].me) return i;
  }
  return -1;
}

void ChatLog::reply_text(const std::string &text) {
  int at = last_reply();
  ChatLine &l = at >= 0 ? lines_[at] : push(false);
  copy_line(l.text, text);
  rev_++;
}

void ChatLog::cut_reply(size_t chars) {
  int at = last_reply();
  if (at < 0) return;
  char *t = lines_[at].text;
  size_t n = utf8_prefix(t, chars);
  if (!t[n]) return;
  if (n == 0) {
    remove(at);
    return;
  }
  // 放不下「……」就往回退整字，直到放得下（按字节算，英文一个字只省 1 字节）
  while (n > 0 && n + sizeof(kPending) > kChatLineLen) {
    do n--;
    while (n > 0 && ((uint8_t)t[n] & 0xC0) == 0x80);
  }
  strcpy(t + n, kPending);
  rev_++;
}

void ChatLog::drop_pending() {
  for (int i = count_ - 1; i >= 0; i--) {
    if (lines_[i].pending) remove(i);
  }
}

size_t utf8_chars(const char *s) {
  size_t n = 0;
  for (; *s; s++) {
    if (((uint8_t)*s & 0xC0) != 0x80) n++;
  }
  return n;
}

size_t utf8_prefix(const char *s, size_t n) {
  size_t i = 0;
  for (; s[i]; i++) {
    if (((uint8_t)s[i] & 0xC0) != 0x80) {
      if (n == 0) break;
      n--;
    }
  }
  return i;
}
