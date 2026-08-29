// main/demo_fortune.c —— "神算"页:语音录入生辰八字,云端确定性排盘,
// 显示今日幸运色/数字/方位/宜忌与一句话解盘。深空鎏金皮肤的主秀场。
//
// 交互:
//   首次进入 -> 语音录入向导(说生日 -> 确认 -> 选男女命)
//   今日页   OK 短按=切到命盘页;命盘页 OK 短按=切回
//   上键长按 = 清除八字重新录入;确定长按 = 返回菜单(main.c 拦截)
//
// 排盘:全部在端侧算(bazi_engine.c + 烘好的节气/朔日表),断网也能看命盘与今日。
// 网络只用于:语音识别(录生辰/问事)、一句话解盘文案、SNTP 授时。
// 设备无 RTC,日期靠 SNTP;未校到时前只显示四柱与大运,今日页提示校时中。
#include "demo.h"
#include "chat_config.h"
#include "chat_provision.h"
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bazi_engine.h"
#include "llm_client.h"
#include "bsp_display.h"
#include "ui_pixel.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "freertos/queue.h"
#include "freertos/queue.h"
#include "esp_websocket_client.h"
#include "esp_log.h"
#include "nvs.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>
#include <time.h>
#include "esp_timer.h"

static const char *TAG = "fortune";

LV_FONT_DECLARE(font_cn_16);
LV_FONT_DECLARE(font_mystic_28);

#define NVS_NS      "fortune"
#define CHUNK_BYTES 2048

typedef enum {
    FS_HOME = 0,       // 天机首页(功能菜单 + 生辰摘要)
    FS_PROVISION,      // 配网模式(首次使用,手机连热点)
    FS_WIZ_INTRO,      // 向导:提示按 OK 说生日
    FS_WIZ_LISTEN,     // 向导:录音中
    FS_WIZ_PARSE,      // 向导:等服务器解析
    FS_WIZ_CONFIRM,    // 向导:回显确认
    FS_WIZ_GENDER,     // 向导:选男/女命
    FS_WIZ_GEO_INTRO,  // 向导:提示说出生城市(真太阳时校准,可跳过)
    FS_WIZ_GEO_LISTEN, // 向导:录出生地
    FS_WIZ_GEO_PARSE,  // 向导:等地理编码
    FS_WIZ_GEO_CONFIRM,// 向导:确认城市与经度
    FS_TODAY,          // 今日运势页
    FS_CHART,          // 命盘页
    FS_ASK_IDLE,       // 问事页:待机
    FS_ASK_LISTEN,     // 问事页:录音中
    FS_ASK_THINKING,   // 问事页:等云端解盘
    FS_ASK_SPEAKING,   // 问事页:播放回答
    FS_SETTINGS,       // 设置页:电量 + Wi-Fi
    FS_SET_PROV,       // 设置页发起的重新配网(区别于开机首次配网)
    FS_ERROR,
} fs_state_t;

typedef struct {
    uint16_t year;
    uint8_t month, day, hour, minute;
    int32_t lng100;        // 出生地经度x100(东经正),默认 12000=东八区不校正
    bool hour_unknown;
    bool male;
    bool valid;
} birth_t;

static fs_state_t s_state;
static bool s_active;
static birth_t s_birth;
static birth_t s_pending;              // 向导解析出的待确认生日
static bool s_pending_male = true;
static int32_t s_pending_lng100 = 12000;   // 向导中待确认的出生地经度
static char s_geo_city[48];
static char s_birth_city[48];              // 已保存的出生城市(展示用)
static int s_home_sel;                     // 首页当前选中项
static lv_obj_t *s_home_cards[5];
static esp_websocket_client_handle_t s_ws;
static TaskHandle_t s_rec_task, s_net_task, s_play_task;
static RingbufHandle_t s_play_rb;      // 问事回答的 TTS(ADPCM 压缩)播放缓冲
// 播放环只做抖动缓冲(12KB ≈ 1.5 秒音频),不再试图装下整句 ——
// 一句话的音频动辄 30KB,本来也装不下,靠逐句流控 + TCP 背压匀速就够了。
// 缩小之后它可以在 TLS 期间一直挂着,于是语音能边出字边播。
#define PLAY_RB_BYTES (12 * 1024)

// 句子队列:LLM 一边流,凑齐一句就丢进来,由 speak_task 去合成。
// 不在 SSE 回调里直接发/等 —— 那会卡住 HTTP 读取,把 TLS 连接拖到超时。
#define SAY_Q_LEN 4
#define SAY_MAX   288
static QueueHandle_t s_say_q;
static TaskHandle_t s_speak_task;
static volatile bool s_say_eof;        // LLM 已结束,队列排空即收尾
static volatile int  s_say_pending;    // 已入队未播完的句子数
static volatile bool s_more_speech;    // 还有话要说,别让播放提前收尾
static char s_sent_buf[SAY_MAX];       // 正在攒的半句
static size_t s_sent_len;
static bool s_tts_done;
static bz_chart_t s_chart;             // 端侧算出来的命盘(唯一数据源)
static TaskHandle_t s_llm_task;        // 直连 LLM 的任务(解盘文案/问事共用,同时只跑一个)
static char s_llm_question[192];       // 待问的问题(问事)
static bool s_llm_is_caption;          // 本轮是要一句话文案还是问事回答
static volatile bool s_llm_abort;      // 用户打断
static char s_llm_answer[1024];        // 累积的完整回答
static size_t s_llm_answer_len;
static bool s_chart_ok;
static bool s_time_ok;                 // SNTP 是否已校时(未校时则流年/今日不可信)
static char s_caption[96];             // 一句话解盘(LLM 写的,联网时才有)
static int32_t s_caption_day;          // 该文案对应的日期 YYYYMMDD,隔天作废

static lv_obj_t *s_scr, *s_content, *s_status;
static lv_obj_t *s_ask_q, *s_ask_a;    // 问事页:问题/回答文本
static lv_obj_t *s_ask_panel;          // 回答滚动容器
static lv_obj_t *s_set_soc, *s_set_bar, *s_set_st, *s_set_de;   // 设置页待刷新控件
static lv_timer_t *s_tick;             // 设置页轮询:刷电量/等配网结果
static bool s_prov_saved;              // 配网页刚写入新凭据,等连上就回设置页
static uint32_t s_prov_gen;            // 进配网页时的联网代数,变了才算换网成功

// ---------------------------------------------------------------- NVS

static void birth_load(void) {
    memset(&s_birth, 0, sizeof(s_birth));
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        if (err != ESP_ERR_NVS_NOT_FOUND) ESP_LOGW(TAG, "读生辰失败: %s", esp_err_to_name(err));
        return;
    }
    uint16_t y = 0;
    uint8_t v = 0;
    if (nvs_get_u16(h, "y", &y) == ESP_OK && y) {
        s_birth.year = y;
        nvs_get_u8(h, "mo", &s_birth.month);
        nvs_get_u8(h, "d", &s_birth.day);
        nvs_get_u8(h, "h", &s_birth.hour);
        nvs_get_u8(h, "mi", &s_birth.minute);
        if (nvs_get_u8(h, "hu", &v) == ESP_OK) s_birth.hour_unknown = v;
        if (nvs_get_u8(h, "male", &v) == ESP_OK) s_birth.male = v;
        s_birth.lng100 = 12000;
        nvs_get_i32(h, "lng", &s_birth.lng100);
        size_t cl = sizeof(s_birth_city);
        if (nvs_get_str(h, "city", s_birth_city, &cl) != ESP_OK) s_birth_city[0] = '\0';
        s_birth.valid = true;
        ESP_LOGI(TAG, "读到生辰 %04d-%02d-%02d %02d:%02d %s %s",
                 s_birth.year, s_birth.month, s_birth.day,
                 s_birth.hour, s_birth.minute,
                 s_birth.hour_unknown ? "(时辰未知)" : "",
                 s_birth.male ? "男" : "女");
    }
    nvs_close(h);
}

static void birth_save(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u16(h, "y", s_birth.year);
    nvs_set_u8(h, "mo", s_birth.month);
    nvs_set_u8(h, "d", s_birth.day);
    nvs_set_u8(h, "h", s_birth.hour);
    nvs_set_u8(h, "mi", s_birth.minute);
    nvs_set_u8(h, "hu", s_birth.hour_unknown);
    nvs_set_u8(h, "male", s_birth.male);
    nvs_set_i32(h, "lng", s_birth.lng100);
    nvs_set_str(h, "city", s_birth_city);
    nvs_commit(h);
    nvs_close(h);
}

static void caption_save(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "cap", s_caption);
    nvs_set_i32(h, "capday", s_caption_day);
    nvs_commit(h);
    nvs_close(h);
}

static void caption_load(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t len = sizeof(s_caption);
    if (nvs_get_str(h, "cap", s_caption, &len) != ESP_OK) s_caption[0] = '\0';
    if (nvs_get_i32(h, "capday", &s_caption_day) != ESP_OK) s_caption_day = 0;
    nvs_close(h);
}

static void fortune_clear_all(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    s_birth.valid = false;
}

