// 好友圈子的单元测试：在电脑上运行：pio test -e native
#include <string.h>
#include <unity.h>

#include "friends.h"

void setUp() {}
void tearDown() {}

static FrSync sync;

// 中转服务 /api/sync 的真实响应格式（server/friends-relay/relay.py）
static const char kSync[] = R"json({"seq": 12, "me": {"id": "ug7bhnzg", "name": "Jie 的桌面屏", "kind": "board"},
 "circles": [{"id": "c3f9a2", "name": "我们家", "owner": "ug7bhnzg", "members": [
   {"id": "ug7bhnzg", "name": "Jie 的桌面屏", "kind": "board", "online": true, "here": true, "me": true},
   {"id": "4rwgvq2p", "name": "小雨", "kind": "web", "online": true, "here": true, "me": false},
   {"id": "833xbpjd", "name": "妈妈", "kind": "board", "online": false, "here": true, "me": false}]}],
 "events": [
   {"type": "poke", "id": "p1", "circle": "c3f9a2", "circle_name": "我们家", "from": "4rwgvq2p", "from_name": "小雨",
    "to": "ug7bhnzg", "text": "", "ts": 1792569193, "seq": 10},
   {"type": "text", "id": "zc4x9y8mwv", "circle": "c3f9a2", "circle_name": "我们家", "from": "4rwgvq2p",
    "from_name": "小雨", "to": "", "text": "今天的晚霞好好看", "ts": 1792569200, "photo": "/p/w4msj5td2agps4gj.jpg", "seq": 11},
   {"type": "read", "id": "m1", "circle": "c3f9a2", "from": "4rwgvq2p", "from_name": "小雨", "ts": 1792569300, "seq": 12},
   {"type": "selfie", "seq": 13}]})json";

static void test_parse() {
  JsonDocument doc;
  TEST_ASSERT_FALSE(deserializeJson(doc, kSync));
  TEST_ASSERT_TRUE(friends_parse_sync(doc, sync));
  TEST_ASSERT_EQUAL(12, sync.seq);
  TEST_ASSERT_EQUAL_STRING("ug7bhnzg", sync.me_id);
  TEST_ASSERT_EQUAL_STRING("Jie 的桌面屏", sync.me_name);
  TEST_ASSERT_EQUAL(1, sync.ncircles);
  const FrCircle &c = sync.circles[0];
  TEST_ASSERT_EQUAL_STRING("我们家", c.name);
  TEST_ASSERT_EQUAL(3, c.n);
  TEST_ASSERT_TRUE(c.m[0].me);
  TEST_ASSERT_TRUE(c.m[0].board);
  TEST_ASSERT_FALSE(c.m[1].board);
  TEST_ASSERT_TRUE(c.m[1].here);
  TEST_ASSERT_FALSE(c.m[2].here);  // 离线就不算在桌前

  TEST_ASSERT_EQUAL(4, sync.nevents);
  TEST_ASSERT_EQUAL(FrEventType::Poke, sync.events[0].type);
  TEST_ASSERT_TRUE(sync.events[0].to_me);
  TEST_ASSERT_EQUAL(FrEventType::Text, sync.events[1].type);
  TEST_ASSERT_FALSE(sync.events[1].to_me);
  TEST_ASSERT_EQUAL_STRING("今天的晚霞好好看", sync.events[1].text);
  TEST_ASSERT_EQUAL_STRING("/p/w4msj5td2agps4gj.jpg", sync.events[1].photo);
  TEST_ASSERT_EQUAL(-1, sync.events[1].thumb);
  TEST_ASSERT_EQUAL(FrEventType::Read, sync.events[2].type);
  TEST_ASSERT_EQUAL(FrEventType::Other, sync.events[3].type);

  JsonDocument bad;
  deserializeJson(bad, "{\"error\":\"token 无效\"}");
  TEST_ASSERT_FALSE(friends_parse_sync(bad, sync));
}

