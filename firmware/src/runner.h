#pragma once

#include <stdint.h>

// 小橘跑酷：点屏幕跳起来，躲开地上的黄瓜（猫最怕黄瓜）。越跑越快，分数是跑了多远。
// 不依赖硬件，时间由调用方传入，可在电脑上测试。坐标单位是屏幕像素，y 向下

class RunnerGame {
 public:
  static constexpr int kWidth = 320;
  static constexpr int kGroundY = 196;  // 地面的 y
  static constexpr int kCatX = 36;      // 小橘的左边
  static constexpr int kCatSize = 32;
  static constexpr int kMaxObstacles = 4;

  enum class State { Ready, Playing, Over };

  struct Obstacle {
    bool active = false;
    float x = 0;
    int w = 0;
    int h = 0;
  };

  explicit RunnerGame(uint32_t seed = 1) : seed_(seed ? seed : 1) {}

  // 点一下：没开始时开始；跑的时候跳；结束后（过 0.6 秒，免得连点误触）重新开始
  void tap(uint32_t now_ms);

  // 推进 dt 毫秒（主循环每次调用，时间长了会拆成小步）
  void tick(uint32_t now_ms);

  State state() const { return state_; }
  int score() const { return (int)(distance_ / 10); }
  float distance() const { return distance_; }
  float speed() const { return speed_; }      // 像素/秒
  float cat_y() const { return cat_y_; }      // 小橘的上边
  bool on_ground() const { return cat_y_ >= kGroundY - kCatSize; }
  const Obstacle &obstacle(int i) const { return obs_[i]; }

 private:
  void start(uint32_t now_ms);
  void step(float dt);
  void spawn();
  uint32_t rand_next();

  State state_ = State::Ready;
  uint32_t seed_;
  uint32_t last_ms_ = 0;
  uint32_t over_at_ = 0;
  float distance_ = 0;
  float speed_ = 0;
  float cat_y_ = kGroundY - kCatSize;
  float vy_ = 0;
  float next_gap_ = 0;  // 再跑多远出下一个黄瓜
  Obstacle obs_[kMaxObstacles];
};