// ---------------------------------------------------------------- 端侧排盘

// 用当前时间重排一次。未校时的话仍能排出四柱与大运(它们只跟生辰有关),
// 只是流年/流月/流日与今日运势不可信,由 s_time_ok 决定要不要显示。
static bool compute_chart(void) {
    if (!s_birth.valid) { s_chart_ok = false; return false; }

    s_time_ok = chat_net_time_ok();
    struct tm tm;
    if (s_time_ok) {
        time_t now = time(NULL);
        localtime_r(&now, &tm);
    } else {
        memset(&tm, 0, sizeof(tm));      // 占位:只为把四柱/大运算出来
        tm.tm_year = 2026 - 1900;
        tm.tm_mon = 0;
        tm.tm_mday = 1;
    }

    bz_birth_t b = {
        .year = s_birth.year, .month = s_birth.month, .day = s_birth.day,
        .hour = s_birth.hour, .minute = s_birth.minute,
        .hour_unknown = s_birth.hour_unknown, .male = s_birth.male,
        .lng100 = s_birth.lng100,
    };
    s_chart_ok = bz_compute(&b, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                            tm.tm_hour, tm.tm_min, &s_chart);
    if (s_chart_ok) {
        ESP_LOGI(TAG, "端侧排盘: %s 日主%s 大运%s%s", s_chart.eight_char,
                 s_chart.day_master, s_chart.current_dayun,
                 s_time_ok ? "" : " (未校时)");
    } else {
        ESP_LOGW(TAG, "排盘失败:生辰超出 1900-2100");
    }
    return s_chart_ok;
}

// 今日的 YYYYMMDD,用来判断缓存的解盘文案是不是隔夜了
static int32_t today_key(void) {
    if (!s_time_ok) return 0;
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    return (tm.tm_year + 1900) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday;
}

// 没联网拿不到 LLM 文案时的兜底:纯确定性,由排好的数据拼
static void caption_fallback(char *buf, size_t n) {
    snprintf(buf, n, "%s日·五行%s喜神·宜%s", s_chart.liuri,
             s_chart.liuri_relation, s_chart.yi[0][0] ? s_chart.yi[0] : "静守");
}

// ---------------------------------------------------------------- UI 骨架

static void ui_status(const char *text, uint32_t color) {
    if (!bsp_lvgl_lock(200)) return;
    if (s_status) {
        lv_label_set_text(s_status, text);
        lv_obj_set_style_text_color(s_status, lv_color_hex(color), 0);
    }
    bsp_lvgl_unlock();
}

// 清空内容区(标题栏与底部状态栏之间整块重建)
static lv_obj_t *content_reset(void) {
    s_ask_q = NULL;                    // 旧视图的子控件即将销毁,防悬垂
    s_ask_a = NULL;
    s_ask_panel = NULL;
    s_set_soc = NULL;
    s_set_bar = NULL;
    s_set_st = NULL;
    s_set_de = NULL;
    if (s_content) lv_obj_delete(s_content);
    s_content = lv_obj_create(s_scr);
    lv_obj_remove_flag(s_content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_content, 0, 46);
    lv_obj_set_size(s_content, 240, 244);   // 46..290,底部让给状态栏(292 起)
    lv_obj_set_style_bg_opa(s_content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_content, 0, 0);
    lv_obj_set_style_pad_all(s_content, 0, 0);
    return s_content;
}

static lv_obj_t *cn_label(lv_obj_t *p, const char *t, int x, int y, uint32_t c) {
    lv_obj_t *l = ui_pixel_label(p, t, &font_cn_16, c);
    lv_obj_set_pos(l, x, y);
    return l;
}

static lv_obj_t *big_label(lv_obj_t *p, const char *t, int x, int y, uint32_t c) {
    lv_obj_t *l = ui_pixel_label(p, t, &font_mystic_28, c);
    lv_obj_set_pos(l, x, y);
    return l;
}

// ---------------------------------------------------------------- 首页

static void view_wizard(const char *line1, const char *line2, uint32_t c1);
static void view_panel(const char *title, const char *line1, const char *line2,
                       uint32_t c1);
static void render_fortune(fs_state_t view);
static bool llm_start(bool caption, const char *question);
static void say_enqueue(const char *sent);

#define HOME_N 5
static const char *HOME_ITEMS[HOME_N] = { "我的命盘", "今日运势", "问事",
                                          "重录命盘", "设置" };

static void home_refresh(void) {
    for (int i = 0; i < HOME_N; i++) {
        if (s_home_cards[i]) ui_pixel_set_selected(s_home_cards[i], i == s_home_sel, true);
    }
}

static void view_home(void) {
    if (!bsp_lvgl_lock(300)) return;
    lv_obj_t *cont = content_reset();
    for (int i = 0; i < HOME_N; i++) s_home_cards[i] = NULL;

    // 生辰摘要卡
    lv_obj_t *p = ui_pixel_panel_create(cont, 12, 2, 216, 52, UI_MUTED);
    char l1[96], l2[96];
    if (s_birth.valid) {
        char tm[24];
        if (s_birth.hour_unknown) snprintf(tm, sizeof(tm), "时辰未知");
        else snprintf(tm, sizeof(tm), "%02u:%02u", s_birth.hour, s_birth.minute);
        snprintf(l1, sizeof(l1), "%s %u年%u月%u日 %s",
                 s_birth.male ? "乾造" : "坤造",
                 s_birth.year, s_birth.month, s_birth.day, tm);
        if (s_birth_city[0])
            snprintf(l2, sizeof(l2), "生于 %s (东经%d.%02d°)", s_birth_city,
                     (int)(s_birth.lng100 / 100), (int)(s_birth.lng100 % 100));
        else
            snprintf(l2, sizeof(l2), "出生地未录·按东八区推算");
    } else {
        snprintf(l1, sizeof(l1), "尚未录入生辰");
        snprintf(l2, sizeof(l2), "选任意功能开始录入");
    }
    lv_obj_t *a = ui_pixel_label(p, l1, &font_cn_16, UI_INK);
    lv_obj_set_pos(a, 2, 2);
    lv_obj_t *b = ui_pixel_label(p, l2, &font_cn_16, 0x9FB8E8);
    lv_obj_set_pos(b, 2, 22);

    // 功能菜单:5 项在 244px 内容区里排满(60..236,末项影子到 241)
    for (int i = 0; i < HOME_N; i++) {
        s_home_cards[i] = ui_pixel_panel_create(cont, 12, 60 + i * 36, 216, 32, UI_PAPER);
        lv_obj_t *t = ui_pixel_label(s_home_cards[i], HOME_ITEMS[i], &font_cn_16, UI_INK);
        lv_obj_center(t);
    }
    home_refresh();
    bsp_lvgl_unlock();
    ui_status(chat_net_ready() ? "上下选择 OK进入" : "离线·仅可看缓存", 0x8A7429);
    s_state = FS_HOME;
}

// ---------------------------------------------------------------- 设置页

// 无边框实心色块(电量条用;面板/标签走 ui_pixel 那套,这里只要一条纯色)
static lv_obj_t *bar_block(lv_obj_t *parent, int x, int y, int w, int h, uint32_t c) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(c), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    return o;
}

#define BATT_BAR_W 202                 // 面板内宽 216-2*7

static uint32_t batt_color(int soc) {
    if (soc < 20) return UI_RED;
    if (soc < 50) return UI_ORANGE;
    return UI_YELLOW;
}

