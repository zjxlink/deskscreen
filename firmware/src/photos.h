#pragma once

#include <Arduino.h>

// 电子相册：TF 卡（BOX-3 的 SENSOR 底座上）photos 文件夹里的照片。
// 解码在后台任务里做，手机原图也不会卡住界面；解码好的一帧是 320x240 的 RGB565，放在 PSRAM。
// 网页上传、删除照片也走这里，和后台任务用同一把锁，不会同时读写卡

enum class PhotoCard {
  NoSlot,   // 这块板子没有 TF 卡槽
  NoCard,   // 没插卡、卡坏了或者不是 FAT32
  Empty,    // 有卡，photos 文件夹里没有照片
  Ok,
};

struct PhotoStatus {
  PhotoCard card;
  int count;        // 照片张数
  int failed;       // 最近一轮读不出来的张数（HEIC、渐进式 JPEG 等）
};

// 开机时调用：给卡上电，尝试挂载，启动后台解码任务
void photos_begin();

bool photos_supported();
PhotoStatus photos_status();

// 请后台解码下一张（随机或按文件名顺序）。还在解码上一张、或者上一张还没取走时忽略
void photos_request_next(bool shuffle);

// 后台正在读卡、解码
bool photos_busy();

// 有新解码好的一帧时返回 true。px 一直有效，直到下一次返回 true 后又过了一帧
bool photos_take_frame(const uint16_t *&px);

// 网页设置用：照片列表（JSON：卡的状态、空间和每张照片的文件名、大小）
String photos_list_json();

// 网页上传：先写到临时文件，结束时改成正式文件名（重名时自动加序号）
bool photos_upload_begin(const String &filename);
bool photos_upload_write(const uint8_t *data, size_t len);
bool photos_upload_end(String &saved_name);
void photos_upload_abort();

// 删除一张照片，name 只能是 photos 文件夹里的文件名
bool photos_delete(const String &name);

// 把一张照片的内容发给 fn（网页上显示缩略图）。照片不存在时返回 false
bool photos_read(const String &name, void (*fn)(void *ctx, const uint8_t *data, size_t len, size_t total), void *ctx);
