// esp-sr 的预编译库通过 get_static_srmodels() 找唤醒词模型。官方做法是把模型烧到单独的 model 分区再映射进来，
// 这里改成直接用编进固件的数组（include/sr_model_wn9.h，由 tools/esp_sr.py 生成），不用改分区表，在线升级也能带上。
// 只实现库里用到的几个函数，和 esp-sr v1.7.1 的 src/model_path.c 保持一致
#ifdef HAS_SR

#include <stdlib.h>
#include <string.h>

#include "model_path.h"
#include "sr_model_wn9.h"

static srmodel_list_t *models;

// _MODEL_INFO_ 里 # 开头的是注释，剩下的一行是模型说明（类型、版本、唤醒词、阈值）
static char *model_info(const char *data, int size) {
  while (size > 0 && *data == '#') {
    while (size > 1 && *data != '\n') {
      data++;
      size--;
    }
    data++;
    size--;
  }
  if (size <= 0) return nullptr;
  char *info = (char *)malloc(size + 1);
  memcpy(info, data, size);
  info[size] = 0;
  if (size > 0 && info[size - 1] == '\n') info[size - 1] = 0;
  return info;
}

extern "C" srmodel_list_t *get_static_srmodels(void) {
  if (models) return models;
  auto *m = (srmodel_list_t *)calloc(1, sizeof(srmodel_list_t));
  auto *d = (srmodel_data_t *)calloc(1, sizeof(srmodel_data_t));
  m->num = 1;
  m->model_name = (char **)malloc(sizeof(char *));
  m->model_info = (char **)malloc(sizeof(char *));
  m->model_data = (srmodel_data_t **)malloc(sizeof(srmodel_data_t *));
  m->model_name[0] = (char *)SR_MODEL_NAME;
  m->model_info[0] = nullptr;
  d->num = SR_FILE_COUNT;
  d->files = (char **)malloc(sizeof(char *) * SR_FILE_COUNT);
  d->data = (char **)malloc(sizeof(char *) * SR_FILE_COUNT);
  d->sizes = (int *)malloc(sizeof(int) * SR_FILE_COUNT);
  for (int i = 0; i < SR_FILE_COUNT; i++) {
    d->files[i] = (char *)SR_FILE_NAMES[i];
    d->data[i] = (char *)SR_FILE_DATA[i];
    d->sizes[i] = SR_FILE_SIZES[i];
    if (strcmp(SR_FILE_NAMES[i], "_MODEL_INFO_") == 0) m->model_info[0] = model_info(d->data[i], d->sizes[i]);
  }
  m->model_data[0] = d;
  models = m;
  return models;
}

// 返回 NULL 表示模型数据在内存里（不是从 SPIFFS、SD 卡按路径读文件）
extern "C" char *get_model_base_path(void) { return nullptr; }

// 只有老的 WakeNet5 用，这里不用
extern "C" model_coeff_getter_t *srmodel_get_model_coeff(char *) { return nullptr; }

// 官方版本按 sdkconfig 提示 CPU、Flash、PSRAM 频率是否够，这里不需要
extern "C" void check_chip_config(void) {}

#endif
