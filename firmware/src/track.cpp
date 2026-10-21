#include "track.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "text_conv.h"

static const char *ext_of(const char *name) {
  const char *dot = strrchr(name, '.');
  const char *slash = strrchr(name, '/');
  return dot && (!slash || dot > slash) ? dot : nullptr;
}

static const char *base_of(const char *path) {
  const char *slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

TrackFormat track_format(const char *name) {
  if (!name) return TrackFormat::None;
  const char *base = base_of(name);
  const char *ext = ext_of(base);
  if (base[0] == '.' || !ext || ext == base) return TrackFormat::None;
  if (strcasecmp(ext, ".mp3") == 0) return TrackFormat::Mp3;
  if (strcasecmp(ext, ".flac") == 0) return TrackFormat::Flac;
  if (strcasecmp(ext, ".wav") == 0) return TrackFormat::Wav;
  return TrackFormat::None;
}

bool track_lrc_path(const char *path, char *out, size_t cap) {
  const char *ext = ext_of(path);
  size_t stem = ext ? (size_t)(ext - path) : strlen(path);
  if (stem + 5 > cap) return false;
  memcpy(out, path, stem);
  memcpy(out + stem, ".lrc", 5);
  return true;
}

// ---- 读文件的小工具 ----

namespace {

struct Src {
  TrackRead read;
  void *ctx;
  uint32_t size;
  // 读 len 字节，读不全时返回 false
  bool get(uint32_t off, void *buf, size_t len) const {
    return off <= size && len <= size - off && read(ctx, off, (uint8_t *)buf, len) == len;
  }
};

uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
uint32_t be24(const uint8_t *p) { return (uint32_t)p[0] << 16 | p[1] << 8 | p[2]; }
uint32_t le32(const uint8_t *p) { return (uint32_t)p[3] << 24 | p[2] << 16 | p[1] << 8 | p[0]; }
uint16_t le16(const uint8_t *p) { return (uint16_t)(p[1] << 8 | p[0]); }
uint32_t syncsafe(const uint8_t *p) { return (p[0] & 0x7F) << 21 | (p[1] & 0x7F) << 14 | (p[2] & 0x7F) << 7 | (p[3] & 0x7F); }

void set_tag(char *dst, const char *src) {
  if (dst[0] || !src[0]) return;  // 已经有了就不覆盖（ID3v2 比 ID3v1 准）
  strncpy(dst, src, kTagMax - 1);
  dst[kTagMax - 1] = '\0';
  utf8_truncate(dst, kTagMax - 1);
}

// ID3v2 文本帧：第一个字节是编码
void id3_text(const uint8_t *body, size_t n, char *out, size_t cap) {
  out[0] = '\0';
  if (n < 2) return;
  switch (body[0]) {
    case 1: text_from_utf16(body + 1, n - 1, false, out, cap); break;  // 带 BOM
    case 2: text_from_utf16(body + 1, n - 1, true, out, cap); break;
    default: text_guess(body + 1, n - 1, out, cap); break;  // 0 名义上是 Latin-1，中文歌其实多是 GBK；3 是 UTF-8
  }
  text_trim(out);
}

// 跳过一个以 0 结尾的字符串（UTF-16 时是两个 0 字节），返回结尾后面的位置，找不到时返回 n
size_t skip_string(const uint8_t *p, size_t at, size_t n, bool wide) {
  if (!wide) {
    while (at < n && p[at]) at++;
    return at < n ? at + 1 : n;
  }
  while (at + 1 < n && (p[at] || p[at + 1])) at += 2;
  return at + 1 < n ? at + 2 : n;
}

bool is_jpeg_mime(const char *mime) {
  return strcasecmp(mime, "image/jpeg") == 0 || strcasecmp(mime, "image/jpg") == 0 || strcasecmp(mime, "jpg") == 0;
}

// 读 ID3v2 标签，返回整个标签有多长（没有标签时返回 0）
uint32_t parse_id3v2(const Src &src, uint32_t at, TrackProbe &out) {
  uint8_t h[10];
  if (!src.get(at, h, 10) || memcmp(h, "ID3", 3) != 0 || h[3] < 2 || h[3] > 4) return 0;
  const int ver = h[3];
  const uint8_t flags = h[5];
  const uint32_t size = syncsafe(h + 6);
  const uint32_t total = 10 + size + ((flags & 0x10) ? 10 : 0);
  const bool unsync = (flags & 0x80) && ver < 4;  // 整个标签做过反同步：文字一般没事，封面就不认了
  uint32_t pos = at + 10;
  const uint32_t end = at + 10 + size;
  if ((flags & 0x40) && ver >= 3) {  // 扩展头
    uint8_t e[4];
    if (!src.get(pos, e, 4)) return total;
    pos += ver == 4 ? syncsafe(e) : 4 + be32(e);
  }
  const int hlen = ver == 2 ? 6 : 10;
  int cover_type = -1;
  uint8_t body[512];
  char text[kTagMax * 2];
  while (pos + hlen <= end) {
    uint8_t f[10];
    if (!src.get(pos, f, hlen) || f[0] == 0) break;  // 后面是填充
    char id[5] = {};
    uint32_t fsize;
    bool skip = false;
    uint32_t fbody = pos + hlen;
    if (ver == 2) {
      memcpy(id, f, 3);
      fsize = be24(f + 3);
    } else {
      memcpy(id, f, 4);
      fsize = ver == 4 ? syncsafe(f + 4) : be32(f + 4);
      uint8_t fl = f[9];
      if (ver == 3) skip = fl & 0xC0;  // 压缩、加密
      if (ver == 4) {
        skip = fl & 0x0E;  // 压缩、加密、反同步
        if ((fl & 0x01) && fsize >= 4) {  // 前面多了 4 字节的长度
          fbody += 4;
          fsize -= 4;
        }
      }
    }
    if (fsize > end - fbody) break;
    pos = fbody + fsize;
    if (skip || fsize == 0) continue;
    char *dst = nullptr;
    if (!strcmp(id, "TIT2") || !strcmp(id, "TT2")) dst = out.title;
    else if (!strcmp(id, "TPE1") || !strcmp(id, "TP1")) dst = out.artist;
    else if (!strcmp(id, "TALB") || !strcmp(id, "TAL")) dst = out.album;
    if (dst) {
      size_t n = fsize < sizeof(body) ? fsize : sizeof(body);
      if (!src.get(fbody, body, n)) break;
      id3_text(body, n, text, sizeof(text));
      set_tag(dst, text);
      continue;
    }
    bool apic = !strcmp(id, "APIC"), pic = !strcmp(id, "PIC");
    if ((apic || pic) && !unsync && cover_type != 3) {
      size_t n = fsize < sizeof(body) ? fsize : sizeof(body);
      if (!src.get(fbody, body, n)) break;
      bool wide = body[0] == 1 || body[0] == 2;
      char mime[32] = {};
      size_t p;
      if (pic) {
        memcpy(mime, body + 1, 3);
        p = 4;
      } else {
        p = skip_string(body, 1, n, false);
        size_t m = p - 1 > sizeof(mime) - 1 ? sizeof(mime) - 1 : p - 1;
        memcpy(mime, body + 1, m);
      }
      if (p >= n) continue;
      int type = body[p];
      p = skip_string(body, p + 1, n, wide);  // 说明文字
      if (p >= n || p >= fsize) continue;
      bool jpeg = is_jpeg_mime(mime) || (p + 1 < n && body[p] == 0xFF && body[p + 1] == 0xD8);
      if (!jpeg) continue;
      cover_type = type;  // 3 是正面封面，有了就不再换
      out.cover_off = fbody + p;
      out.cover_len = fsize - p;
    }
  }
  return total;
}

void parse_id3v1(const Src &src, TrackProbe &out) {
  uint8_t t[128];
  if (src.size < 128 || !src.get(src.size - 128, t, 128) || memcmp(t, "TAG", 3) != 0) return;
  char text[kTagMax * 2];
  char *dst[3] = {out.title, out.artist, out.album};
  for (int i = 0; i < 3; i++) {
    text_guess(t + 3 + i * 30, 30, text, sizeof(text));
    text_trim(text);
    set_tag(dst[i], text);
  }
}

// ---- MP3 ----

struct Mp3Header {
  int version;  // 1 = MPEG1，2 = MPEG2，25 = MPEG2.5
  int layer;
  uint32_t bitrate;  // bps
  uint32_t rate;
  int channels;
  uint32_t bytes;    // 这一帧多长
  int samples;       // 这一帧多少采样
};

bool mp3_header(const uint8_t *b, Mp3Header &h) {
  static const uint16_t kRates[3] = {44100, 48000, 32000};
  static const uint16_t kBr1[3][15] = {
      {0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448},
      {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384},
      {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320}};
  static const uint16_t kBr2[2][15] = {
      {0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256},
      {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160}};
  if (b[0] != 0xFF || (b[1] & 0xE0) != 0xE0) return false;
  int v = (b[1] >> 3) & 3, l = (b[1] >> 1) & 3, bri = b[2] >> 4, sri = (b[2] >> 2) & 3;
  if (v == 1 || l == 0 || bri == 0 || bri == 15 || sri == 3) return false;
  h.version = v == 3 ? 1 : v == 2 ? 2 : 25;
  h.layer = 4 - l;
  h.rate = kRates[sri] / (h.version == 1 ? 1 : h.version == 2 ? 2 : 4);
  h.bitrate = (h.version == 1 ? kBr1[h.layer - 1][bri] : kBr2[h.layer == 1 ? 0 : 1][bri]) * 1000u;
  h.channels = (b[3] >> 6) == 3 ? 1 : 2;
  int pad = (b[2] >> 1) & 1;
  if (h.layer == 1) {
    h.samples = 384;
    h.bytes = (12 * h.bitrate / h.rate + pad) * 4;
  } else {
    h.samples = (h.layer == 3 && h.version != 1) ? 576 : 1152;
    h.bytes = h.samples / 8 * h.bitrate / h.rate + pad;
  }
  return h.bytes >= 24;
}

bool probe_mp3(const Src &src, TrackProbe &out) {
  uint32_t start = parse_id3v2(src, 0, out);
  // 有的文件开头有两个 ID3 标签
  if (start) {
    uint32_t more = parse_id3v2(src, start, out);
    start += more;
  }
  parse_id3v1(src, out);
  uint32_t end = src.size;
  uint8_t tail[3];
  if (src.size >= 128 && src.get(src.size - 128, tail, 3) && memcmp(tail, "TAG", 3) == 0) end -= 128;
  // 找第一帧：帧头对得上，而且按帧长跳过去下一帧的帧头也对得上，免得把标签里的数据当成帧头
  static constexpr uint32_t kSearch = 128 * 1024;
  uint8_t buf[1024];
  Mp3Header h = {};
  uint32_t found = 0;
  bool ok = false;
  for (uint32_t base = start; base < end && base < start + kSearch && !ok; base += sizeof(buf) - 3) {
    uint32_t n = end - base < sizeof(buf) ? end - base : sizeof(buf);
    if (n < 4 || !src.get(base, buf, n)) break;
    for (uint32_t i = 0; i + 4 <= n; i++) {
      if (!mp3_header(buf + i, h)) continue;
      uint32_t next = base + i + h.bytes;
      Mp3Header h2;
      uint8_t nb[4];
      if (next + 4 > end || (src.get(next, nb, 4) && mp3_header(nb, h2) && h2.rate == h.rate && h2.layer == h.layer)) {
        found = base + i;
        ok = true;
        break;
      }
    }
    if (n < sizeof(buf)) break;
  }
  if (!ok) return false;
  out.rate = h.rate;
  out.channels = (uint8_t)h.channels;
  out.mp3_frame_samples = (uint16_t)h.samples;
  out.mp3_bitrate = h.bitrate;
  out.data_off = found;
  out.data_len = end - found;
  // Xing/Info（LAME 写的 VBR 头）或者 VBRI（Fraunhofer）：有总帧数就能算准时长
  uint8_t x[160] = {};
  uint32_t frames = 0;
  int side = h.version == 1 ? (h.channels == 1 ? 17 : 32) : (h.channels == 1 ? 9 : 17);
  if (src.get(found, x, sizeof(x) < h.bytes ? sizeof(x) : h.bytes)) {
    const uint8_t *p = x + 4 + side;
    if (4 + side + 8 <= (int)sizeof(x) && (!memcmp(p, "Xing", 4) || !memcmp(p, "Info", 4))) {
      uint32_t flags = be32(p + 4);
      p += 8;
      if (flags & 1) {
        frames = be32(p);
        p += 4;
      }
      if (flags & 2) {
        uint32_t bytes = be32(p);
        if (bytes > 0 && bytes <= out.data_len) out.data_len = bytes;
        p += 4;
      }
      if ((flags & 4) && p + 100 <= x + sizeof(x)) {
        memcpy(out.mp3_toc, p, 100);
        out.mp3_has_toc = true;
      }
    } else if (!memcmp(x + 36, "VBRI", 4)) {
      frames = be32(x + 36 + 14);
    }
  }
  if (frames) {
    out.dur_ms = (uint32_t)((uint64_t)frames * h.samples * 1000 / h.rate);
  } else if (h.bitrate) {
    out.dur_ms = (uint32_t)((uint64_t)out.data_len * 8000 / h.bitrate);
  }
  return true;
}

// ---- FLAC ----

void vorbis_comment(const uint8_t *b, size_t n, TrackProbe &out) {
  if (n < 8) return;
  size_t p = 4 + le32(b);
  if (p + 4 > n) return;
  uint32_t count = le32(b + p);
  p += 4;
  char text[kTagMax * 2];
  for (uint32_t i = 0; i < count && p + 4 <= n; i++) {
    uint32_t len = le32(b + p);
    p += 4;
    if (len > n - p) break;
    const char *c = (const char *)b + p;
    const char *eq = (const char *)memchr(c, '=', len);
    if (eq) {
      size_t klen = eq - c;
      char *dst = nullptr;
      if (klen == 5 && !strncasecmp(c, "TITLE", 5)) dst = out.title;
      else if (klen == 6 && !strncasecmp(c, "ARTIST", 6)) dst = out.artist;
      else if (klen == 5 && !strncasecmp(c, "ALBUM", 5)) dst = out.album;
      if (dst) {
        text_guess((const uint8_t *)eq + 1, len - klen - 1, text, sizeof(text));
        text_trim(text);
        set_tag(dst, text);
      }
    }
    p += len;
  }
}

bool probe_flac(const Src &src, TrackProbe &out) {
  uint32_t at = parse_id3v2(src, 0, out);
  uint8_t h[4];
  if (!src.get(at, h, 4) || memcmp(h, "fLaC", 4) != 0) return false;
  at += 4;
  bool info = false;
  for (int blocks = 0; blocks < 64; blocks++) {
    uint8_t bh[4];
    if (!src.get(at, bh, 4)) return false;
    bool last = bh[0] & 0x80;
    int type = bh[0] & 0x7F;
    uint32_t len = be24(bh + 1);
    uint32_t body = at + 4;
    if (type == 0 && len >= 18) {
      uint8_t s[18];
      if (!src.get(body, s, 18)) return false;
      out.rate = (uint32_t)s[10] << 12 | s[11] << 4 | s[12] >> 4;
      out.channels = (uint8_t)(((s[12] >> 1) & 7) + 1);
      out.bits = (uint8_t)((((s[12] & 1) << 4) | s[13] >> 4) + 1);
      uint64_t total = (uint64_t)(s[13] & 0x0F) << 32 | be32(s + 14);
      if (out.rate) out.dur_ms = (uint32_t)(total * 1000 / out.rate);
      info = out.rate > 0;
    } else if (type == 4) {
      size_t n = len < 8192 ? len : 8192;
      uint8_t *b = (uint8_t *)malloc(n);
      if (b && src.get(body, b, n)) vorbis_comment(b, n, out);
      free(b);
    } else if (type == 6 && len > 32 && !out.cover_len) {
      uint8_t b[256];
      size_t n = len < sizeof(b) ? len : sizeof(b);
      if (src.get(body, b, n)) {
        uint32_t mlen = be32(b + 4);
        if (8 + mlen + 4 <= n) {
          char mime[32] = {};
          memcpy(mime, b + 8, mlen < sizeof(mime) - 1 ? mlen : sizeof(mime) - 1);
          uint32_t dlen = be32(b + 8 + mlen);
          uint32_t p = 8 + mlen + 4 + dlen + 16;
          if (p + 4 <= n && is_jpeg_mime(mime)) {
            uint32_t size = be32(b + p);
            if (size <= len - p - 4) {
              out.cover_off = body + p + 4;
              out.cover_len = size;
            }
          }
        }
      }
    }
    at = body + len;
    if (last) break;
  }
  out.data_off = at;
  out.data_len = src.size > at ? src.size - at : 0;
  return info;
}

// ---- WAV ----

bool probe_wav(const Src &src, TrackProbe &out) {
  uint8_t h[12];
  if (!src.get(0, h, 12) || memcmp(h, "RIFF", 4) != 0 || memcmp(h + 8, "WAVE", 4) != 0) return false;
  uint32_t at = 12;
  bool fmt = false;
  uint16_t align = 0;
  while (at + 8 <= src.size) {
    uint8_t c[8];
    if (!src.get(at, c, 8)) break;
    uint32_t len = le32(c + 4);
    if (!memcmp(c, "fmt ", 4) && len >= 16) {
      uint8_t f[40] = {};
      if (!src.get(at + 8, f, len < sizeof(f) ? len : sizeof(f))) return false;
      uint16_t tag = le16(f);
      if (tag == 0xFFFE && len >= 26) tag = le16(f + 24);  // WAVE_FORMAT_EXTENSIBLE：看子格式
      out.channels = (uint8_t)le16(f + 2);
      out.rate = le32(f + 4);
      align = le16(f + 12);
      out.bits = (uint8_t)le16(f + 14);
      fmt = tag == 1 && (out.bits == 16 || out.bits == 24) && out.channels >= 1 && out.channels <= 2 && out.rate &&
            align == out.channels * out.bits / 8;
      if (!fmt) return false;
    } else if (!memcmp(c, "data", 4) && fmt) {
      out.data_off = at + 8;
      uint32_t room = src.size - out.data_off;
      out.data_len = len > room ? room : len;  // 边录边写的文件长度可能是 0 或者乱的
      if (out.data_len == 0) out.data_len = room;
      out.data_len -= out.data_len % align;
      out.dur_ms = (uint32_t)((uint64_t)(out.data_len / align) * 1000 / out.rate);
      return true;
    }
    at += 8 + len + (len & 1);
  }
  return false;
}

}  // namespace

bool track_probe(TrackRead read, void *ctx, uint32_t size, TrackFormat format, TrackProbe &out) {
  memset(&out, 0, sizeof(out));
  out.format = format;
  Src src = {read, ctx, size};
  switch (format) {
    case TrackFormat::Mp3: return probe_mp3(src, out);
    case TrackFormat::Flac: return probe_flac(src, out);
    case TrackFormat::Wav: return probe_wav(src, out);
    default: return false;
  }
}

uint32_t track_mp3_seek_offset(const TrackProbe &p, uint32_t ms) {
  if (ms == 0 || p.data_len == 0) return p.data_off;
  uint64_t rel;
  if (p.mp3_has_toc && p.dur_ms) {
    float pct = (float)ms * 100.0f / p.dur_ms;
    if (pct > 99.9f) pct = 99.9f;
    int i = (int)pct;
    float a = p.mp3_toc[i], b = i < 99 ? p.mp3_toc[i + 1] : 256.0f;
    rel = (uint64_t)((a + (b - a) * (pct - i)) / 256.0f * p.data_len);
  } else if (p.mp3_bitrate) {
    rel = (uint64_t)ms * p.mp3_bitrate / 8000;
  } else {
    return p.data_off;
  }
  if (rel >= p.data_len) rel = p.data_len ? p.data_len - 1 : 0;
  return p.data_off + (uint32_t)rel;
}

// 去掉 FAT 不认的字符，最多 cap - 1 字节
static size_t clean_name(const char *in, char *out, size_t cap) {
  size_t n = 0;
  for (const char *p = in ? in : ""; *p && n + 1 < cap; p++) {
    unsigned char c = *p;
    if (c < 0x20 || c == 0x7F || strchr("\\/:*?\"<>|", c)) continue;
    if (n == 0 && (c == '.' || c == ' ')) continue;
    out[n++] = c;
  }
  out[n] = '\0';
  utf8_truncate(out, n);
  return strlen(out);
}

bool track_safe_name(const char *in, char *out, size_t cap) {
  if (cap < 8) return false;
  char tmp[kTrackPathMax * 2];
  clean_name(in, tmp, sizeof(tmp));
  const char *ext = ext_of(tmp);
  static const char *const kExts[] = {".mp3", ".flac", ".wav", ".lrc"};
  const char *want = nullptr;
  for (const char *e : kExts) {
    if (ext && strcasecmp(ext, e) == 0) want = e;
  }
  if (!want) return false;
  size_t elen = strlen(want);
  size_t stem = ext - tmp;
  if (stem + elen + 1 > cap) stem = cap - elen - 1;
  while (stem > 0 && ((uint8_t)tmp[stem] & 0xC0) == 0x80) stem--;  // 不切断一个字
  tmp[stem] = '\0';
  while (stem > 0 && (tmp[stem - 1] == ' ' || tmp[stem - 1] == '.')) tmp[--stem] = '\0';  // FAT 不认结尾的空格和点
  if (stem == 0) return false;
  memcpy(out, tmp, stem);
  memcpy(out + stem, want, elen + 1);
  return true;
}

bool track_safe_folder(const char *in, char *out, size_t cap) {
  size_t n = clean_name(in, out, cap);
  while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '.')) out[--n] = '\0';
  return n > 0;
}

