// main/ui_pixel.c —— "深空鎏金"皮肤:墨黑星空底、鎏金线框、印章角、像素小道童。
// 对外 API 与旧像素风版本完全一致,各页面零改动换肤。
#include "ui_pixel.h"

static void start_blink(lv_obj_t *eye);
static void start_twinkle(lv_obj_t *star, int delay_ms);

static lv_obj_t *block(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    return obj;
}

lv_obj_t *ui_pixel_label(lv_obj_t *parent, const char *text,
                         const lv_font_t *font, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    return label;
}

// 星空:固定坐标表(伪随机手排,避开标题区与主内容),3 颗接呼吸闪烁动画
static const struct { uint8_t x, y, s; } STARS[] = {
    { 18, 58, 1 },  { 47, 96, 1 },  { 76, 52, 2 },  { 108, 70, 1 },
    { 139, 55, 1 }, { 168, 88, 2 }, { 205, 60, 1 }, { 226, 100, 1 },
    { 12, 150, 1 }, { 230, 168, 1 },{ 8, 226, 1 },  { 228, 240, 2 },
    { 30, 290, 1 }, { 90, 302, 1 }, { 150, 296, 1 },{ 208, 288, 1 },
    { 60, 200, 1 }, { 185, 205, 1 },
};

lv_obj_t *ui_pixel_screen_create(const char *title)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(UI_SKY), 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    for (size_t i = 0; i < sizeof(STARS) / sizeof(STARS[0]); i++) {
        lv_obj_t *star = block(scr, STARS[i].x, STARS[i].y, STARS[i].s, STARS[i].s,
                               (i % 3 == 0) ? UI_YELLOW : 0xC8CFE8);
        if (i % 6 == 0) start_twinkle(star, (int)i * 350);
    }

    // 底部鎏金细线 + 两枚菱形缀点
    block(scr, 0, 314, 240, 1, 0x6B5A23);
    block(scr, 116, 312, 5, 5, UI_INK);
    block(scr, 118, 310, 1, 9, UI_SKY);   // 切出菱形效果的十字缺口

    // 标题:鎏金双线框
    lv_obj_t *plate = block(scr, 5, 8, 151, 33, UI_PAPER);
    lv_obj_set_style_border_color(plate, lv_color_hex(UI_INK), 0);
    lv_obj_set_style_border_width(plate, 2, 0);
    block(scr, 9, 12, 143, 1, 0x6B5A23);   // 内衬细线(上)
    block(scr, 9, 36, 143, 1, 0x6B5A23);   // 内衬细线(下)
    lv_obj_t *heading = ui_pixel_label(plate, title, &lv_font_montserrat_20, UI_YELLOW);
    lv_obj_center(heading);
    return scr;
}

lv_obj_t *ui_pixel_panel_create(lv_obj_t *parent, int x, int y, int w, int h,
                                uint32_t color)
{
    block(parent, x + 4, y + 5, w, h, UI_SKY_DARK);          // 影
    lv_obj_t *panel = block(parent, x, y, w, h, color);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x8A7429), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_pad_all(panel, 7, 0);
    // 四角印章框短线(亮金,盖在边框上)
    lv_obj_t *p = lv_obj_get_parent(panel);
    (void)p;
    static const int8_t cx[4] = { 0, 1, 0, 1 };   // 0=左 1=右
    static const int8_t cy[4] = { 0, 0, 1, 1 };   // 0=上 1=下
    for (int i = 0; i < 4; i++) {
        int bx = x + (cx[i] ? w - 7 : 0);
        int by = y + (cy[i] ? h - 2 : 0);
        block(parent, bx, by, 7, 2, UI_INK);                       // 横角线
        block(parent, x + (cx[i] ? w - 2 : 0),
              y + (cy[i] ? h - 7 : 0), 2, 7, UI_INK);              // 竖角线
    }
    return panel;
}

