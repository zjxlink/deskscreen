// 自选股代码解析和行情解析的单元测试，在电脑上运行：pio test -e native
#include <ArduinoJson.h>
#include <string.h>
#include <unity.h>

#include <string>

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

static void check_redirect(const char *location, const char *scheme, const char *expected) {
  const char *prefix = stock_redirect_prefix(location, scheme);
  if (!expected) {
    TEST_ASSERT_NULL_MESSAGE(prefix, location);
    return;
  }
  TEST_ASSERT_NOT_NULL_MESSAGE(prefix, location);
  std::string url = std::string(prefix) + location;
  TEST_ASSERT_EQUAL_STRING(expected, url.c_str());
}

static void test_redirect_prefix() {
  // 实际遇到过的：不带协议的 Location，沿用当前请求的协议
  check_redirect("//push2delay.eastmoney.com/api/qt/ulist.np/get?secids=1.600519", "http",
                 "http://push2delay.eastmoney.com/api/qt/ulist.np/get?secids=1.600519");
  check_redirect("//push2delay.eastmoney.com/api", "https", "https://push2delay.eastmoney.com/api");
  check_redirect("http://82.push2.eastmoney.com/api/x", "http", "http://82.push2.eastmoney.com/api/x");
  check_redirect("HTTPS://PUSH2DELAY.EASTMONEY.COM:443/api", "https", "HTTPS://PUSH2DELAY.EASTMONEY.COM:443/api");
  check_redirect("//eastmoney.com/api", "http", "http://eastmoney.com/api");
  check_redirect("/api/qt/ulist.np/get", "http", "http://push2.eastmoney.com/api/qt/ulist.np/get");
  check_redirect("/api/qt/ulist.np/get", "https", "https://push2.eastmoney.com/api/qt/ulist.np/get");

  // 换协议的不跟随：连接对象是按协议建的
  check_redirect("https://push2delay.eastmoney.com/api", "http", nullptr);
  check_redirect("http://push2delay.eastmoney.com/api", "https", nullptr);

  // 不是东方财富的地址，都不跟随
  check_redirect("//example.com/eastmoney.com", "http", nullptr);
  check_redirect("http://eastmoney.com.example.com/", "http", nullptr);
  check_redirect("http://fakeeastmoney.com/", "http", nullptr);
  check_redirect("//evil.com@push2.eastmoney.com/", "http", nullptr);
  check_redirect("api/relative", "http", nullptr);
  check_redirect("", "http", nullptr);
  check_redirect("//push2.eastmoney.com/", "ftp", nullptr);
}

static void check_tencent_query(std::initializer_list<const char *> input, const char *expected) {
  StockCode codes[8];
  int n = 0;
  for (const char *c : input) strcpy(codes[n++], c);
  char buf[256];
  TEST_ASSERT_TRUE(stock_build_tencent_query(codes, n, buf, sizeof(buf)));
  TEST_ASSERT_EQUAL_STRING(expected, buf);
}

static void test_tencent_query() {
  check_tencent_query({"600519", "000001", "sh000001", "300750"}, "sh600519,sz000001,sh000001,sz300750");
  // 北交所在东方财富里和深市同为 0，腾讯要写 bj
  check_tencent_query({"bj430047", "920001", "430047", "830799", "900901"},
                      "bj430047,bj920001,bj430047,bj830799,sh900901");
  check_tencent_query({"00700", "hk700", "HK09988"}, "hk00700,hk00700,hk09988");
  check_tencent_query({"aapl", "brk.b", "SHOP"}, "usAAPL,usBRK.B,usSHOP");
  check_tencent_query({"12a$", "1234567", "510300"}, "sh510300");

  StockCode codes[1] = {"AAPL"};
  char small[6];
  TEST_ASSERT_FALSE(stock_build_tencent_query(codes, 1, small, sizeof(small)));
}

// 实际返回的格式（字段有删减，但 1/3/31/32 的位置不变），每行以 ";\n 结尾
// 字段 6～28 是成交量、买卖五档等，这里用 0 占位，29 为空
#define TENCENT_PAD "~0~0~0~0~0~0~0~0~0~0~0~0~0~0~0~0~0~0~0~0~0~0~0~"
static const char *kTencentResponse =
    "v_sh600519=\"1~贵州茅台~600519~1324.97~1329.00~1322.50" TENCENT_PAD "~20261019161457~-4.03~-0.30~1348.60~\";\n"
    "v_hk00700=\"100~腾讯控股~00700~417.800~417.800~424.000" TENCENT_PAD "~2026/10/16 16:08:22~0.000~0.00~428.800~\";\n"
    "v_usBRK.B=\"200~伯克希尔B~BRK.B.N~531.68~532.02~530.00" TENCENT_PAD "~2026-10-19 10:10:58~-0.34~-0.06~532.24~\";\n"
    "v_sz000001=\"51~平安银行~000001~0.00~11.50~0.00" TENCENT_PAD "~20261019161500~0.00~0.00~0.00~\";\n";

