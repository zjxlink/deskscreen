#pragma once

#include <stdint.h>

// 屏幕控制芯片看门狗。
// 控制芯片偶尔会被供电抖动或静电干扰：整个复位后回到睡眠、关显示的状态，背光还亮着，屏幕就是一片白；
// 只丢了部分设置（像素格式、扫描方向）时画面会错位、重复、出现白条，也就是花屏。
// 单片机本身照常运行，不会自己发现。这里定时读一次芯片的电源状态、扫描方向和像素格式寄存器
// （RDDPM 0Ah、RDDMADCTL 0Bh、RDDCOLMOD 0Ch），拼成一个读数，
// 和开机时的读数对不上就重新初始化屏幕并整屏重画。
// 有的屏读不出状态（读数全 0、全 1 或不稳定），就退回到每隔一段时间盲目重新初始化一次。
// 不依赖硬件，读数和时间由调用方传入，可在电脑上测试。
class PanelWatch {
 public:
  static constexpr uint32_t kCheckMs = 2000;           // 多久读一次状态
  static constexpr uint32_t kBlindReinitMs = 10 * 60000;  // 读不出状态时多久重新初始化一次
  static constexpr int kBadReads = 2;                  // 连续几次读数不对才算真的被复位，避免偶尔读错
  static constexpr int kMaxFailedRecoveries = 3;       // 重新初始化后读数还是不对，几次后改成盲目模式

  // 屏幕刚初始化完调用，传入连续两次读数，作为正常状态的基准
  void begin(uint32_t first, uint32_t second, uint32_t now_ms);

  // 是否该检查了；为 true 时调用方读一次状态（读不出状态时不用读），再调用 check
  bool due(uint32_t now_ms) const { return now_ms - last_check_ms_ >= kCheckMs; }

  // 返回 true 表示需要重新初始化屏幕；初始化后调用 recovered
  bool check(uint32_t sig, uint32_t now_ms);

  // 重新初始化屏幕后调用，传入初始化后的读数（读不出状态时传什么都行）
  void recovered(uint32_t sig, uint32_t now_ms);

  bool readable() const { return readable_; }
  uint32_t baseline() const { return baseline_; }
  // 开机以来检测到屏幕被复位并恢复的次数（盲目模式的定时初始化不算）
  uint32_t recoveries() const { return recoveries_; }

 private:
  bool readable_ = false;
  uint32_t baseline_ = 0;
  int bad_reads_ = 0;
  int failed_recoveries_ = 0;
  uint32_t recoveries_ = 0;
  uint32_t last_check_ms_ = 0;
  uint32_t last_reinit_ms_ = 0;
};