// ── 像素小道童:道髻、鎏金袍缘、玄青道袍、太极坠 ──
lv_obj_t *ui_pixel_mascot_create(lv_obj_t *parent, int x, int y)
{
    lv_obj_t *m = lv_obj_create(parent);
    lv_obj_remove_flag(m, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(m, x, y);
    lv_obj_set_size(m, 38, 48);
    lv_obj_set_style_bg_opa(m, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(m, 0, 0);
    lv_obj_set_style_pad_all(m, 0, 0);

    block(m, 16, 0, 6, 5, 0x2B2118);          // 发髻
    block(m, 15, 4, 8, 2, UI_INK);            // 金发带
    block(m, 10, 6, 18, 4, 0x2B2118);         // 刘海
    block(m, 9, 9, 20, 12, 0xF0D5B0);         // 脸
    lv_obj_t *left_eye = block(m, 13, 13, 3, 4, 0x2B2118);
    lv_obj_t *right_eye = block(m, 22, 13, 3, 4, 0x2B2118);
    block(m, 17, 18, 4, 1, 0xC98B6B);         // 嘴
    block(m, 7, 21, 24, 3, UI_INK);           // 金衣领
    block(m, 6, 24, 26, 16, 0x1F3A5F);        // 玄青道袍
    block(m, 6, 24, 3, 16, UI_INK);           // 左襟金缘
    block(m, 29, 24, 3, 16, UI_INK);          // 右襟金缘
    block(m, 16, 28, 6, 6, 0xE8E3D3);         // 太极坠(白半)
    block(m, 16, 31, 6, 3, 0x2B2118);         // 太极坠(黑半)
    block(m, 2, 26, 4, 9, 0xF0D5B0);          // 左手
    block(m, 32, 26, 4, 9, 0xF0D5B0);         // 右手
    block(m, 9, 40, 8, 5, 0x2B2118);          // 左靴
    block(m, 21, 40, 8, 5, 0x2B2118);         // 右靴
    start_blink(left_eye);
    start_blink(right_eye);
    return m;
}

static void jump_y(void *obj, int32_t value)
{
    lv_obj_set_y((lv_obj_t *)obj, value);
}

static void set_opa(void *obj, int32_t value)
{
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)value, 0);
}

static void start_blink(lv_obj_t *eye)
{
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, eye);
    lv_anim_set_exec_cb(&anim, set_opa);
    lv_anim_set_values(&anim, LV_OPA_COVER, LV_OPA_20);
    lv_anim_set_duration(&anim, 70);
    lv_anim_set_playback_duration(&anim, 70);
    lv_anim_set_repeat_delay(&anim, 1700);
    lv_anim_set_repeat_count(&anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&anim, lv_anim_path_step);
    lv_anim_start(&anim);
}

// 星星呼吸闪烁(错开 delay,避免整片同频)
static void start_twinkle(lv_obj_t *star, int delay_ms)
{
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, star);
    lv_anim_set_exec_cb(&anim, set_opa);
    lv_anim_set_values(&anim, LV_OPA_COVER, LV_OPA_30);
    lv_anim_set_duration(&anim, 900);
    lv_anim_set_playback_duration(&anim, 900);
    lv_anim_set_delay(&anim, delay_ms);
    lv_anim_set_repeat_delay(&anim, 400);
    lv_anim_set_repeat_count(&anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_in_out);
    lv_anim_start(&anim);
}

void ui_pixel_mascot_jump(lv_obj_t *mascot)
{
    if (!mascot) return;
    int y = lv_obj_get_y(mascot);
    lv_anim_delete(mascot, jump_y);
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, mascot);
    lv_anim_set_exec_cb(&anim, jump_y);
    lv_anim_set_values(&anim, y, y - 5);
    lv_anim_set_duration(&anim, 110);
    lv_anim_set_playback_duration(&anim, 140);
    lv_anim_set_path_cb(&anim, lv_anim_path_step);
    lv_anim_start(&anim);
}

void ui_pixel_set_selected(lv_obj_t *panel, bool selected, bool enabled)
{
    uint32_t bg = !enabled ? 0x1A1F30 : (selected ? 0x1F2A4A : UI_PAPER);
    uint32_t border = !enabled ? 0x3A4258 : (selected ? UI_YELLOW : 0x8A7429);
    lv_obj_set_style_bg_color(panel, lv_color_hex(bg), 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(border), 0);
    lv_obj_set_style_border_width(panel, selected ? 2 : 1, 0);
}