void track_name_from_file(const char *path, char *title, char *artist, size_t cap) {
  const char *base = base_of(path);
  const char *ext = ext_of(base);
  size_t n = ext ? (size_t)(ext - base) : strlen(base);
  char name[kTrackPathMax];
  if (n >= sizeof(name)) n = sizeof(name) - 1;
  memcpy(name, base, n);
  name[n] = '\0';
  artist[0] = '\0';
  const char *dash = strstr(name, " - ");
  if (dash && dash != name && dash[3]) {
    size_t a = dash - name;
    if (a >= cap) a = cap - 1;
    memcpy(artist, name, a);
    artist[a] = '\0';
    utf8_truncate(artist, cap - 1);
    text_trim(artist);
    strncpy(title, dash + 3, cap - 1);
  } else {
    strncpy(title, name, cap - 1);
  }
  title[cap - 1] = '\0';
  utf8_truncate(title, cap - 1);
  text_trim(title);
}

// ---- 歌词 ----

// 解析 [mm:ss] [mm:ss.xx] [mm:ss:xx] [mm:ss.xxx]，成功时返回毫秒数
static bool lrc_time(const char *s, size_t n, uint32_t &ms) {
  size_t i = 0;
  uint32_t m = 0, sec = 0, frac = 0, digits = 0;
  if (i >= n || !isdigit((unsigned char)s[i])) return false;
  while (i < n && isdigit((unsigned char)s[i])) m = m * 10 + (s[i++] - '0');
  if (i >= n || s[i++] != ':') return false;
  if (i >= n || !isdigit((unsigned char)s[i])) return false;
  while (i < n && isdigit((unsigned char)s[i])) sec = sec * 10 + (s[i++] - '0');
  if (i < n && (s[i] == '.' || s[i] == ':')) {
    i++;
    while (i < n && isdigit((unsigned char)s[i])) {
      if (digits < 3) frac = frac * 10 + (s[i] - '0'), digits++;
      i++;
    }
  }
  if (i != n || sec >= 60) return false;
  while (digits < 3) frac *= 10, digits++;
  ms = (m * 60 + sec) * 1000 + frac;
  return true;
}