// 把当前电量/Wi-Fi 状态写进设置页控件。调用方需持 LVGL 锁。
static void settings_paint(void) {
    if (!s_set_soc) return;

    int soc = bsp_battery_soc();
    int mv = bsp_battery_mv();
    char buf[96];
    if (soc < 0) {
        snprintf(buf, sizeof(buf), "电量计未就绪");
    } else if (mv > 0) {
        snprintf(buf, sizeof(buf), "%d%%   %d.%02dV", soc, mv / 1000, (mv % 1000) / 10);
    } else {
        snprintf(buf, sizeof(buf), "%d%%", soc);
    }
    lv_label_set_text(s_set_soc, buf);
    lv_obj_set_style_text_color(s_set_soc,
                                lv_color_hex(soc < 0 ? 0x8A7429 : batt_color(soc)), 0);
    if (s_set_bar) {
        int w = soc < 0 ? 0 : soc * BATT_BAR_W / 100;
        lv_obj_set_width(s_set_bar, w < 1 ? 1 : w);
        lv_obj_set_style_bg_color(s_set_bar, lv_color_hex(batt_color(soc < 0 ? 0 : soc)), 0);
        lv_obj_set_style_bg_opa(s_set_bar, soc < 0 ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
    }

    char ssid[33] = { 0 }, pass[65] = { 0 };
    bool has = chat_prov_get(ssid, sizeof(ssid), pass, sizeof(pass));
    bool up = chat_net_ready();
    lv_label_set_text(s_set_st, !has ? "未配网" : (up ? "已连接" : "连接中..."));
    lv_obj_set_style_text_color(s_set_st,
                                lv_color_hex(!has ? UI_RED : (up ? UI_YELLOW : UI_ORANGE)), 0);

    char ip[16];
    char det[160];
    if (!has) {
        snprintf(det, sizeof(det), "尚未设置 Wi-Fi\n按 OK 用手机配网");
    } else {
        snprintf(det, sizeof(det), "网络  %s\nIP    %s\n信号  %ddBm",
                 ssid, chat_net_ip(ip, sizeof(ip)) ? ip : "—", chat_net_rssi());
    }
    lv_label_set_text(s_set_de, det);
}

static void view_settings(void) {
    if (!bsp_lvgl_lock(300)) return;
    lv_obj_t *cont = content_reset();
    lv_obj_t *t = ui_pixel_label(cont, "设置", &font_mystic_28, UI_YELLOW);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 4);

    // 电量
    lv_obj_t *pb = ui_pixel_panel_create(cont, 12, 46, 216, 62, UI_PAPER);
    cn_label(pb, "剩余电量", 2, 2, UI_INK);
    s_set_soc = ui_pixel_label(pb, "", &font_cn_16, UI_YELLOW);
    lv_obj_align(s_set_soc, LV_ALIGN_TOP_RIGHT, -2, 2);
    bar_block(pb, 2, 26, BATT_BAR_W, 12, UI_GRASS_DARK);       // 槽
    s_set_bar = bar_block(pb, 2, 26, 1, 12, UI_YELLOW);        // 填充(宽度随电量)

    // Wi-Fi
    lv_obj_t *pw = ui_pixel_panel_create(cont, 12, 120, 216, 100, UI_PAPER);
    cn_label(pw, "Wi-Fi", 2, 2, UI_INK);
    s_set_st = ui_pixel_label(pw, "", &font_cn_16, UI_YELLOW);
    lv_obj_align(s_set_st, LV_ALIGN_TOP_RIGHT, -2, 2);
    s_set_de = ui_pixel_label(pw, "", &font_cn_16, 0x9FB8E8);
    lv_obj_set_width(s_set_de, BATT_BAR_W);
    lv_label_set_long_mode(s_set_de, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(s_set_de, 2, 24);

    settings_paint();
    bsp_lvgl_unlock();
    s_state = FS_SETTINGS;
    ui_status(chat_prov_has_creds() ? "OK重新配网  长按OK返回"
                                    : "OK开始配网  长按OK返回", 0x8A7429);
}

// 开热点让手机改 Wi-Fi。已联网时旧连接保持到用户提交新凭据为止。
static void settings_start_prov(void) {
    if (chat_prov_start() != ESP_OK) {
        ui_status("配网启动失败", UI_RED);
        return;
    }
    s_prov_saved = false;
    s_prov_gen = chat_net_gen();
    view_panel("配网",
               "1. 手机连热点\n   Tianji-Setup\n2. 自动弹出配置页\n   (没弹就开 192.168.4.1)\n3. 填 Wi-Fi 名称密码",
               "连上后自动返回设置\n长按OK放弃", UI_INK);
    s_state = FS_SET_PROV;
    ui_status("等待手机配网", UI_YELLOW);
}

// 2s 一跳:设置页刷电量/信号;配网页等新凭据连上后自动回设置页
static void settings_tick(lv_timer_t *t) {
    (void)t;
    if (!s_active) return;

    // SNTP 校到时之后补排一次:流年/流月/今日运势这时才有意义
    if (!s_time_ok && s_birth.valid && chat_net_time_ok()) {
        if (compute_chart()) {
            if (s_state == FS_TODAY) render_fortune(FS_TODAY);
            else if (s_state == FS_CHART) render_fortune(FS_CHART);
            if (s_caption_day != today_key()) llm_start(true, NULL);
        }
    }

    if (s_state == FS_SETTINGS) {
        settings_paint();
    } else if (s_state == FS_SET_PROV && s_prov_saved && chat_net_ready() &&
               chat_net_gen() != s_prov_gen) {
        s_prov_saved = false;
        chat_prov_stop();
        view_settings();
        ui_status("已连上新网络", UI_YELLOW);
    }
}

// 进入录入向导(不清旧数据,完成保存时才覆盖)
static void start_wizard(void) {
    view_wizard("对我说出你的出生日期和时间\n\n例如:\n1998年3月15日早上7点半\n(说农历也可以)",
                "按OK开始说话", UI_INK);
    s_state = FS_WIZ_INTRO;
    ui_status("录入生辰", UI_YELLOW);
}

// ---------------------------------------------------------------- 向导视图

// 通用整屏提示页:居中大标题 + 一段正文 + 钉底副文案
static void view_panel(const char *title, const char *line1, const char *line2,
                       uint32_t c1) {
    if (!bsp_lvgl_lock(300)) return;
    lv_obj_t *cont = content_reset();
    lv_obj_t *t = ui_pixel_label(cont, title, &font_mystic_28, UI_YELLOW);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 4);
    lv_obj_t *p = ui_pixel_panel_create(cont, 12, 52, 216, 186, UI_PAPER);
    lv_obj_t *l1 = ui_pixel_label(p, line1, &font_cn_16, c1);
    lv_obj_set_width(l1, 196);
    lv_label_set_long_mode(l1, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(l1, 2, 2);
    if (line2) {
        // 钉在面板底部,与上方正文永不相交
        lv_obj_t *l2 = ui_pixel_label(p, line2, &font_cn_16, 0x9FB8E8);
        lv_obj_set_width(l2, 196);
        lv_label_set_long_mode(l2, LV_LABEL_LONG_WRAP);
        lv_obj_align(l2, LV_ALIGN_BOTTOM_LEFT, 2, -2);
    }
    bsp_lvgl_unlock();
}

static void view_wizard(const char *line1, const char *line2, uint32_t c1) {
    view_panel("生辰八字", line1, line2, c1);
}

static void view_gender(void) {
    if (!bsp_lvgl_lock(300)) return;
    lv_obj_t *cont = content_reset();
    big_label(cont, "命主性别", 62, 8, UI_YELLOW);
    cn_label(cont, "大运顺逆依此而定", 62, 46, 0x9FB8E8);
    lv_obj_t *p = ui_pixel_panel_create(cont, 46, 84, 148, 96, UI_PAPER);
    lv_obj_t *sel = ui_pixel_label(p, s_pending_male ? "· 男命 ·" : "· 女命 ·",
                                   &font_mystic_28, UI_YELLOW);
    lv_obj_align(sel, LV_ALIGN_TOP_MID, 0, 4);
    lv_obj_t *hint = ui_pixel_label(p, "上下切换 OK确认", &font_cn_16, 0x8A7429);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -2);
    bsp_lvgl_unlock();
}

// ---------------------------------------------------------------- 问事:音频播放
// 与 demo_chat 同一套 IMA-ADPCM 协议(4:1 压缩,高半字节在前),按轮重置解码状态。

static const int8_t FIMA_IDX[16] = { -1, -1, -1, -1, 2, 4, 6, 8,
                                     -1, -1, -1, -1, 2, 4, 6, 8 };
static const int16_t FIMA_STEP[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
    34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
    157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544,
    598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878,
    2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894,
    6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818,
    18500, 20350, 22385, 24623, 27086, 29794, 32767 };
static int s_adp_val, s_adp_idx;

static void adpcm_reset(void) { s_adp_val = 0; s_adp_idx = 0; }

static int16_t adpcm_nibble(uint8_t n) {
    int step = FIMA_STEP[s_adp_idx];
    int diff = step >> 3;
    if (n & 4) diff += step;
    if (n & 2) diff += step >> 1;
    if (n & 1) diff += step >> 2;
    s_adp_val += (n & 8) ? -diff : diff;
    if (s_adp_val > 32767) s_adp_val = 32767;
    else if (s_adp_val < -32768) s_adp_val = -32768;
    s_adp_idx += FIMA_IDX[n];
    if (s_adp_idx < 0) s_adp_idx = 0;
    else if (s_adp_idx > 88) s_adp_idx = 88;
    return (int16_t)s_adp_val;
}

static void play_rb_flush(void) {
    size_t got;
    void *p;
    while ((p = xRingbufferReceiveUpTo(s_play_rb, &got, 0, 1024)) != NULL) {
        vRingbufferReturnItem(s_play_rb, p);
    }
}

