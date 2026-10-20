// 电子相册的单元测试：在电脑上运行：pio test -e native
#include <string.h>
#include <unity.h>

#include "album.h"

void setUp() {}
void tearDown() {}

static void test_is_photo() {
  TEST_ASSERT_TRUE(album_is_photo("IMG_0001.JPG"));
  TEST_ASSERT_TRUE(album_is_photo("cat.jpeg"));
  TEST_ASSERT_TRUE(album_is_photo("截图.png"));
  TEST_ASSERT_FALSE(album_is_photo("IMG_0002.HEIC"));
  TEST_ASSERT_FALSE(album_is_photo("notes.txt"));
  TEST_ASSERT_FALSE(album_is_photo("._IMG_0001.JPG"));  // macOS 拷贝时生成的
  TEST_ASSERT_FALSE(album_is_photo(".upload.jpg"));
  TEST_ASSERT_FALSE(album_is_photo("jpg"));
  TEST_ASSERT_FALSE(album_is_photo(""));
}

static void test_safe_name() {
  char out[64];
  album_safe_name("IMG_1234.HEIC", "x", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("IMG_1234.jpg", out);
  album_safe_name("../../etc/pass wd.jpg", "x", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("etcpasswd.jpg", out);
  album_safe_name("小猫.jpg", "20261020_121314", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("20261020_121314.jpg", out);
  album_safe_name(nullptr, nullptr, out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("p.jpg", out);
  album_safe_name("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.jpg", "x", out, sizeof(out));
  TEST_ASSERT_EQUAL(44, strlen(out));
  char small[10];
  album_safe_name("abcdefgh.jpg", "x", small, sizeof(small));
  TEST_ASSERT_EQUAL_STRING("abcde.jpg", small);
}

static void test_order_sequential() {
  static PhotoOrder o;
  o.reset(3, false, 1);
  bool round;
  TEST_ASSERT_EQUAL(0, o.next(&round));
  TEST_ASSERT_TRUE(round);
  TEST_ASSERT_EQUAL(1, o.next(&round));
  TEST_ASSERT_FALSE(round);
  TEST_ASSERT_EQUAL(2, o.next());
  TEST_ASSERT_EQUAL(0, o.next(&round));
  TEST_ASSERT_TRUE(round);
}

static void test_order_empty() {
  static PhotoOrder o;
  o.reset(0, true, 1);
  TEST_ASSERT_EQUAL(-1, o.next());
}

static void test_order_shuffle_covers_all_without_repeat() {
  static PhotoOrder o;
  o.reset(20, true, 12345);
  int last = -1;
  for (int r = 0; r < 50; r++) {
    bool seen[20] = {};
    for (int i = 0; i < 20; i++) {
      int k = o.next();
      TEST_ASSERT_TRUE(k >= 0 && k < 20);
      TEST_ASSERT_FALSE(seen[k]);
      seen[k] = true;
      TEST_ASSERT_NOT_EQUAL(last, k);  // 跨轮也不会连着放同一张
      last = k;
    }
  }
}

static void test_order_two_photos_alternate() {
  static PhotoOrder o;
  o.reset(2, true, 7);
  int last = o.next();
  for (int i = 0; i < 40; i++) {
    int k = o.next();
    TEST_ASSERT_NOT_EQUAL(last, k);
    last = k;
  }
}

static void test_order_shrink_after_delete() {
  static PhotoOrder o;
  o.reset(5, false, 1);
  for (int i = 0; i < 5; i++) o.next();
  o.reset(2, true, 3);  // 删了几张
  for (int i = 0; i < 10; i++) {
    int k = o.next();
    TEST_ASSERT_TRUE(k >= 0 && k < 2);
  }
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_is_photo);
  RUN_TEST(test_safe_name);
  RUN_TEST(test_order_sequential);
  RUN_TEST(test_order_empty);
  RUN_TEST(test_order_shuffle_covers_all_without_repeat);
  RUN_TEST(test_order_two_photos_alternate);
  RUN_TEST(test_order_shrink_after_delete);
  return UNITY_END();
}
