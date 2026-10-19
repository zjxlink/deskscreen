// OTA 版本比较、manifest 解析、固件格式检查的单元测试，在电脑上运行：pio test -e native
#include <ArduinoJson.h>
#include <string.h>
#include <unity.h>

#include "ota_util.h"

void setUp() {}
void tearDown() {}

static void test_parse_version() {
  int a, b, c;
  TEST_ASSERT_TRUE(ota_parse_version("v0.5.0", a, b, c));
  TEST_ASSERT_EQUAL(0, a);
  TEST_ASSERT_EQUAL(5, b);
  TEST_ASSERT_EQUAL(0, c);
  TEST_ASSERT_TRUE(ota_parse_version("12.34.567", a, b, c));
  TEST_ASSERT_EQUAL(567, c);
  TEST_ASSERT_FALSE(ota_parse_version("dev", a, b, c));
  TEST_ASSERT_FALSE(ota_parse_version("0d02060", a, b, c));          // 提交号
  TEST_ASSERT_FALSE(ota_parse_version("v1.2", a, b, c));
  TEST_ASSERT_FALSE(ota_parse_version("v1.2.3-rc1", a, b, c));       // 预发布版本
  TEST_ASSERT_FALSE(ota_parse_version("v1.2.3-4-gabcdef", a, b, c)); // git describe 的开发版
  TEST_ASSERT_FALSE(ota_parse_version("v1..3", a, b, c));
  TEST_ASSERT_FALSE(ota_parse_version("", a, b, c));
  TEST_ASSERT_FALSE(ota_parse_version(nullptr, a, b, c));
}

static void test_is_newer() {
  TEST_ASSERT_TRUE(ota_is_newer("v0.4.0", "v0.5.0"));
  TEST_ASSERT_TRUE(ota_is_newer("v0.4.9", "v0.4.10"));  // 按数字比较，不是按字符串
  TEST_ASSERT_TRUE(ota_is_newer("v0.9.9", "v1.0.0"));
  TEST_ASSERT_FALSE(ota_is_newer("v0.5.0", "v0.5.0"));
  TEST_ASSERT_FALSE(ota_is_newer("v0.5.1", "v0.5.0"));  // 不降级
  TEST_ASSERT_FALSE(ota_is_newer("dev", "v9.9.9"));     // 开发版不自动升级
  TEST_ASSERT_FALSE(ota_is_newer("v0.4.0", "garbage"));
}

static const char *kManifest =
    "{\"version\":\"v0.5.0\",\"boards\":{"
    "\"box3\":{\"url\":\"https://github.com/o/r/releases/download/v0.5.0/firmware-box3.bin\","
    "\"md5\":\"0123456789abcdef0123456789abcdef\",\"size\":2015632},"
    "\"desk_v1\":{\"url\":\"https://github.com/o/r/releases/download/v0.5.0/firmware-desk_v1.bin\","
    "\"md5\":\"fedcba9876543210fedcba9876543210\",\"size\":1987000}}}";

static void test_parse_manifest() {
  JsonDocument doc;
  TEST_ASSERT_FALSE(deserializeJson(doc, kManifest));
  OtaManifest m{};
  TEST_ASSERT_TRUE(ota_parse_manifest(doc, "box3", m));
  TEST_ASSERT_EQUAL_STRING("v0.5.0", m.version);
  TEST_ASSERT_EQUAL_STRING("https://github.com/o/r/releases/download/v0.5.0/firmware-box3.bin", m.url);
  TEST_ASSERT_EQUAL_STRING("0123456789abcdef0123456789abcdef", m.md5);
  TEST_ASSERT_EQUAL_UINT32(2015632, m.size);

  TEST_ASSERT_TRUE(ota_parse_manifest(doc, "desk_v1", m));
  TEST_ASSERT_EQUAL_UINT32(1987000, m.size);
  TEST_ASSERT_FALSE(ota_parse_manifest(doc, "other_board", m));
}

static void check_bad_manifest(const char *json) {
  JsonDocument doc;
  deserializeJson(doc, json);
  OtaManifest m{};
  TEST_ASSERT_FALSE_MESSAGE(ota_parse_manifest(doc, "box3", m), json);
}

static void test_reject_bad_manifest() {
  // 不是 https
  check_bad_manifest("{\"version\":\"v1.0.0\",\"boards\":{\"box3\":{\"url\":\"http://x/f.bin\","
                     "\"md5\":\"0123456789abcdef0123456789abcdef\",\"size\":1}}}");
  // MD5 不对（长度、大写、非十六进制）
  check_bad_manifest("{\"version\":\"v1.0.0\",\"boards\":{\"box3\":{\"url\":\"https://x/f.bin\","
                     "\"md5\":\"0123\",\"size\":1}}}");
  check_bad_manifest("{\"version\":\"v1.0.0\",\"boards\":{\"box3\":{\"url\":\"https://x/f.bin\","
                     "\"md5\":\"0123456789ABCDEF0123456789ABCDEF\",\"size\":1}}}");
  check_bad_manifest("{\"version\":\"v1.0.0\",\"boards\":{\"box3\":{\"url\":\"https://x/f.bin\","
                     "\"md5\":\"0123456789abcdef0123456789abcdeg\",\"size\":1}}}");
  // 大小为 0、版本号不对、缺字段
  check_bad_manifest("{\"version\":\"v1.0.0\",\"boards\":{\"box3\":{\"url\":\"https://x/f.bin\","
                     "\"md5\":\"0123456789abcdef0123456789abcdef\",\"size\":0}}}");
  check_bad_manifest("{\"version\":\"latest\",\"boards\":{\"box3\":{\"url\":\"https://x/f.bin\","
                     "\"md5\":\"0123456789abcdef0123456789abcdef\",\"size\":1}}}");
  check_bad_manifest("{\"version\":\"v1.0.0\"}");
  check_bad_manifest("{}");
}

static void test_app_image_check() {
  uint8_t app[64] = {0};
  app[0] = 0xE9;
  app[32] = 0x32;
  app[33] = 0x54;
  app[34] = 0xCD;
  app[35] = 0xAB;
  TEST_ASSERT_TRUE(ota_looks_like_app_image(app, sizeof(app)));

  // 单文件固件开头是 bootloader：也以 0xE9 开头，但没有应用描述信息
  uint8_t boot[64] = {0};
  boot[0] = 0xE9;
  TEST_ASSERT_FALSE(ota_looks_like_app_image(boot, sizeof(boot)));

  uint8_t other[64] = {'P', 'K', 3, 4};  // zip 之类的其他文件
  TEST_ASSERT_FALSE(ota_looks_like_app_image(other, sizeof(other)));
  TEST_ASSERT_FALSE(ota_looks_like_app_image(app, 35));  // 太短
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_parse_version);
  RUN_TEST(test_is_newer);
  RUN_TEST(test_parse_manifest);
  RUN_TEST(test_reject_bad_manifest);
  RUN_TEST(test_app_image_check);
  return UNITY_END();
}