static void test_parse_tencent() {
  StockCode codes[] = {"brk.b", "600519", "700", "000001", "sh999999"};
  StockData d{};
  TEST_ASSERT_EQUAL(StockStatus::Ok, stock_parse_tencent(kTencentResponse, codes, 5, d));
  TEST_ASSERT_EQUAL(5, d.count);

  // 按用户填写的顺序，代码去掉市场前缀
  TEST_ASSERT_EQUAL_STRING("伯克希尔B", d.quotes[0].name);
  TEST_ASSERT_EQUAL_STRING("BRK.B", d.quotes[0].code);
  TEST_ASSERT_TRUE(d.quotes[0].valid);
  TEST_ASSERT_FLOAT_WITHIN(0.001, 531.68, d.quotes[0].price);
  TEST_ASSERT_FLOAT_WITHIN(0.001, -0.34, d.quotes[0].change);
  TEST_ASSERT_FLOAT_WITHIN(0.001, -0.06, d.quotes[0].change_pct);

  TEST_ASSERT_EQUAL_STRING("贵州茅台", d.quotes[1].name);
  TEST_ASSERT_EQUAL_STRING("600519", d.quotes[1].code);
  TEST_ASSERT_FLOAT_WITHIN(0.001, 1324.97, d.quotes[1].price);
  TEST_ASSERT_FLOAT_WITHIN(0.001, -4.03, d.quotes[1].change);
  TEST_ASSERT_FLOAT_WITHIN(0.001, -0.30, d.quotes[1].change_pct);

  TEST_ASSERT_EQUAL_STRING("腾讯控股", d.quotes[2].name);
  TEST_ASSERT_EQUAL_STRING("00700", d.quotes[2].code);
  TEST_ASSERT_TRUE(d.quotes[2].valid);
  TEST_ASSERT_FLOAT_WITHIN(0.001, 417.8, d.quotes[2].price);

  // 停牌：现价为 0
  TEST_ASSERT_EQUAL_STRING("平安银行", d.quotes[3].name);
  TEST_ASSERT_FALSE(d.quotes[3].valid);

  // 不存在的代码腾讯不返回，显示用户填的原文
  TEST_ASSERT_EQUAL_STRING("sh999999", d.quotes[4].name);
  TEST_ASSERT_FALSE(d.quotes[4].valid);
}

static void test_parse_tencent_edge_cases() {
  StockCode codes[] = {"600519"};
  StockData d{};
  // 全部代码都不存在时的实际返回
  TEST_ASSERT_EQUAL(StockStatus::Ok, stock_parse_tencent("v_pv_none_match=\"1\";\n", codes, 1, d));
  TEST_ASSERT_FALSE(d.quotes[0].valid);
  TEST_ASSERT_EQUAL_STRING("600519", d.quotes[0].name);

  // 错误页、空响应
  TEST_ASSERT_EQUAL(StockStatus::Error, stock_parse_tencent("<html>502 Bad Gateway</html>", codes, 1, d));
  TEST_ASSERT_EQUAL(StockStatus::Error, stock_parse_tencent("", codes, 1, d));

  // 被截断的行：缺少后面的字段时涨跌按 0 处理，不越界
  TEST_ASSERT_EQUAL(StockStatus::Ok, stock_parse_tencent("v_sh600519=\"1~贵州茅台~600519~1324.97", codes, 1, d));
  TEST_ASSERT_EQUAL_STRING("贵州茅台", d.quotes[0].name);
  TEST_ASSERT_FLOAT_WITHIN(0.001, 1324.97, d.quotes[0].price);
  TEST_ASSERT_FLOAT_WITHIN(0.001, 0, d.quotes[0].change);

  // 只在行首匹配：别的行字段里出现 v_sh600519=" 不能算
  TEST_ASSERT_EQUAL(StockStatus::Ok,
                    stock_parse_tencent("v_sz000001=\"51~x v_sh600519=\"~000001~1\";\n", codes, 1, d));
  TEST_ASSERT_FALSE(d.quotes[0].valid);

  // 名字过长时在字符边界截断
  TEST_ASSERT_EQUAL(StockStatus::Ok,
                    stock_parse_tencent("v_sh600519=\"1~一二三四五六七八九十甲乙~600519~1\";\n", codes, 1, d));
  TEST_ASSERT_EQUAL_STRING("一二三四五六七八九十", d.quotes[0].name);
}

static void test_primary_cooldown() {
  const uint32_t min = 60 * 1000;
  TEST_ASSERT_EQUAL(5 * min, stock_primary_cooldown_ms(1));
  TEST_ASSERT_EQUAL(10 * min, stock_primary_cooldown_ms(2));
  TEST_ASSERT_EQUAL(20 * min, stock_primary_cooldown_ms(3));
  TEST_ASSERT_EQUAL(30 * min, stock_primary_cooldown_ms(4));
  TEST_ASSERT_EQUAL(30 * min, stock_primary_cooldown_ms(100));
}

static uint32_t refresh_at(int wday, int hour, int minute) {
  struct tm t{};
  t.tm_wday = wday;
  t.tm_hour = hour;
  t.tm_min = minute;
  return stock_refresh_ms(t);
}

static void test_refresh_interval() {
  const uint32_t fast = 60 * 1000, slow = 10 * 60 * 1000;
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
  RUN_TEST(test_redirect_prefix);
  RUN_TEST(test_tencent_query);
  RUN_TEST(test_parse_tencent);
  RUN_TEST(test_parse_tencent_edge_cases);
  RUN_TEST(test_primary_cooldown);
  RUN_TEST(test_refresh_interval);
  return UNITY_END();
}
