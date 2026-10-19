// 像素小猫的单元测试，在电脑上运行：pio test -e native
#include <string.h>
#include <unity.h>

#include "pet.h"

void setUp() {}
void tearDown() {}

static PetInputs daytime() {
  PetInputs in;
  in.minutes = 10 * 60;
  return in;
}

static void test_mood_priority() {
  PetInputs in = daytime();
  TEST_ASSERT_EQUAL(PetMood::Normal, pet_mood(in));

  in.pomo_today = 1;
  TEST_ASSERT_EQUAL(PetMood::Happy, pet_mood(in));

  in.stocks_ok = true;
  in.stock_avg_pct = -2.5f;
  TEST_ASSERT_EQUAL(PetMood::Sad, pet_mood(in));  // 大跌比开心优先

  in.night = true;
  TEST_ASSERT_EQUAL(PetMood::Sleep, pet_mood(in));  // 夜里一律睡觉
}

static void test_bored_only_in_afternoon_without_commits() {
  PetInputs in = daytime();
  in.github_ok = true;
  TEST_ASSERT_EQUAL(PetMood::Normal, pet_mood(in));
  in.minutes = 15 * 60;
  TEST_ASSERT_EQUAL(PetMood::Bored, pet_mood(in));
  in.github_today = 2;
  TEST_ASSERT_EQUAL(PetMood::Happy, pet_mood(in));

  // 没设置 GitHub 时不会因为没提交而无聊
  PetInputs no_gh = daytime();
  no_gh.minutes = 16 * 60;
  TEST_ASSERT_EQUAL(PetMood::Normal, pet_mood(no_gh));
}

static void test_small_drop_is_not_sad() {
  PetInputs in = daytime();
  in.stocks_ok = true;
  in.stock_avg_pct = -1.9f;
  TEST_ASSERT_EQUAL(PetMood::Normal, pet_mood(in));
}

static void test_lines() {
  char buf[96];
  PetInputs in = daytime();
  in.github_ok = true;
  in.github_today = 3;
  pet_line(PetMood::Happy, in, buf, sizeof(buf));
  TEST_ASSERT_NOT_NULL(strstr(buf, "3"));

  in.stock_avg_pct = -2.34f;
  pet_line(PetMood::Sad, in, buf, sizeof(buf));
  TEST_ASSERT_NOT_NULL(strstr(buf, "2.3%"));
}

static void test_sprite_is_symmetric_and_in_bounds() {
  PetMood moods[] = {PetMood::Normal, PetMood::Happy, PetMood::Sad, PetMood::Bored, PetMood::Sleep};
  for (PetMood m : moods) {
    for (int y = 0; y < kPetSize; y++) {
      for (int x = 0; x < kPetSize; x++) {
        TEST_ASSERT_EQUAL(pet_pixel(m, false, x, y), pet_pixel(m, false, kPetSize - 1 - x, y));
      }
    }
  }
  TEST_ASSERT_EQUAL(PET_NONE, pet_pixel(PetMood::Normal, false, -1, 0));
  TEST_ASSERT_EQUAL(PET_NONE, pet_pixel(PetMood::Normal, false, 0, kPetSize));
}

static int diff(PetMood a, bool blink_a, PetMood b, bool blink_b) {
  int n = 0;
  for (int y = 0; y < kPetSize; y++) {
    for (int x = 0; x < kPetSize; x++) n += pet_pixel(a, blink_a, x, y) != pet_pixel(b, blink_b, x, y);
  }
  return n;
}

static void test_frames_differ() {
  TEST_ASSERT_TRUE(diff(PetMood::Normal, false, PetMood::Normal, true) > 0);  // 眨眼
  TEST_ASSERT_TRUE(diff(PetMood::Normal, false, PetMood::Happy, false) > 0);
  TEST_ASSERT_TRUE(diff(PetMood::Normal, false, PetMood::Sad, false) > 0);
  TEST_ASSERT_TRUE(diff(PetMood::Normal, false, PetMood::Bored, false) > 0);
  TEST_ASSERT_EQUAL(0, diff(PetMood::Sleep, false, PetMood::Sleep, true));  // 睡着了不眨眼
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_mood_priority);
  RUN_TEST(test_bored_only_in_afternoon_without_commits);
  RUN_TEST(test_small_drop_is_not_sad);
  RUN_TEST(test_lines);
  RUN_TEST(test_sprite_is_symmetric_and_in_bounds);
  RUN_TEST(test_frames_differ);
  return UNITY_END();
}
