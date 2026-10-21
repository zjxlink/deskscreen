#pragma once

#include <stddef.h>
#include <stdint.h>

// 歌曲标签、歌词文件的文字转成 UTF-8（界面只认 UTF-8）。不依赖硬件，可在电脑上测试

// 整段都是合法的 UTF-8
bool utf8_valid(const uint8_t *s, size_t n);

// 把 n 字节当成某种编码转成 UTF-8 写进 out（总是以 '\0' 结尾，放不下时在整字处截断），返回写了多少字节。
// 遇到 '\0' 就停。转不了的字变成 '?'
size_t text_from_latin1(const uint8_t *s, size_t n, char *out, size_t cap);
size_t text_from_gbk(const uint8_t *s, size_t n, char *out, size_t cap);
// big_endian 为 false 时是小端（Windows 的「Unicode」）。开头的 BOM 会自己认出来并跳过
size_t text_from_utf16(const uint8_t *s, size_t n, bool big_endian, char *out, size_t cap);

// 不知道是什么编码的单字节文字（老 MP3 标签、歌词文件）：有 UTF-8 BOM 或者本身是合法 UTF-8 就当 UTF-8；
// 有 UTF-16 BOM 按 UTF-16；否则大多是 GBK（中文 Windows 存的），GBK 也解不通时按 Latin-1
size_t text_guess(const uint8_t *s, size_t n, char *out, size_t cap);

// 把 UTF-8 字符串截到最多 max 字节（不含结尾的 '\0'），不切断一个字
void utf8_truncate(char *s, size_t max);

// 去掉首尾空白（空格、制表、回车换行和全角空格）
void text_trim(char *s);
