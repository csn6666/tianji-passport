#pragma once

#include "lvgl.h"

// ── 深空鎏金调色板 ──
// 宏名沿用旧版(各页面零改动换肤),语义按新皮肤理解:
//   UI_SKY=全局底色  UI_PAPER=面板底  UI_INK=主文字/描边(鎏金)
#define UI_SKY        0x0B0E17   // 深空墨黑
#define UI_SKY_DARK   0x060810   // 更深的底(阴影/underlay)
#define UI_INK        0xD4AF37   // 鎏金
#define UI_PAPER      0x151B2E   // 夜幕面板
#define UI_GRASS      0x1F3A5F   // 玄青
#define UI_GRASS_DARK 0x142842
#define UI_YELLOW     0xF0C75E   // 亮金(选中/强调)
#define UI_ORANGE     0xE09B3D   // 暖金(状态提示)
#define UI_RED        0xE04A3F   // 朱砂(警示,暗底上调亮保证对比度)
#define UI_MUTED      0x2A3350   // 暗蓝灰面板

lv_obj_t *ui_pixel_screen_create(const char *title);
lv_obj_t *ui_pixel_panel_create(lv_obj_t *parent, int x, int y, int w, int h,
                                uint32_t color);
lv_obj_t *ui_pixel_label(lv_obj_t *parent, const char *text,
                         const lv_font_t *font, uint32_t color);
lv_obj_t *ui_pixel_mascot_create(lv_obj_t *parent, int x, int y);
void ui_pixel_mascot_jump(lv_obj_t *mascot);
void ui_pixel_set_selected(lv_obj_t *panel, bool selected, bool enabled);
