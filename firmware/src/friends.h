#pragma once

#include <ArduinoJson.h>
#include <stddef.h>
#include <stdint.h>

#include <string>

// 好友圈子：几台桌面屏（和手机网页）组成一个「圈子」，互相戳一戳、发留言（可以带照片）、看谁在桌前。
// 板子之间连不上，消息都经过 NAS 上的中转服务（server/friends-relay）。后台联网任务每隔几秒同步一次：
// 好友页显示着、有弹窗时 3 秒，平时 6 秒，夜里、聊天、看片子时 30 秒。
// 设置网页里填服务器地址，板子第一次连上时自己注册；建圈子、邀请码、加入、退出在中转服务的网页上做
// （设置网页上的「管理圈子」直接打开它），板子的好友页上也能生成邀请码。

static constexpr int kFrMaxCircles = 4;  // 和服务器的限制一样
static constexpr int kFrMaxMembers = 8;
static constexpr int kFrIdLen = 20;
static constexpr int kFrNameLen = 32;    // 名字最多 10 个字（UTF-8 每个汉字 3 字节）
static constexpr int kFrTextLen = 244;   // 留言最多 60 个字
static constexpr int kFrMaxEvents = 16;  // 一次同步最多处理几条
static constexpr int kFrPhraseCount = 6;
static constexpr int kFrThumbW = 144;    // 留言里照片的缩略图
static constexpr int kFrThumbH = 108;

// 板子上没有键盘，发留言用这几句
extern const char *const kFrPhrases[kFrPhraseCount];

struct FrMember {
  char id[kFrIdLen];
  char name[kFrNameLen];
  bool online;  // 最近 90 秒同步过
  bool here;    // 在桌前（对方打开了分享，雷达感应到人）
  bool me;
  bool board;   // 是桌面屏，不是手机网页
};

struct FrCircle {
  char id[kFrIdLen];
  char name[kFrNameLen];
  char owner[kFrIdLen];
  int n;
  FrMember m[kFrMaxMembers];
};

enum class FrEventType : uint8_t {
  Poke,    // 戳了一下
  Text,    // 留言
  Read,    // 我发的留言对方看过了
  Circle,  // 圈子成员变了（加入、退出、被移出）
  Other,   // 不认识的（新版服务器加的），忽略
};

struct FrEvent {
  uint32_t seq;
  FrEventType type;
  bool mine;       // 我自己发的（服务器在我的收件箱里也放一份）
  bool to_me;      // 只发给我的，不是发到整个圈子
  char id[kFrIdLen];
  char circle[kFrIdLen];
  char circle_name[kFrNameLen];
  char from[kFrIdLen];
  char from_name[kFrNameLen];
  char text[kFrTextLen];
  char photo[48];  // 照片的路径（/p/xxx.jpg），没有就是空字符串
  int64_t ts;
  int8_t thumb;    // 缩略图在第几格（friends_thumb），-1 表示没有照片或者没下载下来
  uint32_t thumb_gen;
};

struct FrSync {
  uint32_t seq;
  char me_id[kFrIdLen];
  char me_name[kFrNameLen];
  int ncircles;
  FrCircle circles[kFrMaxCircles];
  int nevents;
  FrEvent events[kFrMaxEvents];
};

// 板子的状态，给界面和设置网页用
struct FrStatus {
  bool configured;   // 填了服务器地址
  bool registered;   // 已经在服务器上注册
  bool synced;       // 最近一次同步成功
  char error[96];    // 最近一次失败的原因，成功时为空
  char me_name[kFrNameLen];
  int ncircles;
  FrCircle circles[kFrMaxCircles];
  char invite_code[8];   // 最近一次生成的邀请码，过期后清空
  char invite_circle[kFrNameLen];
  uint32_t invite_until;  // millis()
  char note[96];         // 最近一次操作的结果（「戳了 小雨」「发送失败：…」），显示在好友页底部
  uint32_t note_seq;     // 每次操作完加一
};

// 联网同步的快慢
enum class FrPace : uint8_t { Fast, Normal, Slow };

// 启动后台同步（挂在共用的联网任务上）
void friends_begin();
// 设置改了（服务器地址、名字）：马上同步一轮
void friends_kick();
void friends_set_pace(FrPace pace);
// 要不要告诉圈子「我在桌前」：主循环按设置和雷达算好传进来
void friends_set_here(bool here);

// 状态有变化时返回 true 并写入 out（out 很大，放在 PSRAM 或静态变量里）
bool friends_take_status(FrStatus &out);
// 收到的新消息（戳一戳、留言、已读、成员变化），返回条数。只在主循环里调用
int friends_take_events(FrEvent *out, int max);
// 留言照片的缩略图（kFrThumbW x kFrThumbH，RGB565），已经被后来的照片占用时返回 nullptr
const uint16_t *friends_thumb(int slot, uint32_t gen);

// 发出去：排队，联网任务马上发。to 为空表示发给圈子里所有人
void friends_poke(const char *circle, const char *to);
void friends_send_text(const char *circle, const char *to, const char *text);
void friends_mark_read(const char *msg_id);
void friends_invite(const char *circle);

// 设置网页的一句话状态：「● 已连接 · 2 个圈子」「连不上：…」「未开启」
std::string friends_brief();
// 设置网页用：服务器地址（去掉结尾的 /）和板子的 token，「管理圈子」按钮拼成 地址/#t=token
std::string friends_manage_url();

// 以下在 friends_parse.cpp 中实现，不依赖网络，可在电脑上测试

// 解析 /api/sync 的响应。圈子、成员、消息超过上限的丢掉；格式不对返回 false
bool friends_parse_sync(JsonDocument &doc, FrSync &out);

// 时钟页右上角的一行：「小雨在桌前」「小雨、妈妈在桌前」「3 人在桌前」，没人时写空字符串。
// 几个圈子里的同一个人只算一次，不算自己
void friends_presence_text(const FrCircle *circles, int n, char *out, size_t size);

// 好几个人接连戳你时合成一句：names 里已有的名字不重复加，超过 3 个写「等 N 人」。count 是一共几个人
void friends_add_name(char *names, size_t size, int &count, const char *name);
void friends_names_text(const char *names, int count, char *out, size_t size);

// 服务器地址：去掉首尾空白和结尾的 /，只接受 http:// 或 https:// 开头的，不对时写空字符串
void friends_clean_url(const char *in, char *out, size_t size);

// 按 UTF-8 字符截断复制，不会切坏汉字
void friends_copy(char *out, size_t size, const char *in);