static void test_parse_limits() {
  // 超过上限的圈子、成员丢掉；消息太多留最新的
  std::string json = "{\"seq\":99,\"circles\":[";
  for (int c = 0; c < kFrMaxCircles + 2; c++) {
    if (c) json += ",";
    json += "{\"id\":\"c" + std::to_string(c) + "\",\"name\":\"圈\",\"members\":[";
    for (int m = 0; m < kFrMaxMembers + 3; m++) {
      if (m) json += ",";
      json += "{\"id\":\"m" + std::to_string(m) + "\",\"name\":\"人\",\"online\":true,\"here\":true}";
    }
    json += "]}";
  }
  json += "],\"events\":[";
  for (int i = 0; i < kFrMaxEvents + 5; i++) {
    if (i) json += ",";
    json += "{\"type\":\"poke\",\"seq\":" + std::to_string(i + 1) + ",\"photo\":\"https://evil/x.jpg\"}";
  }
  json += "]}";
  JsonDocument doc;
  TEST_ASSERT_FALSE(deserializeJson(doc, json));
  TEST_ASSERT_TRUE(friends_parse_sync(doc, sync));
  TEST_ASSERT_EQUAL(kFrMaxCircles, sync.ncircles);
  TEST_ASSERT_EQUAL(kFrMaxMembers, sync.circles[0].n);
  TEST_ASSERT_EQUAL(kFrMaxEvents, sync.nevents);
  TEST_ASSERT_EQUAL(6, sync.events[0].seq);
  TEST_ASSERT_EQUAL(kFrMaxEvents + 5, sync.events[kFrMaxEvents - 1].seq);
  TEST_ASSERT_EQUAL_STRING("", sync.events[0].photo);  // 只认服务器自己的路径
}

static FrMember member(const char *id, const char *name, bool here, bool me = false) {
  FrMember m = {};
  strcpy(m.id, id);
  strcpy(m.name, name);
  m.online = true;
  m.here = here;
  m.me = me;
  return m;
}

static void test_presence() {
  FrCircle c[2] = {};
  c[0].n = 3;
  c[0].m[0] = member("me", "我", true, true);
  c[0].m[1] = member("a", "小雨", true);
  c[0].m[2] = member("b", "妈妈", false);
  char out[64];
  friends_presence_text(c, 1, out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("小雨在桌前", out);

  c[0].m[2].here = true;
  friends_presence_text(c, 1, out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("小雨、妈妈在桌前", out);

  // 另一个圈子里也有小雨：只算一次
  c[1].n = 2;
  c[1].m[0] = member("a", "小雨", true);
  c[1].m[1] = member("x", "老王", false);
  friends_presence_text(c, 2, out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("小雨、妈妈在桌前", out);
  c[1].m[1].here = true;
  friends_presence_text(c, 2, out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("3 人在桌前", out);

  c[0].m[1].here = c[0].m[2].here = c[1].m[0].here = c[1].m[1].here = false;
  friends_presence_text(c, 2, out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("", out);  // 自己在桌前不显示
}

static void test_names() {
  char names[96] = "";
  int count = 0;
  char out[96];
  friends_add_name(names, sizeof(names), count, "小雨");
  friends_add_name(names, sizeof(names), count, "小雨");
  TEST_ASSERT_EQUAL(1, count);
  friends_add_name(names, sizeof(names), count, "爸爸");
  friends_names_text(names, count, out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("小雨、爸爸", out);
  friends_add_name(names, sizeof(names), count, "妈妈");
  friends_add_name(names, sizeof(names), count, "老王");
  friends_names_text(names, count, out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("小雨、爸爸、妈妈等 4 人", out);
  friends_add_name(names, sizeof(names), count, "妈妈");
  TEST_ASSERT_EQUAL(4, count);
}

static void test_url_and_copy() {
  char out[64];
  friends_clean_url("  https://friends.zjxlink.cn/ ", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("https://friends.zjxlink.cn", out);
  friends_clean_url("http://192.168.0.10:18267", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("http://192.168.0.10:18267", out);
  friends_clean_url("friends.zjxlink.cn", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("", out);
  friends_clean_url("https://a b", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("", out);
  friends_clean_url("https://", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("", out);

  char small[8];
  friends_copy(small, sizeof(small), "我们家的");  // 12 字节，放不下时整字截断
  TEST_ASSERT_EQUAL_STRING("我们", small);
  friends_copy(small, sizeof(small), nullptr);
  TEST_ASSERT_EQUAL_STRING("", small);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_parse);
  RUN_TEST(test_parse_limits);
  RUN_TEST(test_presence);
  RUN_TEST(test_names);
  RUN_TEST(test_url_and_copy);
  return UNITY_END();
}
