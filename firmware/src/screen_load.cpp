#include "screen_load.h"

static constexpr uint32_t kSlideMs = 250;

// 相当于让正在播的切换动画立即播完：停掉所有屏幕上的动画、放回原位，目标屏幕直接显示
static void finish_pending_load(lv_disp_t *d) {
  lv_obj_t *pending = d->scr_to_load;
  if (!pending) return;
  // 被打断过的旧屏幕可能还在往外滑，所有屏幕都要停
  for (uint32_t i = 0; i < d->screen_cnt; i++) {
    lv_anim_del(d->screens[i], nullptr);
    lv_obj_set_pos(d->screens[i], 0, 0);
  }
  // 动画被删掉后不会再走 LVGL 的收尾回调，这几项要自己清掉
  d->prev_scr = nullptr;
  d->draw_prev_over_act = false;
  d->scr_to_load = nullptr;
  if (lv_scr_act() == pending) {
    lv_obj_invalidate(pending);
  } else {
    lv_scr_load(pending);  // 动画还没开始，没有动画时会直接切换
  }
}

void screen_load(lv_obj_t *scr, bool animate, bool back) {
  lv_disp_t *d = lv_obj_get_disp(scr);
  finish_pending_load(d);
  if (lv_scr_act() == scr) return;
  if (animate) {
    lv_scr_load_anim(scr, back ? LV_SCR_LOAD_ANIM_MOVE_RIGHT : LV_SCR_LOAD_ANIM_MOVE_LEFT, kSlideMs, 0, false);
  } else {
    lv_scr_load(scr);
  }
}