void lrc_parse(const uint8_t *data, size_t len, Lyrics &out) {
  out.count = 0;
  if (!out.lines || !out.pool || out.pool_size == 0) return;
  // 先整段转成 UTF-8（GBK 的歌词很常见），放在文字池的后半截，解析出来的歌词写到前半截
  size_t half = out.pool_size / 2;
  char *text = out.pool + half;
  size_t tlen = text_guess(data, len, text, out.pool_size - half);
  size_t used = 0;
  int32_t offset = 0;
  out.pool[used++] = '\0';  // 位置 0 是空字符串
  const char *p = text, *end = text + tlen;
  while (p < end) {
    const char *eol = (const char *)memchr(p, '\n', end - p);
    if (!eol) eol = end;
    const char *q = p;
    uint32_t times[16];
    int nt = 0;
    while (q < eol && *q == '[') {
      const char *close = (const char *)memchr(q, ']', eol - q);
      if (!close) break;
      uint32_t ms;
      if (lrc_time(q + 1, close - q - 1, ms)) {
        if (nt < 16) times[nt++] = ms;
      } else if (close - q > 8 && !strncasecmp(q + 1, "offset:", 7)) {
        offset = atoi(q + 8);
      }
      q = close + 1;
    }
    if (nt > 0) {
      size_t n = eol - q;
      while (n > 0 && (q[n - 1] == '\r' || q[n - 1] == ' ')) n--;
      while (n > 0 && *q == ' ') q++, n--;
      uint16_t at = 0;
      if (n > 0 && used + n + 1 <= half && used + n + 1 <= 0xFFFF) {
        at = (uint16_t)used;
        memcpy(out.pool + used, q, n);
        used += n;
        out.pool[used++] = '\0';
      }
      for (int i = 0; i < nt && out.count < out.max_lines; i++) out.lines[out.count++] = {times[i], at};
    }
    p = eol + 1;
  }
  // 按时间排（插入排序，稳定：同一时间的按出现顺序）
  for (int i = 1; i < out.count; i++) {
    LrcLine v = out.lines[i];
    int j = i - 1;
    while (j >= 0 && out.lines[j].ms > v.ms) {
      out.lines[j + 1] = out.lines[j];
      j--;
    }
    out.lines[j + 1] = v;
  }
  // [offset:+500] 表示歌词整体提前 500 毫秒
  if (offset) {
    for (int i = 0; i < out.count; i++) {
      int64_t t = (int64_t)out.lines[i].ms - offset;
      out.lines[i].ms = t < 0 ? 0 : (uint32_t)t;
    }
  }
}

