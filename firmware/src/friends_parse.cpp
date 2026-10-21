#include <stdio.h>
#include <string.h>

#include "friends.h"

const char *const kFrPhrases[kFrPhraseCount] = {"想你了", "吃饭了吗", "加油", "早点睡", "晚安", "在忙吗"};

void friends_copy(char *out, size_t size, const char *in) {
  if (!size) return;
  if (!in) in = "";
  size_t n = strlen(in);
  if (n >= size) {
    n = size - 1;
    while (n > 0 && ((uint8_t)in[n] & 0xC0) == 0x80) n--;  // 停在一个字的开头
  }
  memcpy(out, in, n);
  out[n] = '\0';
}

static FrEventType event_type(const char *t) {
  if (!t) return FrEventType::Other;
  if (strcmp(t, "poke") == 0) return FrEventType::Poke;
  if (strcmp(t, "text") == 0) return FrEventType::Text;
  if (strcmp(t, "read") == 0) return FrEventType::Read;
  if (strcmp(t, "circle") == 0) return FrEventType::Circle;
  return FrEventType::Other;
}

bool friends_parse_sync(JsonDocument &doc, FrSync &out) {
  memset(&out, 0, sizeof(out));
  if (!doc["seq"].is<uint32_t>()) return false;
  out.seq = doc["seq"];
  friends_copy(out.me_id, sizeof(out.me_id), doc["me"]["id"] | "");
  friends_copy(out.me_name, sizeof(out.me_name), doc["me"]["name"] | "");
  for (JsonObject c : doc["circles"].as<JsonArray>()) {
    if (out.ncircles >= kFrMaxCircles) break;
    FrCircle &fc = out.circles[out.ncircles++];
    friends_copy(fc.id, sizeof(fc.id), c["id"] | "");
    friends_copy(fc.name, sizeof(fc.name), c["name"] | "");
    friends_copy(fc.owner, sizeof(fc.owner), c["owner"] | "");
    for (JsonObject m : c["members"].as<JsonArray>()) {
      if (fc.n >= kFrMaxMembers) break;
      FrMember &fm = fc.m[fc.n++];
      friends_copy(fm.id, sizeof(fm.id), m["id"] | "");
      friends_copy(fm.name, sizeof(fm.name), m["name"] | "");
      fm.online = m["online"] | false;
      fm.here = fm.online && (m["here"] | false);
      fm.me = m["me"] | false;
      fm.board = strcmp(m["kind"] | "", "board") == 0;
    }
  }
  JsonArray events = doc["events"].as<JsonArray>();
  // 太多时留最新的
  int skip = (int)events.size() - kFrMaxEvents;
  for (JsonObject e : events) {
    if (skip-- > 0) continue;
    FrEvent &fe = out.events[out.nevents++];
    fe.seq = e["seq"] | 0u;
    fe.type = event_type(e["type"]);
    fe.mine = e["mine"] | false;
    const char *to = e["to"] | "";
    fe.to_me = to[0] && strcmp(to, out.me_id) == 0;
    friends_copy(fe.id, sizeof(fe.id), e["id"] | "");
    friends_copy(fe.circle, sizeof(fe.circle), e["circle"] | "");
    friends_copy(fe.circle_name, sizeof(fe.circle_name), e["circle_name"] | "");
    friends_copy(fe.from, sizeof(fe.from), e["from"] | "");
    friends_copy(fe.from_name, sizeof(fe.from_name), e["from_name"] | "");
    friends_copy(fe.text, sizeof(fe.text), e["text"] | "");
    const char *photo = e["photo"] | "";
    // 只认服务器自己的照片路径，太长的不要
    if (photo[0] == '/' && strlen(photo) < sizeof(fe.photo)) friends_copy(fe.photo, sizeof(fe.photo), photo);
    fe.ts = e["ts"] | (int64_t)0;
    fe.thumb = -1;
  }
  return true;
}

void friends_presence_text(const FrCircle *circles, int n, char *out, size_t size) {
  const char *ids[kFrMaxCircles * kFrMaxMembers];
  const char *names[kFrMaxCircles * kFrMaxMembers];
  int count = 0;
  for (int c = 0; c < n; c++) {
    for (int i = 0; i < circles[c].n; i++) {
      const FrMember &m = circles[c].m[i];
      if (m.me || !m.here) continue;
      bool seen = false;
      for (int k = 0; k < count && !seen; k++) seen = strcmp(ids[k], m.id) == 0;
      if (seen) continue;
      ids[count] = m.id;
      names[count] = m.name;
      count++;
    }
  }
  if (size) out[0] = '\0';
  if (count == 0) return;
  if (count > 2) {
    snprintf(out, size, "%d 人在桌前", count);
  } else if (count == 2) {
    snprintf(out, size, "%s、%s在桌前", names[0], names[1]);
  } else {
    snprintf(out, size, "%s在桌前", names[0]);
  }
}

void friends_add_name(char *names, size_t size, int &count, const char *name) {
  // names 是「、」隔开的名字，最多 3 个
  size_t len = strlen(names);
  const char *p = names;
  int listed = 0;
  while (*p) {
    const char *end = strstr(p, "、");
    size_t n = end ? (size_t)(end - p) : strlen(p);
    if (n == strlen(name) && strncmp(p, name, n) == 0) return;  // 已经有了
    listed++;
    if (!end) break;
    p = end + strlen("、");
  }
  count++;
  if (listed >= 3) return;
  if (len) snprintf(names + len, size - len, "、%s", name);
  else friends_copy(names, size, name);
}

void friends_names_text(const char *names, int count, char *out, size_t size) {
  if (count > 3) snprintf(out, size, "%s等 %d 人", names, count);
  else friends_copy(out, size, names);
}

void friends_clean_url(const char *in, char *out, size_t size) {
  if (!size) return;
  out[0] = '\0';
  if (!in) return;
  while (*in == ' ' || *in == '\t' || *in == '\r' || *in == '\n') in++;
  size_t n = strlen(in);
  while (n > 0 && (in[n - 1] == ' ' || in[n - 1] == '\t' || in[n - 1] == '\r' || in[n - 1] == '\n' || in[n - 1] == '/'))
    n--;
  bool ok = (n > 8 && strncmp(in, "https://", 8) == 0) || (n > 7 && strncmp(in, "http://", 7) == 0);
  if (!ok || n >= size) return;
  for (size_t i = 0; i < n; i++) {
    if (in[i] == ' ') return;
  }
  memcpy(out, in, n);
  out[n] = '\0';
}
