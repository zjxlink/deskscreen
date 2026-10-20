#pragma once

#include <Arduino.h>

// 电子相册：两个图片来源，分开存放，在设置里各自开关，都打开时交替播放：
//   自己的照片：TF 卡（BOX-3 的 SENSOR 底座上）photos 文件夹
//   必应每日一图：TF 卡 bing 文件夹，由 bing.cpp 下载；没插卡时只有当天这一张，放在 PSRAM
// 解码在后台任务里做，手机原图也不会卡住界面；解码好的一帧是 320x240 的 RGB565，放在 PSRAM。
// 网页上传、删除照片和必应下载也走这里，和后台任务用同一把锁，不会同时读写卡

enum class PhotoCard {
  NoSlot,   // 这块板子没有 TF 卡槽
  NoCard,   // 没插卡、卡坏了或者不是 FAT32
  Empty,    // 有卡，photos 文件夹里没有照片
  Ok,       // 有卡，photos 文件夹里有照片
};

enum class PhotoSource : uint8_t { Local = 0, Bing = 1 };

struct PhotoStatus {
  PhotoCard card;   // TF 卡和自己的照片
  int local;        // 自己的照片张数
  int bing;         // 必应图片张数（没插卡时最多 1 张，在内存里）
  int count;        // 按设置里打开的来源，能放的张数
  int failed;       // 最近一轮读不出来的张数（HEIC、渐进式 JPEG 等）
};

// 开机时调用：给卡上电，尝试挂载，启动后台解码任务
void photos_begin();

bool photos_supported();
PhotoStatus photos_status();

// 设置里打开了哪些来源
void photos_set_sources(bool local, bool bing);

// 请后台解码下一张（随机或按文件名顺序）。还在解码上一张、或者上一张还没取走时忽略
void photos_request_next(bool shuffle);

// 后台正在读卡、解码
bool photos_busy();

// 有新解码好的一帧时返回 true。px 一直有效，直到下一次返回 true 后又过了一帧；
// caption 是必应图片的说明（自己的照片为空字符串），和 px 一样有效
bool photos_take_frame(const uint16_t *&px, const char *&caption);

// 网页设置用：照片列表（JSON：卡的状态、空间，自己的照片和必应图片各自的文件名、大小）
String photos_list_json();

// 网页上传：先写到临时文件，结束时改成正式文件名（重名时自动加序号）
bool photos_upload_begin(const String &filename);
bool photos_upload_write(const uint8_t *data, size_t len);
bool photos_upload_end(String &saved_name);
void photos_upload_abort();

// 删除一张照片，name 只能是对应文件夹里的文件名。删掉的必应图片不会再下载回来
bool photos_delete(PhotoSource src, const String &name);

// 把一张照片的内容发给 fn（网页上显示缩略图）。照片不存在时返回 false
bool photos_read(PhotoSource src, const String &name,
                 void (*fn)(void *ctx, const uint8_t *data, size_t len, size_t total), void *ctx);

// 以下给 bing.cpp 用，date 是 8 位日期（如 20261020），都可能等 SD 卡的锁，不要在主循环里调用

// 能不能往 TF 卡存必应图片（没挂载时试着挂载一次）
bool photos_bing_card();
// 卡上已经有这一天的图，或者在网页上删掉过
bool photos_bing_known(const char *date);
// 存到 TF 卡的 bing 文件夹，说明文字存在同名的 .txt 里
bool photos_bing_save(const char *date, const uint8_t *data, size_t len, const char *caption);
// 没插卡时把当天这一张放在内存里，接管 data（heap_caps_malloc 分配的 PSRAM），换图时释放旧的
void photos_bing_keep(const char *date, uint8_t *data, size_t len, const char *caption);
// 内存里放的是不是这一天的图
bool photos_bing_kept(const char *date);
// 最多留 keep 张，先删最早的；早于 oldest 的删除记录也清掉（接口不会再给那么早的图）
void photos_bing_prune(int keep, const char *oldest);
