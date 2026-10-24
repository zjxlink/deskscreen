#pragma once

// 把容器和 JSON 文档的内存放进 PSRAM：内部内存只有几十 KB 余量，用完了 HTTPS 就全连不上
#include <ArduinoJson.h>
#include <esp_heap_caps.h>

#include <stdlib.h>
#include <string>

inline void *psram_malloc(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }

// ArduinoJson 用：JsonDocument doc(psram_json());
struct PsramJsonAllocator : ArduinoJson::Allocator {
  void *allocate(size_t n) override { return psram_malloc(n); }
  void deallocate(void *p) override { heap_caps_free(p); }
  void *reallocate(void *p, size_t n) override { return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
};
inline PsramJsonAllocator *psram_json() {
  static PsramJsonAllocator a;
  return &a;
}

// std::vector、std::basic_string 用
template <typename T>
struct PsramStl {
  using value_type = T;
  PsramStl() = default;
  template <typename U>
  PsramStl(const PsramStl<U> &) {}
  T *allocate(size_t n) {
    void *p = psram_malloc(n * sizeof(T));
    if (!p) p = malloc(n * sizeof(T));  // PSRAM 满了（不应该）才退回内部内存
    return (T *)p;
  }
  void deallocate(T *p, size_t) { heap_caps_free(p); }
  template <typename U>
  bool operator==(const PsramStl<U> &) const { return true; }
  template <typename U>
  bool operator!=(const PsramStl<U> &) const { return false; }
};

using PsString = std::basic_string<char, std::char_traits<char>, PsramStl<char>>;
