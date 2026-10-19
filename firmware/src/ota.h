#pragma once

#include <stddef.h>
#include <stdint.h>

// 在线升级：从 GitHub Release 的 manifest.json 检查新版本，下载后校验 MD5 写入另一个 OTA 分区

enum class OtaState : uint8_t {
  Idle,
  Checking,
  UpToDate,
  Available,    // 有新版本，等待安装（自动更新时会在凌晨 3~5 点安装）
  Downloading,
  Done,         // 写入并校验成功，等待重启
  Failed,
};

struct OtaStatus {
  OtaState state;
  char latest[24];  // 最新版本号（检查过才有）
  int progress;     // 下载进度 0~100
  char error[64];
};

// 当前固件版本，由编译脚本从 git 标签生成；开发版是提交号或 "dev"
const char *ota_current_version();

// 启动后台任务：开机后检查一次，之后每 24 小时检查一次
void ota_begin();

void ota_check_now();
void ota_install_now();

// 线程安全，返回状态副本
OtaStatus ota_status();

// 网页上传固件：分块写入，begin 时传入第一块数据用于检查文件格式，total 是请求的大致总长度（算进度用）
// 返回 false 表示出错，错误信息写入 ota_status().error
bool ota_upload_begin(const uint8_t *first, size_t len, size_t total);
bool ota_upload_write(const uint8_t *data, size_t len);
bool ota_upload_end();
void ota_upload_abort();