int lrc_find(const Lyrics &l, uint32_t ms) {
  int lo = 0, hi = l.count - 1, best = -1;
  while (lo <= hi) {
    int mid = (lo + hi) / 2;
    if (l.lines[mid].ms <= ms) {
      best = mid;
      lo = mid + 1;
    } else {
      hi = mid - 1;
    }
  }
  return best;
}

// ---- 目录缓存 ----

static size_t put_field(char *out, size_t cap, size_t len, const char *s, char sep) {
  for (; *s && len + 2 < cap; s++) out[len++] = (*s == '\t' || *s == '\n' || *s == '\r') ? ' ' : *s;
  if (len + 1 < cap) out[len++] = sep;
  return len;
}

size_t track_index_line(const TrackInfo &t, char *out, size_t cap) {
  int n = snprintf(out, cap, "%u\t%u\t", (unsigned)t.size, (unsigned)t.dur_ms);
  if (n < 0 || (size_t)n >= cap) return 0;
  size_t len = n;
  len = put_field(out, cap, len, t.path, '\t');
  len = put_field(out, cap, len, t.title, '\t');
  len = put_field(out, cap, len, t.artist, '\t');
  len = put_field(out, cap, len, t.album, '\n');
  if (len >= cap || out[len - 1] != '\n') return 0;
  out[len] = '\0';
  return len;
}

