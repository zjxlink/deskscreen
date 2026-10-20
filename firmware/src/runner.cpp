#include "runner.h"

static constexpr float kStartSpeed = 140;   // 像素/秒
static constexpr float kMaxSpeed = 330;
static constexpr float kAccel = 4;          // 每秒加快多少
static constexpr float kGravity = 1500;     // 像素/秒²
static constexpr float kJumpV = -520;       // 起跳速度，能跳约 90 像素高、0.7 秒
static constexpr uint32_t kRestartGuardMs = 600;
static constexpr int kHitMargin = 5;        // 碰撞框比画出来的小一点，擦边不算

uint32_t RunnerGame::rand_next() {
  seed_ = seed_ * 1664525u + 1013904223u;
  return seed_ >> 8;
}

void RunnerGame::start(uint32_t now_ms) {
  state_ = State::Playing;
  last_ms_ = now_ms;
  distance_ = 0;
  speed_ = kStartSpeed;
  cat_y_ = kGroundY - kCatSize;
  vy_ = 0;
  for (auto &o : obs_) o.active = false;
  next_gap_ = 120;  // 开局先跑一小段
}

void RunnerGame::tap(uint32_t now_ms) {
  switch (state_) {
    case State::Ready:
      start(now_ms);
      break;
    case State::Playing:
      if (on_ground()) vy_ = kJumpV;
      break;
    case State::Over:
      if (now_ms - over_at_ >= kRestartGuardMs) start(now_ms);
      break;
  }
}

void RunnerGame::spawn() {
  for (auto &o : obs_) {
    if (o.active) continue;
    o.active = true;
    o.x = kWidth;
    int kind = rand_next() % 3;  // 小黄瓜、长黄瓜、两根并排
    o.w = kind == 2 ? 30 : 14;
    o.h = kind == 1 ? 36 : 24;
    break;
  }
  // 间隔随速度变大，保证落地后来得及再跳：至少是一次跳跃的距离再加一点
  float jump_len = speed_ * 0.75f;
  next_gap_ = jump_len + 40 + rand_next() % 160;
}

void RunnerGame::step(float dt) {
  speed_ += kAccel * dt;
  if (speed_ > kMaxSpeed) speed_ = kMaxSpeed;
  float dx = speed_ * dt;
  distance_ += dx;

  vy_ += kGravity * dt;
  cat_y_ += vy_ * dt;
  if (cat_y_ >= kGroundY - kCatSize) {
    cat_y_ = kGroundY - kCatSize;
    vy_ = 0;
  }

  next_gap_ -= dx;
  if (next_gap_ <= 0) spawn();

  int cx1 = kCatX + kHitMargin, cx2 = kCatX + kCatSize - kHitMargin;
  int cy2 = (int)cat_y_ + kCatSize - kHitMargin;
  for (auto &o : obs_) {
    if (!o.active) continue;
    o.x -= dx;
    if (o.x + o.w < 0) {
      o.active = false;
      continue;
    }
    int ox1 = (int)o.x + 2, ox2 = (int)o.x + o.w - 2, oy1 = kGroundY - o.h + 2;
    if (cx2 > ox1 && cx1 < ox2 && cy2 > oy1) {
      state_ = State::Over;
      return;
    }
  }
}

void RunnerGame::tick(uint32_t now_ms) {
  if (state_ != State::Playing) return;
  uint32_t elapsed = now_ms - last_ms_;
  last_ms_ = now_ms;
  if (elapsed > 200) elapsed = 200;  // 卡顿太久就当只过了 0.2 秒，免得一下穿过去
  while (elapsed > 0 && state_ == State::Playing) {
    uint32_t ms = elapsed > 10 ? 10 : elapsed;
    step(ms / 1000.0f);
    elapsed -= ms;
  }
  if (state_ == State::Over) over_at_ = now_ms;
}
