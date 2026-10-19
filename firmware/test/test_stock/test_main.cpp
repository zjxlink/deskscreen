// 自选股代码解析和行情解析的单元测试，在电脑上运行：pio test -e native
#include <ArduinoJson.h>
#include <string.h>
#include <unity.h>

#include "stock.h"

void setUp() {}
void tearDown() {}

static void check_secids(std::initializer_list<const char *> input, const char *expected) {
  StockCode codes[8];
  int n = 0;
  for (const char *c : input) strcpy(codes[n++], c);
  char buf[256];
  TEST_ASSERT_TRUE(stock_build_secids(codes, n, buf, sizeof(buf)));
  TEST_ASSERT_EQUAL_STRING(expected, buf);
}

static void test_split_codes() {
  StockCode codes[kMaxStocks];
  // 英文逗号、中文逗号、顿号、空格混用
  int n = stock_split_codes(" 600519, 000001\xEF\xBC\x8C" "00700\xE3\x80\x81" "AAPL  sh000001 ", codes, kMaxStocks);
  TEST_ASSERT_EQUAL(5, n);
  TEST_ASSERT_EQUAL_STRING("600519", codes[0]);
  TEST_ASSERT_EQUAL_STRING("000001", codes[1]);
  TEST_ASSERT_EQUAL_STRING("00700", codes[2]);
  TEST_ASSERT_EQUAL_STRING("AAPL", codes[3]);
  TEST_ASSERT_EQUAL_STRING("sh000001", codes[4]);

  TEST_ASSERT_EQUAL(kMaxStocks, stock_split_codes("1,2,3,4,5,6,7,8", codes, kMaxStocks));
  TEST_ASSERT_EQUAL(0, stock_split_codes(" , ", codes, kMaxStocks));
  // 太长的代码直接跳过
  TEST_ASSERT_EQUAL(1, stock_split_codes("ABCDEFGHIJKLMNOP 600519", codes, kMaxStocks));
  TEST_ASSERT_EQUAL_STRING("600519", codes[0]);
}

static void test_secids() {
  check_secids({"600519", "000001", "sh000001", "300750"}, "1.600519,0.000001,1.000001,0.300750");
  check_secids({"510300", "159915", "bj430047", "920001", "900901"},
               "1.510300,0.159915,0.430047,0.920001,1.900901");
  check_secids({"00700", "hk700", "HK09988"}, "116.00700,116.00700,116.09988");
  check_secids({"aapl"}, "105.AAPL,106.AAPL,107.AAPL");
  check_secids({"brk.b"}, "105.BRK.B,106.BRK.B,107.BRK.B");
  // SHOP 是美股，不能被当成 sh 前缀
  check_secids({"SHOP"}, "105.SHOP,106.SHOP,107.SHOP");
  // 格式不对的代码不参与请求
  check_secids({"12a$", "1234567", "600519"}, "1.600519");

  StockCode codes[1] = {"AAPL"};
  char small[10];
  TEST_ASSERT_FALSE(stock_build_secids(codes, 1, small, sizeof(small)));
}

static const char *kResponse =
    "{\"rc\":0,\"rt\":11,\"svr\":1,\"lt\":1,\"full\":1,\"dlmkts\":\"\",\"data\":{\"total\":3,\"diff\":["
    "{\"f2\":1488.0,\"f3\":-1.23,\"f4\":-18.5,\"f12\":\"600519\",\"f13\":1,\"f14\":\"贵州茅台\"},"
    "{\"f2\":\"-\",\"f3\":\"-\",\"f4\":\"-\",\"f12\":\"000001\",\"f13\":0,\"f14\":\"平安银行\"},"
    "{\"f2\":228.52,\"f3\":0.85,\"f4\":1.93,\"f12\":\"AAPL\",\"f13\":105,\"f14\":\"苹果\"}]}}";

static void test_parse_keeps_user_order() {
  JsonDocument doc;
  TEST_ASSERT_FALSE(deserializeJson(doc, kResponse));
  StockCode codes[] = {"aapl", "600519", "000001", "XYZ1"};
  StockData d{};
  TEST_ASSERT_EQUAL(StockStatus::Ok, stock_parse(doc, codes, 4, d));
  TEST_ASSERT_EQUAL(4, d.count);

  TEST_ASSERT_EQUAL_STRING("苹果", d.quotes[0].name);
  TEST_ASSERT_EQUAL_STRING("AAPL", d.quotes[0].code);
  TEST_ASSERT_TRUE(d.quotes[0].valid);
  TEST_ASSERT_FLOAT_WITHIN(0.001, 228.52, d.quotes[0].price);

  TEST_ASSERT_EQUAL_STRING("贵州茅台", d.quotes[1].name);
  TEST_ASSERT_FLOAT_WITHIN(0.001, -1.23, d.quotes[1].change_pct);
  TEST_ASSERT_FLOAT_WITHIN(0.001, -18.5, d.quotes[1].change);

  // 停牌：有名字，没有价格
  TEST_ASSERT_EQUAL_STRING("平安银行", d.quotes[2].name);
  TEST_ASSERT_FALSE(d.quotes[2].valid);

  // 不存在的代码：显示用户填的原文
  TEST_ASSERT_EQUAL_STRING("XYZ1", d.quotes[3].name);
  TEST_ASSERT_FALSE(d.quotes[3].valid);
}