bool track_index_parse(const char *line, size_t len, TrackInfo &out) {
  const char *f[6];
  size_t fl[6];
  int k = 0;
  const char *start = line;
  for (size_t i = 0; i <= len && k < 6; i++) {
    if (i == len || line[i] == '\t') {
      f[k] = start;
      fl[k] = line + i - start;
      k++;
      start = line + i + 1;
    }
  }
  if (k != 6 || fl[2] == 0 || fl[2] >= kTrackPathMax) return false;
  auto copy = [](char *dst, size_t cap, const char *s, size_t n) {
    if (n >= cap) n = cap - 1;
    memcpy(dst, s, n);
    dst[n] = '\0';
    utf8_truncate(dst, cap - 1);
  };
  char num[16];
  copy(num, sizeof(num), f[0], fl[0]);
  out.size = (uint32_t)strtoul(num, nullptr, 10);
  copy(num, sizeof(num), f[1], fl[1]);
  out.dur_ms = (uint32_t)strtoul(num, nullptr, 10);
  copy(out.path, sizeof(out.path), f[2], fl[2]);
  copy(out.title, sizeof(out.title), f[3], fl[3]);
  copy(out.artist, sizeof(out.artist), f[4], fl[4]);
  copy(out.album, sizeof(out.album), f[5], fl[5]);
  return track_format(out.path) != TrackFormat::None;
}

