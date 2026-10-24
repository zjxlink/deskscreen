// 聊天记录（气泡）的单元测试，在电脑上运行：pio test -e native
#include <stdio.h>
#include <string.h>
#include <unity.h>

#include "chat_log.h"

void setUp() {}
void tearDown() {}

static void test_order_with_late_transcript() {
  ChatLog log;
  log.user_started();
  log.user_started();  // 服务器重复报开口，不加第二个占位
  TEST_ASSERT_EQUAL(1, log.count());
  TEST_ASSERT_TRUE(log.line(0).me);
  TEST_ASSERT_TRUE(log.line(0).pending);
  TEST_ASSERT_EQUAL_STRING("……", log.line(0).text);
  // 回答先到，识别结果后到：主人那句还在前面
  log.reply_started();
  log.reply_text("今天");
  log.reply_text("今天晴天。");
  log.user_heard("今天天气怎么样");
  TEST_ASSERT_EQUAL(2, log.count());
  TEST_ASSERT_EQUAL_STRING("今天天气怎么样", log.line(0).text);
  TEST_ASSERT_FALSE(log.line(0).pending);
  TEST_ASSERT_FALSE(log.line(1).me);
  TEST_ASSERT_EQUAL_STRING("今天晴天。", log.line(1).text);
}

static void test_barge_in_keeps_reply_line() {
  ChatLog log;
  log.user_heard("讲个故事");  // 没有占位时直接加
  log.reply_started();
  log.reply_text("从前有座山，山里有座庙。");
  log.user_started();  // 插话
  log.cut_reply(5);
  log.user_heard("换一个");
  TEST_ASSERT_EQUAL(3, log.count());
  TEST_ASSERT_EQUAL_STRING("从前有座山……", log.line(1).text);
  TEST_ASSERT_EQUAL_STRING("换一个", log.line(2).text);
  // 后到的回答文字写进最后一句小橘的话，不会跑到主人那句前后乱掉
  log.reply_started();
  log.user_heard("");  // 没有占位可去，什么也不做
  TEST_ASSERT_EQUAL(4, log.count());
}

static void test_cut_before_speaking_removes_line() {
  ChatLog log;
  log.reply_started();
  log.reply_text("好的");
  log.cut_reply(0);
  TEST_ASSERT_EQUAL(0, log.count());
  // 全念完了再打断，不加省略号
  log.reply_started();
  log.reply_text("好的");
  log.cut_reply(2);
  TEST_ASSERT_EQUAL_STRING("好的", log.line(0).text);
}

static void test_empty_transcript_and_drop_pending() {
  ChatLog log;
  log.user_started();
  log.user_heard("");  // 只是噪音
  TEST_ASSERT_EQUAL(0, log.count());
  log.user_started();
  log.reply_started();
  log.reply_text("嗯？");
  log.drop_pending();
  TEST_ASSERT_EQUAL(1, log.count());
  TEST_ASSERT_FALSE(log.line(0).me);
}

static void test_oldest_dropped_and_rev() {
  ChatLog log;
  uint32_t rev = log.rev();
  for (int i = 0; i < kChatLines + 2; i++) {
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", i);
    log.user_heard(buf);
  }
  TEST_ASSERT_EQUAL(kChatLines, log.count());
  TEST_ASSERT_EQUAL_STRING("2", log.line(0).text);
  TEST_ASSERT_NOT_EQUAL(rev, log.rev());
  rev = log.rev();
  log.clear();
  TEST_ASSERT_EQUAL(0, log.count());
  TEST_ASSERT_NOT_EQUAL(rev, log.rev());
}

static void test_long_text_and_tilde() {
  ChatLog log;
  std::string s;
  for (int i = 0; i < 200; i++) s += "猫";
  log.reply_text(s);
  TEST_ASSERT_TRUE(strlen(log.line(0).text) < kChatLineLen);
  TEST_ASSERT_EQUAL(0, strlen(log.line(0).text) % 3);  // 没把汉字切成两半
  log.reply_text("好呀\xEF\xBD\x9E");
  TEST_ASSERT_EQUAL_STRING("好呀~", log.line(0).text);
}

static void test_utf8_helpers() {
  TEST_ASSERT_EQUAL(4, utf8_chars("你好ab"));
  TEST_ASSERT_EQUAL(6, utf8_prefix("你好ab", 2));
  TEST_ASSERT_EQUAL(7, utf8_prefix("你好ab", 3));
  TEST_ASSERT_EQUAL(8, utf8_prefix("你好ab", 99));
  TEST_ASSERT_EQUAL(0, utf8_prefix("你好", 0));
}

// 记录满了以后，很长的英文回答被打断：截断后接「……」不能写出这一句（以前会改坏 count）
static void test_cut_long_ascii_reply() {
  ChatLog log;
  for (int i = 0; i < 3; i++) {
    log.user_heard("hi");
    log.reply_started();
    log.reply_text("hello");
  }
  TEST_ASSERT_EQUAL(kChatLines, log.count());
  std::string longer(kChatLineLen - 1, 'a');
  log.reply_started();
  log.reply_text(longer);
  int n = log.count();
  log.cut_reply(kChatLineLen - 3);
  TEST_ASSERT_EQUAL(n, log.count());
  const char *t = log.line(n - 1).text;
  size_t len = strlen(t);
  TEST_ASSERT_TRUE(len < kChatLineLen);
  TEST_ASSERT_EQUAL_STRING("……", t + len - strlen("……"));
  // 中文也一样，不能切出半个字
  std::string cn;
  while (cn.size() + 3 < kChatLineLen) cn += "好";
  log.reply_started();
  log.reply_text(cn);
  log.cut_reply(utf8_chars(cn.c_str()) - 1);
  t = log.line(log.count() - 1).text;
  TEST_ASSERT_TRUE(strlen(t) < kChatLineLen);
  TEST_ASSERT_EQUAL(0, (strlen(t) - strlen("……")) % 3);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_cut_long_ascii_reply);
  RUN_TEST(test_order_with_late_transcript);
  RUN_TEST(test_barge_in_keeps_reply_line);
  RUN_TEST(test_cut_before_speaking_removes_line);
  RUN_TEST(test_empty_transcript_and_drop_pending);
  RUN_TEST(test_oldest_dropped_and_rev);
  RUN_TEST(test_long_text_and_tilde);
  RUN_TEST(test_utf8_helpers);
  return UNITY_END();
}