static void play_task(void *arg) {
    bool primed = false;
    static int16_t pcm[2048];
    while (s_active) {
        UBaseType_t waiting = 0;
        vRingbufferGetInfo(s_play_rb, NULL, NULL, NULL, NULL, &waiting);
        if (!primed) {
            if (waiting >= 6144 || (s_tts_done && waiting > 0)) {
                primed = true;
            } else if (s_state == FS_ASK_SPEAKING && s_tts_done && waiting == 0 &&
                       !s_more_speech) {
                s_tts_done = false;
                s_state = FS_ASK_IDLE;
                ui_status("OK提问 上下翻页", 0x8A7429);
            } else {
                vTaskDelay(pdMS_TO_TICKS(30));
            }
            continue;
        }
        size_t got = 0;
        uint8_t *chunk = xRingbufferReceiveUpTo(s_play_rb, &got, pdMS_TO_TICKS(100), 1024);
        if (chunk) {
            for (size_t i = 0; i < got; i++) {
                pcm[i * 2]     = adpcm_nibble(chunk[i] >> 4);
                pcm[i * 2 + 1] = adpcm_nibble(chunk[i] & 0x0F);
            }
            vRingbufferReturnItem(s_play_rb, chunk);
            bsp_audio_write(pcm, got * 4);
        } else {
            primed = false;
        }
    }
    s_play_task = NULL;
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------- 问事视图

static void view_ask(void) {
    if (!bsp_lvgl_lock(300)) return;
    lv_obj_t *cont = content_reset();
    big_label(cont, "问事", 88, 0, UI_YELLOW);

    lv_obj_t *pq = ui_pixel_panel_create(cont, 8, 42, 224, 58, UI_MUTED);
    s_ask_q = ui_pixel_label(pq, "", &font_cn_16, UI_INK);
    lv_obj_set_width(s_ask_q, 204);
    lv_label_set_long_mode(s_ask_q, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(s_ask_q, 2, 2);

    s_ask_panel = ui_pixel_panel_create(cont, 8, 110, 224, 132, UI_PAPER);
    lv_obj_add_flag(s_ask_panel, LV_OBJ_FLAG_SCROLLABLE);   // 长回答上下键翻页
    lv_obj_set_scroll_dir(s_ask_panel, LV_DIR_VER);
    s_ask_a = ui_pixel_label(s_ask_panel, "问命理、问运势、问宜忌\n均以你的命盘作答",
                             &font_cn_16, 0x9FB8E8);
    lv_obj_set_width(s_ask_a, 200);
    lv_label_set_long_mode(s_ask_a, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(s_ask_a, 0, 0);
    bsp_lvgl_unlock();
    ui_status("OK提问 上下翻页", 0x8A7429);
}

static void ask_append_answer(const char *text) {
    if (!bsp_lvgl_lock(200)) return;
    if (s_ask_a) {
        const char *old = lv_label_get_text(s_ask_a);
        size_t olen = strlen(old);
        if (olen > 1400) {                     // 超长时丢最旧的一半(对齐 UTF-8 边界)
            old += olen - 800;
            while ((*old & 0xC0) == 0x80) old++;
        }
        static char buf[1600];
        snprintf(buf, sizeof(buf), "%s%s", old, text);
        lv_label_set_text(s_ask_a, buf);
        lv_obj_set_style_text_color(s_ask_a, lv_color_hex(UI_INK), 0);
        if (s_ask_panel) {                     // 追加后自动滚到底
            lv_obj_update_layout(s_ask_panel);
            lv_obj_scroll_to_y(s_ask_panel, LV_COORD_MAX, LV_ANIM_OFF);
        }
    }
    bsp_lvgl_unlock();
}

// ---------------------------------------------------------------- 直连 LLM
//
// 排盘在本地算完,LLM 只做解读。人设与红线跟原来服务器上那份一致:
// 只准引用系统排好的数据,不准自己推算干支,不做医疗/寿命/重大决策断言。
//
// 内存:llm_chat 期间堆只剩 10KB 上下(见 llm_client.c 的说明),所以这里
// 先把整段回答收完、TLS 连接关掉,再把句子送去服务器合成语音 —— 不让
// TLS 和音频流同时在线。代价是语音比文字晚几秒,屏幕上仍是逐字出的。

static const char PERSONA[] =
    "你是一位温和笃定的八字命理师,在一个语音小设备上回答命主的问题。规则:"
    "1) 只依据下方系统排好的命盘数据作答,严禁自行推算任何干支、日期或大运;"
    "2) 口语化,不用 markdown、列表、表情;"
    "3) 控制在三到五句话;"
    "4) 不做健康、疾病、寿命、重大财务决策的断言;语气积极不宿命。";

// 把端侧算出来的命盘拼成给 LLM 的上下文
static void build_chart_context(char *buf, size_t n) {
    static const char *const WX[5] = { "金", "木", "水", "火", "土" };
    size_t u = 0;
    u += snprintf(buf + u, n - u,
                  "【命主命盘(系统已排好,直接引用)】\n"
                  "八字: %s\n日主: %s\n起运: %s\n当前大运: %s\n",
                  s_chart.eight_char, s_chart.day_master, s_chart.start_info,
                  s_chart.current_dayun[0] ? s_chart.current_dayun : "未起运");
    if (u < n) u += snprintf(buf + u, n - u,
                  "五行占比: 金%.1f 木%.1f 水%.1f 火%.1f 土%.1f(%s)\n喜用神: %s\n",
                  s_chart.wx_pct[0], s_chart.wx_pct[1], s_chart.wx_pct[2],
                  s_chart.wx_pct[3], s_chart.wx_pct[4],
                  s_chart.strong ? "身强" : "身弱", WX[s_chart.fav]);
    if (u < n && s_time_ok) snprintf(buf + u, n - u,
                  "今日: %s %s 流年%s 流月%s 流日%s 宜%s 忌%s\n",
                  s_chart.solar_date, s_chart.lunar_date, s_chart.liunian,
                  s_chart.liuyue, s_chart.liuri, s_chart.yi[0], s_chart.ji[0]);
}

// UTF-8 首字节 -> 这个字符占几个字节
static size_t utf8_clen(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;                                   // 非法首字节:当单字节吞掉,别卡死
}

// 一个完整字符是不是句末标点
static bool is_sentence_end(const char *p, size_t n) {
    if (n == 3 && p[0] == (char)0xE3 && p[1] == (char)0x80 &&
        (p[2] == (char)0x82 || p[2] == (char)0x81)) return true;   // 。 、
    if (n == 3 && p[0] == (char)0xEF && p[1] == (char)0xBC &&
        (p[2] == (char)0x81 || p[2] == (char)0x9F)) return true;   // ！ ？
    return n == 1 && (p[0] == '!' || p[0] == '?' || p[0] == '\n' || p[0] == ';');
}

// 把 s_sent_buf 的前 cut 字节送去合成,余下的挪到开头。
// cut 必须落在字符边界上 —— 切出半个汉字会让 WebSocket 文本帧变成非法 UTF-8,
// 协议层会直接判违规关掉连接(踩过一次,整条语音链就此哑掉)。
static void sentence_flush(size_t cut) {
    if (cut == 0 || cut > s_sent_len) return;
    char one[SAY_MAX];
    memcpy(one, s_sent_buf, cut);
    one[cut] = '\0';
    say_enqueue(one);
    s_sent_len -= cut;
    memmove(s_sent_buf, s_sent_buf + cut, s_sent_len);
    s_sent_buf[s_sent_len] = '\0';
}

// 增量回调:累积全文、刷屏,并且凑够一句就立刻送去合成 ——
// 语音因此能跟着文字一起出,而不是等整段说完。
static bool llm_on_delta(const char *text, void *user) {
    (void)user;
    if (s_llm_abort || !s_active) return false;
    size_t len = strlen(text);
    if (s_llm_answer_len + len < sizeof(s_llm_answer)) {
        memcpy(s_llm_answer + s_llm_answer_len, text, len + 1);
        s_llm_answer_len += len;
    }
    if (s_llm_is_caption) return true;
    ask_append_answer(text);

    // 整段增量一起追加 —— 每段本身是完整的 UTF-8,这样缓冲永远合法
    if (s_sent_len + len >= sizeof(s_sent_buf) - 1) {
        size_t cut = s_sent_len;                // 太长了,在最后一个完整字符处断开
        while (cut > 0 && (s_sent_buf[cut - 1] & 0xC0) == 0x80) cut--;
        if (cut > 0 && (s_sent_buf[cut - 1] & 0x80)) {
            size_t st = cut - 1;
            while (st > 0 && (s_sent_buf[st] & 0xC0) == 0x80) st--;
            if (st + utf8_clen(s_sent_buf[st]) > cut) cut = st;   // 末尾那个字符不完整
        }
        sentence_flush(cut);
    }
    if (s_sent_len + len >= sizeof(s_sent_buf) - 1) return true;   // 还是放不下,丢这段
    memcpy(s_sent_buf + s_sent_len, text, len + 1);
    s_sent_len += len;

    // 从头扫一遍,找最后一个句末标点,连它一起送走
    size_t cut = 0;
    for (size_t i = 0; i < s_sent_len; ) {
        size_t clen = utf8_clen((unsigned char)s_sent_buf[i]);
        if (i + clen > s_sent_len) break;
        if (is_sentence_end(s_sent_buf + i, clen)) cut = i + clen;
        i += clen;
    }
    sentence_flush(cut);
    return true;
}

// 播报任务:从队列里取句子,发去服务器合成,等这句的 tts_end,
// 再等播放环降下来才取下一句 —— 既是流控,也让音频匀速而不是灌爆。
static void speak_task(void *arg) {
    char sent[SAY_MAX];
    char msg[SAY_MAX * 2 + 32];
    while (s_active) {
        if (xQueueReceive(s_say_q, sent, pdMS_TO_TICKS(200)) != pdTRUE) {
            if (s_say_eof && s_say_pending == 0) s_more_speech = false;
            continue;
        }
        if (s_llm_abort || !s_ws || !esp_websocket_client_is_connected(s_ws)) {
            s_say_pending--;
            continue;
        }
        char esc[SAY_MAX * 2];
        size_t o = 0;
        for (size_t i = 0; sent[i] && o < sizeof(esc) - 8; i++) {
            char c = sent[i];
            if (c == '"' || c == '\\') { esc[o++] = '\\'; esc[o++] = c; }
            else if (c == '\n') { esc[o++] = ' '; }
            else esc[o++] = c;
        }
        esc[o] = '\0';

        s_tts_done = false;
        int n = snprintf(msg, sizeof(msg), "{\"type\":\"say\",\"text\":\"%s\"}", esc);
        esp_websocket_client_send_text(s_ws, msg, n, pdMS_TO_TICKS(3000));
        ESP_LOGI(TAG, "送合成(待播%d): %.30s", s_say_pending, sent);

        for (int i = 0; i < 250 && !s_tts_done && !s_llm_abort && s_active; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        // 等这句基本放完再要下一句,免得音频堆在环里被丢掉
        for (int i = 0; i < 400 && !s_llm_abort && s_active; i++) {
            UBaseType_t waiting = 0;
            if (!s_play_rb) break;
            vRingbufferGetInfo(s_play_rb, NULL, NULL, NULL, NULL, &waiting);
            if (waiting < 3 * 1024) break;
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        s_say_pending--;
        if (s_say_eof && s_say_pending == 0) s_more_speech = false;
    }
    s_speak_task = NULL;
    vTaskDelete(NULL);
}

// 把攒好的一句丢进队列。队列满就等 —— 背压比丢句子强。
static void say_enqueue(const char *sent) {
    if (!s_say_q || !sent[0]) return;
    char buf[SAY_MAX];
    snprintf(buf, sizeof(buf), "%s", sent);
    s_say_pending++;
    s_more_speech = true;
    if (xQueueSend(s_say_q, buf, pdMS_TO_TICKS(20000)) != pdTRUE) {
        s_say_pending--;
        ESP_LOGW(TAG, "句子队列满,丢弃一句");
    }
}

static void llm_task(void *arg) {
    char ctx[640];
    build_chart_context(ctx, sizeof(ctx));
    char sys[1400];        // 人设 + 命盘上下文,别截断 —— 截掉的正好是命盘数据
    s_llm_answer[0] = '\0';
    s_llm_answer_len = 0;
    s_sent_len = 0;
    s_sent_buf[0] = '\0';
    s_say_eof = false;

    esp_err_t err;
    if (s_llm_is_caption) {
        snprintf(sys, sizeof(sys),
                 "你是玄学日历上的一句话解盘师。只输出一句话,12~20个汉字,古风语感,"
                 "不用标点结尾,围绕今日五行气场与行动宜忌。严禁自行推算干支。\n%s", ctx);
        err = llm_chat(sys, "请给出今日一句话。", 64, llm_on_delta, NULL);
    } else {
        snprintf(sys, sizeof(sys), "%s\n\n%s", PERSONA, ctx);
        err = llm_chat(sys, s_llm_question, 300, llm_on_delta, NULL);
    }

    if (s_llm_abort || !s_active) goto out;

    if (err != ESP_OK) {
        const char *m = llm_last_error();
        ESP_LOGW(TAG, "LLM 失败: %s", m ? m : esp_err_to_name(err));
        if (s_llm_is_caption) {
            s_caption[0] = '\0';           // 今日页会退回确定性兜底文案
        } else {
            ask_append_answer(m ? m : "连不上 AI 服务");
            s_state = FS_ASK_IDLE;
            ui_status("OK再问", UI_RED);
        }
        goto out;
    }

    if (s_llm_is_caption) {
        // 文案本该 20 字以内;万一模型话多,按 UTF-8 边界截,别切出半个汉字
        size_t n = strlen(s_llm_answer);
        if (n >= sizeof(s_caption)) {
            n = sizeof(s_caption) - 1;
            while (n > 0 && (s_llm_answer[n] & 0xC0) == 0x80) n--;
        }
        memcpy(s_caption, s_llm_answer, n);
        s_caption[n] = '\0';
        s_caption_day = today_key();
        caption_save();
        if (s_state == FS_TODAY) render_fortune(FS_TODAY);
        ESP_LOGI(TAG, "解盘文案: %s", s_caption);
    } else {
        say_enqueue(s_sent_buf);            // 最后没凑够标点的尾巴
        s_sent_len = 0;
    }
out:
    s_say_eof = true;                       // 队列排空后 speak_task 就收尾
    s_llm_task = NULL;
    vTaskDelete(NULL);
}

// 起一轮 LLM。同时只允许一个在跑 —— 两个 TLS 会话这点内存扛不住。
static bool llm_start(bool caption, const char *question) {
    if (s_llm_task) return false;
    if (!llm_cfg_ready()) return false;
    if (!chat_net_ready()) return false;
    s_llm_is_caption = caption;
    s_llm_abort = false;
    if (question) snprintf(s_llm_question, sizeof(s_llm_question), "%s", question);
    return xTaskCreate(llm_task, "llm", 7168, NULL, 4, &s_llm_task) == pdPASS;
}

// ---------------------------------------------------------------- 今日/命盘视图

static void anim_rotate(void *obj, int32_t v) {
    lv_arc_set_rotation((lv_obj_t *)obj, (uint16_t)(v % 360));
}

static void anim_opa(void *obj, int32_t v) {
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

static lv_obj_t *make_arc(lv_obj_t *parent, int size, int width, uint32_t color,
                          int angle_len, bool reverse) {
    lv_obj_t *arc = lv_arc_create(parent);
    lv_obj_set_size(arc, size, size);
    lv_arc_set_bg_angles(arc, 0, angle_len);
    lv_arc_set_value(arc, 100);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(arc, width, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, width, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(arc, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(arc, LV_OPA_TRANSP, LV_PART_INDICATOR);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, arc);
    lv_anim_set_exec_cb(&a, anim_rotate);
    if (reverse) lv_anim_set_values(&a, 360, 0);
    else lv_anim_set_values(&a, 0, 360);
    lv_anim_set_duration(&a, 14000);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
    return arc;
}

static void view_today(void) {
    if (!s_chart_ok) return;
    if (!bsp_lvgl_lock(300)) return;
    lv_obj_t *cont = content_reset();

    if (!s_time_ok) {                    // 还没授时,今日运势无从谈起
        big_label(cont, "今日", 88, 0, UI_YELLOW);
        lv_obj_t *p = ui_pixel_panel_create(cont, 12, 60, 216, 120, UI_PAPER);
        lv_obj_t *l = ui_pixel_label(p, "正在校时...\n\n设备没有时钟,联网后\n才知道今天是哪天",
                                     &font_cn_16, 0x9FB8E8);
        lv_obj_set_width(l, 196);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_center(l);
        bsp_lvgl_unlock();
        ui_status("等待授时·OK看命盘", UI_ORANGE);
        return;
    }

    char buf[128];
    // 顶行:今日干支(书法体) + 公历日期
    snprintf(buf, sizeof(buf), "%s日", s_chart.liuri);
    big_label(cont, buf, 14, 0, UI_YELLOW);
    cn_label(cont, s_chart.solar_date, 118, 8, 0x9FB8E8);

    // 罗盘:双环缓旋 + 中心指数
    lv_obj_t *ring = make_arc(cont, 104, 3, UI_INK, 300, false);
    lv_obj_set_pos(ring, 68, 32);
    lv_obj_t *ring2 = make_arc(cont, 82, 2, 0x8A7429, 240, true);
    lv_obj_set_pos(ring2, 79, 43);
    snprintf(buf, sizeof(buf), "%d", s_chart.score);
    lv_obj_t *sc = ui_pixel_label(cont, buf, &font_mystic_28, UI_YELLOW);
    lv_obj_align(sc, LV_ALIGN_TOP_MID, 0, 62);
    lv_obj_t *sct = ui_pixel_label(cont, "指数", &font_cn_16, 0x8A7429);
    lv_obj_align(sct, LV_ALIGN_TOP_MID, 0, 94);

    // 幸运色:实色呼吸块 + 名称
    lv_obj_t *sw = lv_obj_create(cont);
    lv_obj_remove_flag(sw, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(sw, 16, 142);
    lv_obj_set_size(sw, 30, 30);
    lv_obj_set_style_radius(sw, 4, 0);
    lv_obj_set_style_border_color(sw, lv_color_hex(UI_INK), 0);
    lv_obj_set_style_border_width(sw, 1, 0);
    lv_obj_set_style_pad_all(sw, 0, 0);
    lv_obj_set_style_bg_color(sw, lv_color_hex(s_chart.color_rgb), 0);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, sw);
    lv_anim_set_exec_cb(&a, anim_opa);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_50);
    lv_anim_set_duration(&a, 1200);
    lv_anim_set_playback_duration(&a, 1200);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);

    cn_label(cont, "幸运色", 54, 142, 0x9FB8E8);
    cn_label(cont, s_chart.color_name, 54, 160, s_chart.color_rgb);
    snprintf(buf, sizeof(buf), "幸运数 %s", s_chart.number);
    cn_label(cont, buf, 132, 142, UI_INK);
    snprintf(buf, sizeof(buf), "方位 %s", s_chart.direction);
    cn_label(cont, buf, 132, 160, UI_INK);

    // 宜忌一行
    snprintf(buf, sizeof(buf), "宜 %s%s%s   忌 %s",
             s_chart.yi[0], s_chart.yi[1][0] ? "·" : "", s_chart.yi[1],
             s_chart.ji[0][0] ? s_chart.ji[0] : "-");
    cn_label(cont, buf, 16, 178, 0xC8CFE8);

    // 一句话解盘:联网时是 LLM 写的,否则用确定性兜底
    bool fresh_cap = s_caption[0] && s_caption_day == today_key();
    char cap[96];
    if (!fresh_cap) caption_fallback(cap, sizeof(cap));
    lv_obj_t *p = ui_pixel_panel_create(cont, 10, 196, 220, 46, UI_PAPER);
    snprintf(buf, sizeof(buf), "「%s」", fresh_cap ? s_caption : cap);
    lv_obj_t *cl = ui_pixel_label(p, buf, &font_cn_16, UI_YELLOW);
    lv_obj_set_width(cl, 202);
    lv_label_set_long_mode(cl, LV_LABEL_LONG_WRAP);
    lv_obj_center(cl);

    bsp_lvgl_unlock();
    ui_status("OK命盘 下键问事", 0x8A7429);
}

static void view_chart(void) {
    if (!s_chart_ok) return;
    if (!bsp_lvgl_lock(300)) return;
    lv_obj_t *cont = content_reset();

    // 四柱:横排四列,干上支下,五行套色(端侧按干支字自己配色)
    static const char *const LABELS[4] = { "年", "月", "日", "时" };
    for (int i = 0; i < 4; i++) {
        char g[4] = { 0 }, z[4] = { 0 };
        bool unknown = (i == 3 && s_chart.hour_unknown);
        if (!unknown) {
            memcpy(g, s_chart.pillar[i], 3);
            memcpy(z, s_chart.pillar[i] + 3, 3);
        }
        int x = 24 + i * 52;
        cn_label(cont, LABELS[i], x + 6, 0, 0x8A7429);
        big_label(cont, unknown ? "？" : g, x, 20, unknown ? 0x8A7429 : bz_char_color(g));
        big_label(cont, unknown ? "？" : z, x, 54, unknown ? 0x8A7429 : bz_char_color(z));
    }
    lv_obj_t *sep = lv_obj_create(cont);
    lv_obj_remove_flag(sep, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(sep, 16, 94);
    lv_obj_set_size(sep, 208, 1);
    lv_obj_set_style_bg_color(sep, lv_color_hex(0x6B5A23), 0);
    lv_obj_set_style_border_width(sep, 0, 0);

    char buf[224];
    snprintf(buf, sizeof(buf), "日主 %s   %s", s_chart.day_master, s_chart.start_info);
    cn_label(cont, buf, 16, 102, UI_INK);
    if (s_time_ok)
        snprintf(buf, sizeof(buf), "当前大运 %s",
                 s_chart.current_dayun[0] ? s_chart.current_dayun : "未起运");
    else
        snprintf(buf, sizeof(buf), "当前大运 校时后可知");
    cn_label(cont, buf, 16, 124, UI_YELLOW);

    // 大运长河
    lv_obj_t *p = ui_pixel_panel_create(cont, 10, 150, 220, 92, UI_PAPER);
    size_t used = 0;
    buf[0] = '\0';
    for (int i = 0; i < s_chart.dayun_n && i < 8; i++) {
        used += snprintf(buf + used, sizeof(buf) - used, "%s%s(%d-%d)",
                         i ? "  " : "", s_chart.dayun[i].gan_zhi,
                         s_chart.dayun[i].start_age, s_chart.dayun[i].start_age + 9);
        if (used >= sizeof(buf) - 24) break;
    }
    lv_obj_t *dl = ui_pixel_label(p, buf, &font_cn_16, 0xC8CFE8);
    lv_obj_set_width(dl, 202);
    lv_label_set_long_mode(dl, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(dl, 2, 2);

    bsp_lvgl_unlock();
    ui_status("OK今日 下键问事", 0x8A7429);
}

static void render_fortune(fs_state_t view) {
    if (!s_chart_ok && !compute_chart()) return;
    if (view == FS_CHART) view_chart();
    else view_today();
    s_state = view;
}

// ---------------------------------------------------------------- ws


static void handle_json(const char *json) {
    cJSON *root = cJSON_Parse(json);
    if (!root) return;
    const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(root, "type"));
    if (!type) { cJSON_Delete(root); return; }

    if (strcmp(type, "bazi_parsed") == 0) {
        const cJSON *b = cJSON_GetObjectItem(root, "birth");
        s_pending.year = (uint16_t)cJSON_GetNumberValue(cJSON_GetObjectItem(b, "year"));
        s_pending.month = (uint8_t)cJSON_GetNumberValue(cJSON_GetObjectItem(b, "month"));
        s_pending.day = (uint8_t)cJSON_GetNumberValue(cJSON_GetObjectItem(b, "day"));
        s_pending.hour_unknown = cJSON_IsTrue(cJSON_GetObjectItem(b, "hour_unknown"));
        if (!s_pending.hour_unknown) {
            s_pending.hour = (uint8_t)cJSON_GetNumberValue(cJSON_GetObjectItem(b, "hour"));
            s_pending.minute = (uint8_t)cJSON_GetNumberValue(cJSON_GetObjectItem(b, "minute"));
        }
        s_pending.valid = true;
        const char *echo = cJSON_GetStringValue(cJSON_GetObjectItem(b, "echo"));
        char line[160];
        snprintf(line, sizeof(line), "%s%s", echo ? echo : "",
                 s_pending.hour_unknown ? "\n(未说时辰,时柱将留空)" : "");
        view_wizard(line, "OK确认  上键重说", UI_INK);
        s_state = FS_WIZ_CONFIRM;
        ui_status("请核对出生信息", UI_YELLOW);
    } else if (strcmp(type, "bazi_error") == 0) {
        const char *m = cJSON_GetStringValue(cJSON_GetObjectItem(root, "message"));
        view_wizard(m ? m : "没听清", "按OK再说一次\n例如:1998年3月15日早上7点半", UI_RED);
        s_state = FS_WIZ_INTRO;
        ui_status("再试一次", UI_RED);
    } else if (strcmp(type, "geo_parsed") == 0) {
        const char *city = cJSON_GetStringValue(cJSON_GetObjectItem(root, "city"));
        double lng = cJSON_GetNumberValue(cJSON_GetObjectItem(root, "longitude"));
        s_pending_lng100 = (int32_t)(lng * 100 + (lng >= 0 ? 0.5 : -0.5));
        strlcpy(s_geo_city, city ? city : "?", sizeof(s_geo_city));
        char line[160];
        snprintf(line, sizeof(line), "出生地: %s\n东经 %d.%02d°\n\n将启用真太阳时校准",
                 s_geo_city, (int)(s_pending_lng100 / 100),
                 (int)(s_pending_lng100 % 100));
        view_wizard(line, "OK确认  上键重说", UI_INK);
        s_state = FS_WIZ_GEO_CONFIRM;
        ui_status("请核对出生地", UI_YELLOW);
    } else if (strcmp(type, "geo_error") == 0) {
        const char *m = cJSON_GetStringValue(cJSON_GetObjectItem(root, "message"));
        view_wizard(m ? m : "没听清", "OK再说一次  上键跳过", UI_RED);
        s_state = FS_WIZ_GEO_INTRO;
        ui_status("再试一次", UI_RED);
    } else if (strcmp(type, "fortune_data") == 0) {
        // 排盘本身端侧已经算完,这里只取 LLM 写的那句解盘文案
        // (顺带让服务器把命盘挂进本连接的问命上下文)。
        const cJSON *t = cJSON_GetObjectItem(root, "today");
        const char *cap = cJSON_GetStringValue(cJSON_GetObjectItem(t, "caption"));
        if (cap && cap[0]) {
            snprintf(s_caption, sizeof(s_caption), "%s", cap);
            s_caption_day = today_key();
            caption_save();
            if (s_state == FS_TODAY) render_fortune(FS_TODAY);
        }
    } else if (strcmp(type, "asr") == 0) {
        const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(root, "text"));
        if (s_state != FS_ASK_THINKING && s_state != FS_ASK_SPEAKING) t = NULL;
        if (t && bsp_lvgl_lock(200)) {
            if (s_ask_q) lv_label_set_text(s_ask_q, t);
            bsp_lvgl_unlock();
        }
        // 服务器只负责把话转成字,推理由设备自己拿命盘去问 LLM
        if (t && t[0] && s_state == FS_ASK_THINKING) {
            if (!llm_start(false, t)) {
                ask_append_answer(llm_cfg_ready() ? "上一个问题还在推演中"
                                                  : "还没配 LLM,见 main/chat_config.h");
                s_state = FS_ASK_IDLE;
                ui_status("OK再问", UI_RED);
            }
        }
    } else if (strcmp(type, "error") == 0) {
        const char *m = cJSON_GetStringValue(cJSON_GetObjectItem(root, "message"));
        if (m) ask_append_answer(m);
    } else if (strcmp(type, "tts_end") == 0) {
        if (s_state == FS_ASK_THINKING || s_state == FS_ASK_SPEAKING) {
            s_tts_done = true;
            if (s_state == FS_ASK_THINKING) s_state = FS_ASK_SPEAKING;  // 空回答收尾
        }
    }
    cJSON_Delete(root);
}

static char s_txt[2048];
static size_t s_txt_len;

static void ws_event_cb(void *arg, esp_event_base_t base, int32_t id, void *data) {
    esp_websocket_event_data_t *ev = (esp_websocket_event_data_t *)data;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "ws 已连接");
        if (s_state == FS_HOME) ui_status("上下选择 OK进入", 0x8A7429);
        if (s_birth.valid && s_caption_day != today_key()) llm_start(true, NULL);
        break;
    case WEBSOCKET_EVENT_DATA: {
        static uint8_t s_frame_op;
        if (ev->op_code == 0x01 || ev->op_code == 0x02) s_frame_op = ev->op_code;
        uint8_t op = (ev->op_code == 0x00) ? s_frame_op : ev->op_code;

        if (op == 0x02) {
            // 二进制 = 问事回答的 TTS 音频
            if (s_play_rb && (s_state == FS_ASK_THINKING || s_state == FS_ASK_SPEAKING)) {
                // 环小了,满的时候宁可在这儿等(TCP 自然背压),也不能丢音频
                if (xRingbufferSend(s_play_rb, ev->data_ptr, ev->data_len,
                                    pdMS_TO_TICKS(5000)) != pdTRUE) {
                    ESP_LOGW(TAG, "播放环满,丢了 %d 字节音频", ev->data_len);
                }
            }
        } else if (op == 0x01) {
            if (ev->payload_offset == 0) s_txt_len = 0;
            if (s_txt_len + ev->data_len < sizeof(s_txt)) {
                memcpy(s_txt + s_txt_len, ev->data_ptr, ev->data_len);
                s_txt_len += ev->data_len;
            }
            if (ev->payload_offset + ev->data_len >= ev->payload_len) {
                s_txt[s_txt_len] = '\0';
                handle_json(s_txt);
            }
        }
        break;
    }
    default:
        break;
    }
}

// 录音任务:录生日(FS_WIZ_LISTEN)和问问题(FS_ASK_LISTEN)时把麦克风数据发 ws
static void rec_task(void *arg) {
    static int16_t buf[CHUNK_BYTES / 2];
    while (s_active) {
        bool listening = (s_state == FS_WIZ_LISTEN || s_state == FS_ASK_LISTEN ||
                          s_state == FS_WIZ_GEO_LISTEN);
        if (!listening) {
            vTaskDelay(pdMS_TO_TICKS(40));
            continue;
        }
        if (bsp_audio_read(buf, CHUNK_BYTES) == ESP_OK &&
            (s_state == FS_WIZ_LISTEN || s_state == FS_ASK_LISTEN ||
             s_state == FS_WIZ_GEO_LISTEN) &&
            s_ws && esp_websocket_client_is_connected(s_ws)) {
            esp_websocket_client_send_bin(s_ws, (const char *)buf, CHUNK_BYTES,
                                          pdMS_TO_TICKS(10000));
        }
    }
    s_rec_task = NULL;
    vTaskDelete(NULL);
}

// 联网任务:等 Wi-Fi 就绪后起 ws;无凭据则提示去 Chat 页配网
static void net_task(void *arg) {
    esp_err_t err = chat_net_ensure();
    if (err == ESP_ERR_NOT_FOUND) {
        // 首次使用:就地开配网热点
        chat_prov_start();
        view_wizard("首次使用,先连Wi-Fi\n\n1. 手机连热点\n   Tianji-Setup\n2. 自动弹出配置页\n   (没弹就开 192.168.4.1)",
                    "配好后自动继续", UI_INK);
        s_state = FS_PROVISION;
        ui_status("等待手机配网", UI_YELLOW);
    }
    for (int i = 0; s_active && !chat_net_ready(); i++) {
        vTaskDelay(pdMS_TO_TICKS(500));
        if (i == 40 && s_state != FS_PROVISION)
            ui_status("连不上Wi-Fi,仍在重试", UI_RED);
    }
    if (!s_active) goto out;
    chat_prov_stop();
    if (s_state == FS_PROVISION) view_home();

    char uri[160];
    snprintf(uri, sizeof(uri), "ws://%s:%d%s?token=%s",
             CHAT_SERVER_HOST, CHAT_SERVER_PORT, CHAT_SERVER_PATH, CHAT_AUTH_TOKEN);
    esp_websocket_client_config_t cfg = {
        .uri = uri,
        .buffer_size = 2048,
        .task_stack = 5120,
        .reconnect_timeout_ms = 3000,
        .network_timeout_ms = 10000,
    };
    s_ws = esp_websocket_client_init(&cfg);
    if (s_ws) {
        esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, ws_event_cb, NULL);
        esp_websocket_client_start(s_ws);
    }
out:
    s_net_task = NULL;
    vTaskDelete(NULL);
}

// 配网页面保存成功回调(httpd 任务上下文;Wi-Fi 连接由配网模块自己发起)
void chat_prov_on_saved(void) {
    chat_net_creds_saved();
    s_prov_saved = true;               // FS_SET_PROV 下由 settings_tick 接管回程
    ui_status("Wi-Fi连接中...", UI_ORANGE);
}

// 问事:开始聆听新问题(播放/思考中调用即打断——服务器收到 start 会取消旧回答)
static void ask_start_listen(void) {
    if (!s_ws || !esp_websocket_client_is_connected(s_ws)) {
        ui_status("未连接服务器", UI_RED);
        return;
    }
    if (bsp_lvgl_lock(200)) {
        if (s_ask_q) lv_label_set_text(s_ask_q, "");
        if (s_ask_a) lv_label_set_text(s_ask_a, "");
        bsp_lvgl_unlock();
    }
    adpcm_reset();
    s_tts_done = false;
    s_llm_abort = true;                    // 上一轮还在推演的话,叫停
    s_more_speech = false;
    esp_websocket_client_send_text(s_ws, "{\"type\":\"start\",\"mode\":\"ask\"}",
                                   29, pdMS_TO_TICKS(2000));
    s_state = FS_ASK_LISTEN;
    ui_status("聆听中·OK结束", UI_RED);
}

// 向导收尾:保存生日(含经度) -> 请求排盘
static void wizard_finish(void) {
    // 城市:确认过出生地才有;跳过则清空(按东八区)
    if (s_birth.lng100 == 12000) s_birth_city[0] = '\0';
    birth_save();
    s_caption[0] = '\0';               // 换了生辰,旧文案作废
    s_caption_day = 0;
    if (compute_chart()) {
        render_fortune(FS_CHART);       // 端侧排盘是同步的,不用等网络
        llm_start(true, NULL);          // 顺带去讨一句解盘文案
    } else {
        view_wizard("生辰超出可排范围\n(仅支持 1900-2100)", "长按OK返回", UI_RED);
        s_state = FS_ERROR;
        ui_status("排盘失败", UI_RED);
    }
}

// ---------------------------------------------------------------- 页面接口

void demo_fortune_enter(void) {
    s_active = true;
    s_txt_len = 0;
    birth_load();
    caption_load();

    if (!bsp_lvgl_lock(500)) return;
    s_scr = ui_pixel_screen_create("");
    s_content = NULL;
    // 标题牌上盖书法"天机",小道童立于牌侧
    lv_obj_t *title = ui_pixel_label(s_scr, "天机", &font_mystic_28, UI_YELLOW);
    lv_obj_set_pos(title, 48, 8);
    ui_pixel_mascot_create(s_scr, 196, 2);
    // 状态/按键提示:底部通栏,单行截断,绝不与正文重叠
    s_status = ui_pixel_label(s_scr, "", &font_cn_16, UI_ORANGE);
    lv_obj_set_pos(s_status, 10, 292);
    lv_obj_set_width(s_status, 220);
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_screen_load(s_scr);
    bsp_lvgl_unlock();

    bsp_audio_set_format(16000, 16, 1);   // 向导录音要用

    compute_chart();        // 开机即排盘,不等网络(未校时也能出四柱与大运)
    view_home();

    if (bsp_lvgl_lock(300)) {
        s_tick = lv_timer_create(settings_tick, 2000, NULL);
        bsp_lvgl_unlock();
    }

    s_play_rb = xRingbufferCreate(PLAY_RB_BYTES, RINGBUF_TYPE_BYTEBUF);
    s_say_q = xQueueCreate(SAY_Q_LEN, SAY_MAX);
    bsp_audio_set_volume(75);
    xTaskCreate(rec_task, "fortune_rec", 4096, NULL, 5, &s_rec_task);
    xTaskCreate(play_task, "fortune_play", 4096, NULL, 5, &s_play_task);
    xTaskCreate(speak_task, "fortune_say", 4096, NULL, 5, &s_speak_task);
    xTaskCreate(net_task, "fortune_net", 4096, NULL, 5, &s_net_task);
}

void demo_fortune_exit(void) {
    s_active = false;
    if (s_tick && bsp_lvgl_lock(300)) {
        lv_timer_delete(s_tick);
        s_tick = NULL;
        bsp_lvgl_unlock();
    }
    if (s_ws) {
        esp_websocket_client_close(s_ws, pdMS_TO_TICKS(800));
        esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
    }
    for (int i = 0; i < 60 && (s_rec_task || s_net_task || s_play_task ||
                               s_speak_task); i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (s_play_rb) { vRingbufferDelete(s_play_rb); s_play_rb = NULL; }
    if (s_say_q) { vQueueDelete(s_say_q); s_say_q = NULL; }
    s_scr = NULL;
    s_content = NULL;
    s_status = NULL;
    s_ask_q = NULL;
    s_ask_a = NULL;
}

void demo_fortune_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    // 长按OK = 从任何子页面回首页(录音/播放中会先收尾)
    if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG) {
        if (s_state == FS_HOME || s_state == FS_PROVISION) return;
        if (s_state == FS_SET_PROV) { chat_prov_stop(); s_prov_saved = false; }
        if (s_state == FS_WIZ_LISTEN || s_state == FS_ASK_LISTEN ||
            s_state == FS_WIZ_GEO_LISTEN) {
            if (s_ws) esp_websocket_client_send_text(s_ws, "{\"type\":\"abort\"}", 16,
                                                     pdMS_TO_TICKS(500));
        }
        play_rb_flush();
        s_tts_done = false;
        view_home();
        return;
    }
    if (ev != BSP_BTN_CLICK) return;

    if (s_state == FS_HOME) {
        if (btn == BSP_BTN_UP)   { s_home_sel = (s_home_sel + HOME_N - 1) % HOME_N; home_refresh(); }
        if (btn == BSP_BTN_DOWN) { s_home_sel = (s_home_sel + 1) % HOME_N; home_refresh(); }
        if (btn == BSP_BTN_OK) {
            if (s_home_sel == 4) {
                view_settings();                      // 设置:未录生辰也能进
            } else if (s_home_sel == 3 || !s_birth.valid) {
                start_wizard();                       // 重录 / 未录入
            } else if (s_home_sel == 0 || s_home_sel == 1) {
                if (s_chart_ok || compute_chart()) {
                    render_fortune(s_home_sel == 0 ? FS_CHART : FS_TODAY);
                } else {
                    ui_status("生辰超出可排范围", UI_RED);
                }
            } else {                                  // 问事
                if (s_ws && esp_websocket_client_is_connected(s_ws)) {
                    view_ask();
                    s_state = FS_ASK_IDLE;
                } else {
                    ui_status("未连接服务器", UI_RED);
                }
            }
        }
        return;
    }

    switch (s_state) {
    case FS_SETTINGS:
        if (btn == BSP_BTN_OK) settings_start_prov();
        else if (bsp_lvgl_lock(200)) {               // 上/下:立刻刷新读数
            settings_paint();
            bsp_lvgl_unlock();
        }
        break;
    case FS_WIZ_INTRO:
        if (btn == BSP_BTN_OK && s_ws && esp_websocket_client_is_connected(s_ws)) {
            esp_websocket_client_send_text(s_ws, "{\"type\":\"start\",\"mode\":\"bazi\"}",
                                           30, pdMS_TO_TICKS(2000));
            view_wizard("正在聆听...\n\n请说出生日期和时间\n说完按OK结束", NULL, UI_RED);
            s_state = FS_WIZ_LISTEN;
            ui_status("聆听中", UI_RED);
        }
        break;
    case FS_WIZ_LISTEN:
        if (btn == BSP_BTN_OK) {
            s_state = FS_WIZ_PARSE;
            esp_websocket_client_send_text(s_ws, "{\"type\":\"end\"}", 14,
                                           pdMS_TO_TICKS(2000));
            view_wizard("正在解析...", NULL, 0x9FB8E8);
            ui_status("解析中", UI_ORANGE);
        }
        break;
    case FS_WIZ_CONFIRM:
        if (btn == BSP_BTN_OK) {
            s_state = FS_WIZ_GENDER;
            view_gender();
            ui_status("选择性别", UI_YELLOW);
        } else if (btn == BSP_BTN_UP) {
            view_wizard("再说一次你的出生日期和时间", "按OK开始说话", UI_INK);
            s_state = FS_WIZ_INTRO;
        }
        break;
    case FS_WIZ_GENDER:
        if (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) {
            s_pending_male = !s_pending_male;
            view_gender();
        } else if (btn == BSP_BTN_OK) {
            s_birth = s_pending;
            s_birth.male = s_pending_male;
            s_birth.lng100 = 12000;
            // 最后一步:出生地(真太阳时校准,可跳过)
            view_wizard("说出你的出生城市\n如: 北京 / 成都\n\n用于真太阳时校准\n(经度不同,时柱可能不同)",
                        "OK说话  上键跳过", UI_INK);
            s_state = FS_WIZ_GEO_INTRO;
            ui_status("出生地(可跳过)", UI_YELLOW);
        }
        break;
    case FS_WIZ_GEO_INTRO:
        if (btn == BSP_BTN_OK && s_ws && esp_websocket_client_is_connected(s_ws)) {
            const char *m = "{\"type\":\"start\",\"mode\":\"geo\"}";
            esp_websocket_client_send_text(s_ws, m, strlen(m), pdMS_TO_TICKS(2000));
            view_wizard("正在聆听...\n\n请说出生城市\n说完按OK结束", NULL, UI_RED);
            s_state = FS_WIZ_GEO_LISTEN;
            ui_status("聆听中·OK结束", UI_RED);
        } else if (btn == BSP_BTN_UP) {
            wizard_finish();                 // 跳过:按东八区
        }
        break;
    case FS_WIZ_GEO_LISTEN:
        if (btn == BSP_BTN_OK) {
            s_state = FS_WIZ_GEO_PARSE;
            esp_websocket_client_send_text(s_ws, "{\"type\":\"end\"}", 14,
                                           pdMS_TO_TICKS(2000));
            view_wizard("正在查询...", NULL, 0x9FB8E8);
            ui_status("查询中", UI_ORANGE);
        }
        break;
    case FS_WIZ_GEO_CONFIRM:
        if (btn == BSP_BTN_OK) {
            s_birth.lng100 = s_pending_lng100;
            strlcpy(s_birth_city, s_geo_city, sizeof(s_birth_city));
            wizard_finish();
        } else if (btn == BSP_BTN_UP) {
            view_wizard("再说一次出生城市", "OK说话  上键跳过", UI_INK);
            s_state = FS_WIZ_GEO_INTRO;
        }
        break;
    case FS_TODAY:
        if (btn == BSP_BTN_OK) render_fortune(FS_CHART);
        else if (btn == BSP_BTN_DOWN && s_ws &&
                 esp_websocket_client_is_connected(s_ws)) {
            view_ask();
            s_state = FS_ASK_IDLE;
        }
        break;
    case FS_CHART:
        if (btn == BSP_BTN_OK) render_fortune(FS_TODAY);
        else if (btn == BSP_BTN_DOWN && s_ws &&
                 esp_websocket_client_is_connected(s_ws)) {
            view_ask();
            s_state = FS_ASK_IDLE;
        }
        break;
    case FS_ASK_IDLE:
        if (btn == BSP_BTN_OK) {
            ask_start_listen();
        } else if ((btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) && s_ask_panel) {
            // 手动翻页
            if (bsp_lvgl_lock(200)) {
                lv_obj_scroll_by(s_ask_panel, 0, btn == BSP_BTN_UP ? 80 : -80, LV_ANIM_ON);
                bsp_lvgl_unlock();
            }
        }
        break;
    case FS_ASK_LISTEN:
        if (btn == BSP_BTN_OK) {
            s_state = FS_ASK_THINKING;
            esp_websocket_client_send_text(s_ws, "{\"type\":\"end\"}", 14,
                                           pdMS_TO_TICKS(2000));
            ui_status("推演中...", 0x9FB8E8);
        }
        break;
    case FS_ASK_SPEAKING:
    case FS_ASK_THINKING:
        if (btn == BSP_BTN_OK) {
            // 一键打断并直接开始新提问(start 消息会让服务器取消旧回答)
            play_rb_flush();
            s_tts_done = false;
            ask_start_listen();
        } else if ((btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) && s_ask_panel) {
            if (bsp_lvgl_lock(200)) {
                lv_obj_scroll_by(s_ask_panel, 0, btn == BSP_BTN_UP ? 80 : -80, LV_ANIM_ON);
                bsp_lvgl_unlock();
            }
        }
        break;
    default:
        break;
    }
}
