#include "panel_watch.h"

void PanelWatch::begin(uint32_t first, uint32_t second, uint32_t now_ms) {
  readable_ = first == second && first != 0 && first != 0xFFFFFF;
  baseline_ = first;
  bad_reads_ = 0;
  failed_recoveries_ = 0;
  last_check_ms_ = now_ms;
  last_reinit_ms_ = now_ms;
}

bool PanelWatch::check(uint32_t sig, uint32_t now_ms) {
  last_check_ms_ = now_ms;
  if (!readable_) return now_ms - last_reinit_ms_ >= kBlindReinitMs;
  if (sig == baseline_) {
    bad_reads_ = 0;
    return false;
  }
  return ++bad_reads_ >= kBadReads;
}

void PanelWatch::recovered(uint32_t sig, uint32_t now_ms) {
  last_check_ms_ = now_ms;
  last_reinit_ms_ = now_ms;
  bad_reads_ = 0;
  if (!readable_) return;
  recoveries_++;
  if (sig == baseline_) {
    failed_recoveries_ = 0;
  } else if (++failed_recoveries_ >= kMaxFailedRecoveries) {
    readable_ = false;  // 初始化了也对不上，说明读数本身不可信
  }
}
