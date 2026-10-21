// 设置网页配对登录的单元测试，在电脑上运行：pio test -e native
#include <string.h>
#include <unity.h>

#include "web_auth.h"

void setUp() {}
void tearDown() {}

static uint32_t seq = 0;
static uint32_t fake_rand() { return 0x1234567u + (seq++) * 0x9E3779B9u; }

static void test_code_reused_until_expired() {
  WebAuth a;
  seq = 0;
  const char *c1 = web_auth_start(a, 1000, fake_rand);
  TEST_ASSERT_EQUAL(6, strlen(c1));
  char first[7];
  strcpy(first, c1);
  TEST_ASSERT_EQUAL_STRING(first, web_auth_start(a, 60000, fake_rand));  // 刷新网页不换码
  TEST_ASSERT_TRUE(web_auth_code_active(a, 1000 + kWebAuthCodeMs - 1));
  TEST_ASSERT_FALSE(web_auth_code_active(a, 1000 + kWebAuthCodeMs));
  TEST_ASSERT_NOT_EQUAL(0, strcmp(first, web_auth_start(a, 1000 + kWebAuthCodeMs, fake_rand)));
}

static void test_pair_and_check() {
  WebAuth a;
  char code[7], token[kWebAuthTokenLen + 1];
  strcpy(code, web_auth_start(a, 0, fake_rand));
  TEST_ASSERT_FALSE(web_auth_check(a, "00000000000000000000000000000000"));
  TEST_ASSERT_TRUE(web_auth_pair(a, code, 10, fake_rand, token));
  TEST_ASSERT_EQUAL(kWebAuthTokenLen, strlen(token));
  TEST_ASSERT_TRUE(web_auth_check(a, token));
  TEST_ASSERT_FALSE(web_auth_code_active(a, 20));             // 用过的码作废
  TEST_ASSERT_FALSE(web_auth_pair(a, code, 20, fake_rand, token));
  TEST_ASSERT_FALSE(web_auth_check(a, nullptr));
  TEST_ASSERT_FALSE(web_auth_check(a, ""));
  token[0] = token[0] == 'a' ? 'b' : 'a';
  TEST_ASSERT_FALSE(web_auth_check(a, token));
}

static void test_wrong_code_limit() {
  WebAuth a;
  char code[7], wrong[7], token[kWebAuthTokenLen + 1];
  strcpy(code, web_auth_start(a, 0, fake_rand));
  strcpy(wrong, code);
  wrong[0] = wrong[0] == '9' ? '0' : wrong[0] + 1;
  for (int i = 0; i < kWebAuthMaxTries; i++) TEST_ASSERT_FALSE(web_auth_pair(a, wrong, 1, fake_rand, token));
  TEST_ASSERT_FALSE(web_auth_code_active(a, 2));
  TEST_ASSERT_FALSE(web_auth_pair(a, code, 2, fake_rand, token));  // 错满后对的也不行
  TEST_ASSERT_EQUAL(0, a.count);
}

static void test_oldest_token_dropped() {
  WebAuth a;
  char first[kWebAuthTokenLen + 1], token[kWebAuthTokenLen + 1];
  for (int i = 0; i <= kWebAuthMaxTokens; i++) {
    char code[7];
    strcpy(code, web_auth_start(a, 0, fake_rand));
    TEST_ASSERT_TRUE(web_auth_pair(a, code, 0, fake_rand, token));
    if (i == 0) strcpy(first, token);
  }
  TEST_ASSERT_EQUAL(kWebAuthMaxTokens, a.count);
  TEST_ASSERT_FALSE(web_auth_check(a, first));
  TEST_ASSERT_TRUE(web_auth_check(a, token));
  web_auth_forget_all(a);
  TEST_ASSERT_FALSE(web_auth_check(a, token));
}

static void test_save_load() {
  WebAuth a, b;
  char token[kWebAuthTokenLen + 1], code[7];
  for (int i = 0; i < 3; i++) {
    strcpy(code, web_auth_start(a, 0, fake_rand));
    web_auth_pair(a, code, 0, fake_rand, token);
  }
  char buf[kWebAuthMaxTokens * (kWebAuthTokenLen + 1)];
  web_auth_save_tokens(a, buf, sizeof(buf));
  web_auth_load_tokens(b, buf);
  TEST_ASSERT_EQUAL(3, b.count);
  TEST_ASSERT_TRUE(web_auth_check(b, token));
  web_auth_load_tokens(b, "bad,0123456789abcdef0123456789ABCDEF,,0123456789abcdef0123456789abcdef");
  TEST_ASSERT_EQUAL(1, b.count);  // 长度不对、大写的都跳过
  web_auth_load_tokens(b, nullptr);
  TEST_ASSERT_EQUAL(0, b.count);
}

static void test_cookie() {
  char v[40];
  TEST_ASSERT_TRUE(web_auth_cookie("a=1; ds_auth=abc; b=2", "ds_auth", v, sizeof(v)));
  TEST_ASSERT_EQUAL_STRING("abc", v);
  TEST_ASSERT_TRUE(web_auth_cookie("ds_auth=xyz", "ds_auth", v, sizeof(v)));
  TEST_ASSERT_EQUAL_STRING("xyz", v);
  TEST_ASSERT_FALSE(web_auth_cookie("xds_auth=1; ds_authx=2", "ds_auth", v, sizeof(v)));
  TEST_ASSERT_FALSE(web_auth_cookie("", "ds_auth", v, sizeof(v)));
  TEST_ASSERT_FALSE(web_auth_cookie(nullptr, "ds_auth", v, sizeof(v)));
  TEST_ASSERT_FALSE(web_auth_cookie("ds_auth=0123456789", "ds_auth", v, 5));  // 放不下
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_code_reused_until_expired);
  RUN_TEST(test_pair_and_check);
  RUN_TEST(test_wrong_code_limit);
  RUN_TEST(test_oldest_token_dropped);
  RUN_TEST(test_save_load);
  RUN_TEST(test_cookie);
  return UNITY_END();
}
