// main/bazi_engine.h —— 端侧排盘引擎。
//
// 与 server/bazi 是同一套口径:真太阳时换算、以节为界的年月柱、精确起运、
// 加权五行、喜神/幸运色/宜忌。天文部分不在这里算,全部查 bazi_tables.h
// (由服务器那套引擎烘出来),所以端侧结果与服务器逐字段一致。
//
// 纯 C99 + libm,不依赖 ESP-IDF —— 可以在主机上编译跑对拍(见 tools/verify_bazi.sh)。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define BZ_GZ_LEN 7        // "甲子" + NUL
#define BZ_DAYUN_MAX 9

typedef struct {
    int  year, month, day, hour, minute;   // 北京时间
    bool hour_unknown;                     // 只报了日期,没有时辰
    bool male;
    int  lng100;                           // 出生地经度 ×100,12000 = 东经120
} bz_birth_t;

typedef struct {
    char gan_zhi[BZ_GZ_LEN];
    int  start_age, start_year, end_year;
} bz_dayun_t;

typedef struct {
    // ── 四柱 ──
    char eight_char[28];                   // "己丑 丙子 壬子 甲辰"
    char pillar[4][BZ_GZ_LEN];             // 年月日时
    char day_master[4];                    // 日干
    uint8_t gz[4];                         // 四柱干支序 0..59(时柱未知时仍给出)
    bool hour_unknown;
    char true_solar[24];                   // "YYYY-MM-DD HH:MM"

    // ── 大运 ──
    bool is_forward;
    char start_info[48];                   // "N年N月N日起运"
    bz_dayun_t dayun[BZ_DAYUN_MAX];
    int  dayun_n;
    char current_dayun[BZ_GZ_LEN];

    // ── 当下 ──
    char solar_date[12];                   // "YYYY-MM-DD"
    char lunar_date[40];                   // "二零二六年七月十四"
    char liunian[BZ_GZ_LEN], liuyue[BZ_GZ_LEN], liuri[BZ_GZ_LEN];
    char next_jie_name[10], next_jie_date[12], next_liuyue[BZ_GZ_LEN];

    // ── 五行档案(权重口径同 get_bazi_profile) ──
    float wx_pct[5];                       // 金木水火土,已按 1 位小数取整
    bool  strong;                          // 身强
    int   fav;                             // 喜神五行下标(0..4)

    // ── 今日运势 ──
    int   score;
    char  color_name[10];
    uint32_t color_rgb;
    char  number[10];                      // 河图数 "一·六"
    char  direction[8];
    char  liuri_relation[4];               // 同/生/克/泄/耗
    char  yi[2][16], ji[2][16];            // 黄历宜忌各两条
} bz_chart_t;

// 排盘。now_* 是"当下"(设备靠 SNTP 取),用于流年/流月/流日与今日运势。
// 返回 false = 生日或当下超出表范围(1900..2100)。
bool bz_compute(const bz_birth_t *b,
                int now_y, int now_mo, int now_d, int now_h, int now_mi,
                bz_chart_t *out);

// 干支单字 -> 五行显示色(命盘页按色渲染)。未知字给鎏金。
uint32_t bz_char_color(const char *utf8_char);
