#pragma once

#include <stddef.h>
#include <stdint.h>

// 影片页里不依赖硬件的部分：认哪些文件是影片、看到哪里的记录、时间的写法。可在电脑上测试

// TF 卡 movies 文件夹里的 .avi（不分大小写），隐藏文件和 macOS 生成的 ._ 文件不算
bool movie_is_file(const char *name);

// 看到哪里的记录存在卡上的一个文本文件里，每行「文件名<Tab>秒数」。
// 找 name 的那一行，返回秒数；没有时返回 0
uint32_t movie_progress_find(const char *text, const char *name);

// 把 name 的秒数写进记录（已有就换掉，seconds 为 0 时删掉这一行），结果写到 out，返回长度。
// out 放不下时只保留放得下的整行
size_t movie_progress_set(const char *text, const char *name, uint32_t seconds, char *out, size_t out_size);

// 列表和播放画面上显示的片名：去掉 .avi
void movie_display_name(const char *name, char *out, size_t out_size);

// 时长写成「1:05:30」或「47:30」
void movie_format_time(uint32_t ms, char *out, size_t out_size);

// 看到哪里的记录离结尾不到这么久，就算看完了，下次从头放
static constexpr uint32_t kMovieEndSlackMs = 30000;