static void test_parse_object_diff_and_errors() {
  JsonDocument doc;
  deserializeJson(doc, "{\"rc\":0,\"data\":{\"total\":1,\"diff\":{\"0\":{\"f2\":3.456,\"f3\":0,\"f4\":0,"
                       "\"f12\":\"510300\",\"f13\":1,\"f14\":\"沪深300ETF\"}}}}");
  StockCode codes[] = {"510300"};
  StockData d{};
  TEST_ASSERT_EQUAL(StockStatus::Ok, stock_parse(doc, codes, 1, d));
  TEST_ASSERT_TRUE(d.quotes[0].valid);
  TEST_ASSERT_FLOAT_WITHIN(0.0001, 3.456, d.quotes[0].price);

  // 全部代码都不存在时 data 为 null，仍然算成功，每一项都标记无效
  deserializeJson(doc, "{\"rc\":0,\"data\":null}");
  TEST_ASSERT_EQUAL(StockStatus::Ok, stock_parse(doc, codes, 1, d));
  TEST_ASSERT_FALSE(d.quotes[0].valid);

  deserializeJson(doc, "{\"rc\":102,\"data\":null}");
  TEST_ASSERT_EQUAL(StockStatus::Error, stock_parse(doc, codes, 1, d));
  deserializeJson(doc, "{}");
  TEST_ASSERT_EQUAL(StockStatus::Error, stock_parse(doc, codes, 1, d));
}

static void test_long_name_cut_on_char_boundary() {
  JsonDocument doc;
  // 12 个汉字共 36 字节，name 最多放 31 字节，应截成 10 个汉字（30 字节）
  deserializeJson(doc, "{\"rc\":0,\"data\":{\"diff\":[{\"f2\":1,\"f3\":0,\"f4\":0,\"f12\":\"600000\",\"f13\":1,"
                       "\"f14\":\"一二三四五六七八九十甲乙\"}]}}");
  StockCode codes[] = {"600000"};
  StockData d{};
  stock_parse(doc, codes, 1, d);
  TEST_ASSERT_EQUAL(30, strlen(d.quotes[0].name));
  TEST_ASSERT_EQUAL_STRING("一二三四五六七八九十", d.quotes[0].name);
}

static uint32_t refresh_at(int wday, int hour, int minute) {
  struct tm t{};
  t.tm_wday = wday;
  t.tm_hour = hour;
  t.tm_min = minute;
  return stock_refresh_ms(t);
}

static void test_refresh_interval() {
  const uint32_t fast = 30 * 1000, slow = 10 * 60 * 1000;
  TEST_ASSERT_EQUAL(fast, refresh_at(1, 10, 0));   // 周一上午 A 股
  TEST_ASSERT_EQUAL(fast, refresh_at(3, 15, 50));  // 港股收盘前
  TEST_ASSERT_EQUAL(slow, refresh_at(1, 17, 0));   // 收盘后
  TEST_ASSERT_EQUAL(fast, refresh_at(1, 22, 0));   // 周一晚上美股
  TEST_ASSERT_EQUAL(fast, refresh_at(2, 3, 0));    // 周二凌晨，美股周一的交易
  TEST_ASSERT_EQUAL(slow, refresh_at(1, 3, 0));    // 周一凌晨：周日没有美股交易
  TEST_ASSERT_EQUAL(fast, refresh_at(6, 3, 0));    // 周六凌晨，美股周五的交易
  TEST_ASSERT_EQUAL(slow, refresh_at(6, 10, 0));   // 周六白天
  TEST_ASSERT_EQUAL(slow, refresh_at(0, 22, 0));   // 周日晚上
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_split_codes);
  RUN_TEST(test_secids);
  RUN_TEST(test_parse_keeps_user_order);
  RUN_TEST(test_parse_object_diff_and_errors);
  RUN_TEST(test_long_name_cut_on_char_boundary);
  RUN_TEST(test_refresh_interval);
  return UNITY_END();
}