void track_folder(const char *path, char *out, size_t cap) {
  const char *slash = strchr(path, '/');
  size_t n = slash ? (size_t)(slash - path) : 0;
  if (n >= cap) n = cap - 1;
  memcpy(out, path, n);
  out[n] = '\0';
}

// ---- 播放顺序 ----

void PlayOrder::shuffle(uint32_t seed) {
  uint32_t x = seed ? seed : 0x9E3779B9u;
  for (int i = n_ - 1; i > 0; i--) {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    int j = (int)(x % (uint32_t)(i + 1));
    int t = order_[i];
    order_[i] = order_[j];
    order_[j] = t;
  }
}

void PlayOrder::set(int n, int current, PlayMode mode, uint32_t seed) {
  n_ = n < 0 ? 0 : n > kMax ? kMax : n;
  mode_ = mode;
  pos_ = 0;
  for (int i = 0; i < n_; i++) order_[i] = i;
  if (n_ == 0) return;
  if (current < 0 || current >= n_) current = 0;
  if (mode == PlayMode::Shuffle) {
    shuffle(seed);
    jump(current);
    // 当前这首换到最前面，后面的都还没放过
    int t = order_[0];
    order_[0] = order_[pos_];
    order_[pos_] = t;
    pos_ = 0;
  } else {
    pos_ = current;
  }
}

void PlayOrder::set_mode(PlayMode mode, uint32_t seed) { set(n_, current(), mode, seed); }

int PlayOrder::next(bool user) {
  if (n_ == 0) return -1;
  if (mode_ == PlayMode::One && !user) return current();
  pos_ = (pos_ + 1) % n_;
  return current();
}

int PlayOrder::prev() {
  if (n_ == 0) return -1;
  pos_ = (pos_ + n_ - 1) % n_;
  return current();
}

void PlayOrder::jump(int i) {
  for (int k = 0; k < n_; k++) {
    if (order_[k] == i) {
      pos_ = k;
      return;
    }
  }
}

int sleep_next_choice(int minutes) {
  if (minutes <= 0) return 15;
  if (minutes <= 15) return 30;
  if (minutes <= 30) return 60;
  return 0;
}
