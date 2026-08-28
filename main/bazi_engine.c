// main/bazi_engine.c —— 端侧排盘。算法逐条对齐 server/bazi/{engine,fortune}.py,
// 包括它那几处刻意的口径(时辰未知仍按 00:00 排时柱、五行档案在经度=120 时
// 完全不做真太阳时校正),否则两边对不上。
#include "bazi_engine.h"
#include "bazi_tables.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <math.h>
#include <stdio.h>
#include <string.h>

#define WX_METAL 0
#define WX_WOOD  1
#define WX_WATER 2
#define WX_FIRE  3
#define WX_EARTH 4

// 五行相生:index -> 我生者
static const int WX_SHENG[5] = { WX_WATER, WX_FIRE, WX_WOOD, WX_EARTH, WX_METAL };
// 五行相克:index -> 我克者
static const int WX_KE[5]    = { WX_WOOD, WX_EARTH, WX_FIRE, WX_METAL, WX_WATER };

static const char *const WX_COLOR_NAME[5] = { "鎏金白", "翠竹青", "玄墨蓝", "朱砂红", "琥珀黄" };
static const uint32_t     WX_COLOR_RGB[5] = { 0xE8E3D3, 0x2E8B57, 0x1F4E79, 0xC3272B, 0xD9A404 };
static const char *const  WX_NUMBER[5]    = { "四·九", "三·八", "一·六", "二·七", "五·十" };
static const char *const  WX_DIRECTION[5] = { "西", "东", "北", "南", "中宫" };

static const char *const JIE_NAME[12] = {
    "立春", "惊蛰", "清明", "立夏", "芒种", "小暑",
    "立秋", "白露", "寒露", "立冬", "大雪", "小寒"
};

// ---------------------------------------------------------------- 历法基础

// 公历 -> 距 1900-01-01 的天数(Howard Hinnant 的 days_from_civil)
static int32_t days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    const int32_t era = (y >= 0 ? y : y - 399) / 400;
    const uint32_t yoe = (uint32_t)(y - era * 400);
    const uint32_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    // Hinnant 原式给的是距 1970-01-01 的天数,这里换算到 1900-01-01 基准
    return era * 146097 + (int32_t)doe - 719468 + 25567;
}

static void civil_from_days(int32_t z, int *y, int *m, int *d) {
    z += 719468 - 25567;
    const int32_t era = (z >= 0 ? z : z - 146096) / 146097;
    const uint32_t doe = (uint32_t)(z - era * 146097);
    const uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int32_t yr = (int32_t)yoe + era * 400;
    const uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const uint32_t mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp + (mp < 10 ? 3 : -9));
    *y = yr + (*m <= 2);
}

static bool is_leap(int y) {
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

static int days_in_month(int y, int m) {
    static const int md[13] = { 0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    return (m == 2 && is_leap(y)) ? 29 : md[m];
}

static int day_of_year(int y, int m, int d) {
    return days_from_civil(y, m, d) - days_from_civil(y, 1, 1) + 1;
}

// 距 1900-01-01 00:00:00 的秒
static int64_t secs_of(int y, int mo, int d, int h, int mi, int s) {
    return (int64_t)days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + s;
}

// ---------------------------------------------------------------- 真太阳时

// 均时差(分钟),Spencer 近似 —— 与 engine.py 的 _equation_of_time 同式
static double equation_of_time(int doy) {
    double B = 2.0 * M_PI * (doy - 81) / 365.0;
    return 9.87 * sin(2 * B) - 7.53 * cos(B) - 1.5 * sin(B);
}

// 北京时间 -> 真太阳时(秒)。Python 侧是 datetime + timedelta(分钟),
// timedelta 精度到微秒,取秒时向下取整,这里照做。
static int64_t to_true_solar(int y, int mo, int d, int h, int mi, int lng100) {
    double corr = (lng100 / 100.0 - 120.0) * 4.0 + equation_of_time(day_of_year(y, mo, d));
    int64_t us = (int64_t)llround(corr * 60e6);
    int64_t base_us = secs_of(y, mo, d, h, mi, 0) * 1000000LL + us;
    return (int64_t)floor((double)base_us / 1e6);
}

// ---------------------------------------------------------------- 节表

static int64_t jie_secs(int i) {
    return (int64_t)bz_jie_min[i] * 60 + bz_jie_sec[i];
}

// 最后一个 <= t 的节;t 早于首节返回 -1
static int jie_index_at(int64_t t) {
    if (t < jie_secs(0)) return -1;
    int lo = 0, hi = BZ_JIE_N - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (jie_secs(mid) <= t) lo = mid; else hi = mid - 1;
    }
    return lo;
}

// 节序 -> 年干支序。BZ_JIE_FIRST_YEAR 是过完 0 号节(小寒)之后的年柱,
// 之后每过一个立春(i%12==1)+1。
static int year_gz_at(int i) {
    int lichun = i >= 1 ? (i - 1) / 12 + 1 : 0;
    return (BZ_JIE_FIRST_YEAR + lichun) % 60;
}

static int month_gz_at(int i) { return (BZ_JIE_FIRST_MONTH + i) % 60; }

// ---------------------------------------------------------------- 干支

static void gz_str(int idx, char *out) {
    snprintf(out, BZ_GZ_LEN, "%s%s", bz_gan[idx % 10], bz_zhi[idx % 12]);
}

// 某一天(距基准日 days)的日干支序
static int day_gz_at(int32_t days) {
    int v = (int)((BZ_DAY_GZ_EPOCH + days) % 60);
    return v < 0 ? v + 60 : v;
}

// 时柱。晚子时(23:00-23:59)的时干按次日日干起五鼠遁,日柱本身不变。
static int hour_gz(int day_gz_idx, int32_t days, int hour) {
    int zhi = ((hour + 1) / 2) % 12;
    int day_gan = (hour == 23 ? day_gz_at(days + 1) : day_gz_idx) % 10;
    int gan = (day_gan % 5 * 2 + zhi) % 10;
    // 由 (gan, zhi) 反查 60 甲子序:同余方程,gan/zhi 差值决定
    int idx = gan;
    while (idx % 12 != zhi) idx += 10;
    return idx % 60;
}

// ---------------------------------------------------------------- 农历

// 距基准日 days 的公历日 -> 农历。返回农历月表下标
static int lunar_month_index(int32_t days) {
    if (days < (int32_t)bz_lm_day[0]) return -1;
    int lo = 0, hi = BZ_LM_N - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if ((int32_t)bz_lm_day[mid] <= days) lo = mid; else hi = mid - 1;
    }
    return lo;
}

static void lunar_date_str(int32_t days, char *out, size_t n) {
    int i = lunar_month_index(days);
    if (i < 0) { snprintf(out, n, "—"); return; }
    int day = days - (int32_t)bz_lm_day[i] + 1;
    int mon = bz_lm_flag[i] & 0x0F;
    int leap = (bz_lm_flag[i] >> 4) & 1;
    int year = bz_lm_year[i];

    char ys[24] = { 0 };
    for (int p = 1000; p >= 1; p /= 10) {
        int dgt = (year / p) % 10;
        strncat(ys, bz_digit[dgt], sizeof(ys) - strlen(ys) - 1);
    }
    snprintf(out, n, "%s年%s%s月%s", ys, leap ? "闰" : "", bz_lmonth[mon],
             (day >= 1 && day <= 30) ? bz_lday[day] : "?");
}

// ---------------------------------------------------------------- 起运

// Yun.__compute_start 的 sect=1 分支。start/end 都是"秒 + 该时刻的时支序"。
static int yun_time_zhi_index(int hour) {
    if (hour == 23) return 11;             // Yun 的特例:23 点按亥算
    for (int i = 1, x = 1; i <= 21; i += 2, x++) {
        if (hour == i || hour == i + 1) return x;
    }
    return 0;                              // 00:xx 落子
}

// ---------------------------------------------------------------- 五行档案

static const int GAN_W[4]      = { 7, 10, 10, 7 };            // 年月日时
static const int HIDE_W[4][3]  = { { 7, 3, 1 }, { 15, 5, 3 }, { 10, 4, 2 }, { 7, 3, 1 } };

// Python 的 round(x, 1)
static double round1(double x) {
    double v = x * 10.0;
    double r = floor(v);
    double frac = v - r;
    if (frac > 0.5) r += 1;
    else if (frac == 0.5 && fmod(r, 2.0) != 0.0) r += 1;
    return r / 10.0;
}

// ---------------------------------------------------------------- 主流程

uint32_t bz_char_color(const char *c) {
    for (int i = 0; i < 10; i++)
        if (strcmp(c, bz_gan[i]) == 0) return WX_COLOR_RGB[bz_gan_wx[i]];
    for (int i = 0; i < 12; i++)
        if (strcmp(c, bz_zhi[i]) == 0) return WX_COLOR_RGB[bz_zhi_wx[i]];
    return 0xD4AF37;
}

// 四柱:给定真太阳时(秒),填 gz[4]
static bool pillars_at(int64_t t, int *gz) {
    int ji = jie_index_at(t);
    if (ji < 0) return false;
    int32_t days = (int32_t)(t >= 0 ? t / 86400 : (t - 86399) / 86400);
    int secs_in_day = (int)(t - (int64_t)days * 86400);
    int hour = secs_in_day / 3600;

    gz[0] = year_gz_at(ji);
    gz[1] = month_gz_at(ji);
    gz[2] = day_gz_at(days);
    gz[3] = hour_gz(gz[2], days, hour);
    return true;
}

// 喜神:口径同 fortune.favorable_element
static int favorable_of(const float pct[5], int day_wx, bool *strong) {
    int yin = -1;
    for (int i = 0; i < 5; i++) if (WX_SHENG[i] == day_wx) yin = i;
    double tong = pct[day_wx] + pct[yin];
    *strong = tong >= 50.0;

    int cand[3], n;
    if (*strong) {
        int guan = -1;
        for (int i = 0; i < 5; i++) if (WX_KE[i] == day_wx) guan = i;
        cand[0] = guan; cand[1] = WX_SHENG[day_wx]; cand[2] = WX_KE[day_wx]; n = 3;
    } else {
        cand[0] = yin; cand[1] = day_wx; n = 2;
    }
    int best = cand[0];
    for (int i = 1; i < n; i++) if (pct[cand[i]] < pct[best]) best = cand[i];
    return best;
}

static const char *relation_to(int fav, int other) {
    if (other == fav) return "同";
    if (WX_SHENG[other] == fav) return "生";
    if (WX_KE[other] == fav) return "克";
    if (WX_SHENG[fav] == other) return "泄";
    return "耗";
}

static int rel_score(const char *r) {
    if (!strcmp(r, "同")) return 88;
    if (!strcmp(r, "生")) return 92;
    if (!strcmp(r, "泄")) return 72;
    if (!strcmp(r, "耗")) return 62;
    return 48;
}

static int rel_bonus(const char *r) {
    if (!strcmp(r, "同")) return 4;
    if (!strcmp(r, "生")) return 6;
    if (!strcmp(r, "泄")) return -2;
    if (!strcmp(r, "耗")) return -4;
    return -8;
}

bool bz_compute(const bz_birth_t *b, int ny, int nmo, int nd, int nh, int nmi,
                bz_chart_t *o)
{
    memset(o, 0, sizeof(*o));
    o->hour_unknown = b->hour_unknown;

    // ── 四柱(真太阳时) ──
    int bh = b->hour_unknown ? 0 : b->hour;
    int bmi = b->hour_unknown ? 0 : b->minute;
    int64_t t = to_true_solar(b->year, b->month, b->day, bh, bmi, b->lng100);
    int gz[4];
    if (!pillars_at(t, gz)) return false;
    for (int i = 0; i < 4; i++) { o->gz[i] = (uint8_t)gz[i]; gz_str(gz[i], o->pillar[i]); }
    snprintf(o->day_master, sizeof(o->day_master), "%s", bz_gan[gz[2] % 10]);

    {
        int32_t dd = (int32_t)(t >= 0 ? t / 86400 : (t - 86399) / 86400);
        int rem = (int)(t - (int64_t)dd * 86400);
        int yy, mm, dy;
        civil_from_days(dd, &yy, &mm, &dy);
        snprintf(o->true_solar, sizeof(o->true_solar), "%04d-%02d-%02d %02d:%02d",
                 yy, mm, dy, rem / 3600, rem % 3600 / 60);
    }
    snprintf(o->eight_char, sizeof(o->eight_char), "%s %s %s %s",
             o->pillar[0], o->pillar[1], o->pillar[2],
             b->hour_unknown ? "？？" : o->pillar[3]);

    // ── 大运 ──
    int year_gan = gz[0] % 10;
    bool yang = (year_gan % 2) == 0;
    o->is_forward = (yang && b->male) || (!yang && !b->male);

    int ji = jie_index_at(t);
    int64_t s_start, s_end;
    if (o->is_forward) {
        if (ji + 1 >= BZ_JIE_N) return false;
        s_start = t; s_end = jie_secs(ji + 1);
    } else {
        s_start = jie_secs(ji); s_end = t;
    }
    int start_year, start_month, start_day;
    {
        int32_t d_s = (int32_t)(s_start / 86400), d_e = (int32_t)(s_end / 86400);
        int h_s = (int)(s_start - (int64_t)d_s * 86400) / 3600;
        int h_e = (int)(s_end - (int64_t)d_e * 86400) / 3600;
        int hour_diff = yun_time_zhi_index(h_e) - yun_time_zhi_index(h_s);
        int day_diff = d_e - d_s;
        if (hour_diff < 0) { hour_diff += 12; day_diff -= 1; }
        int month_diff = hour_diff * 10 / 30;
        int month = day_diff * 4 + month_diff;
        start_day = hour_diff * 10 - month_diff * 30;
        start_year = month / 12;
        start_month = month - start_year * 12;
    }
    snprintf(o->start_info, sizeof(o->start_info), "%d年%d月%d日起运",
             start_year, start_month, start_day);

    // 起运公历日 = 真太阳时的那一天 + N年 + N月 + N天。
    // 基准是真太阳时而非输入时间 —— lunar 的 DaYun 用 Lunar 自己的 Solar,
    // 校正后可能落到前一天甚至前一年(如 2000-01-01 00:00 -> 1999-12-31)。
    int ts_y, ts_mo, ts_d;
    {
        int32_t dd = (int32_t)(t >= 0 ? t / 86400 : (t - 86399) / 86400);
        civil_from_days(dd, &ts_y, &ts_mo, &ts_d);
    }
    int sy = ts_y + start_year, sm = ts_mo, sd = ts_d;
    if (sm == 2 && sd > 28 && !is_leap(sy)) sd = 28;      // nextYear 的钳位
    sm += start_month;
    while (sm > 12) { sm -= 12; sy++; }
    if (sd > days_in_month(sy, sm)) sd = days_in_month(sy, sm);   // nextMonth 的钳位
    int32_t start_days = days_from_civil(sy, sm, sd) + start_day;
    int yun_y, yun_mo, yun_d;
    civil_from_days(start_days, &yun_y, &yun_mo, &yun_d);

    int month_gz = gz[1];
    o->dayun_n = 0;
    for (int i = 1; i <= BZ_DAYUN_MAX; i++) {
        int off = o->is_forward ? month_gz + i : month_gz - i;
        off = ((off % 60) + 60) % 60;
        bz_dayun_t *dy = &o->dayun[o->dayun_n++];
        gz_str(off, dy->gan_zhi);
        dy->start_year = yun_y + (i - 1) * 10;
        dy->start_age = dy->start_year - ts_y + 1;
        dy->end_year = dy->start_year + 9;
    }

    // 当前大运:以精确起止日判定(与 engine.py 一致,月日沿用起运日)
    int32_t today_days = days_from_civil(ny, nmo, nd);
    for (int i = 0; i < o->dayun_n; i++) {
        int32_t s = days_from_civil(o->dayun[i].start_year, yun_mo, yun_d);
        int32_t e = (i + 1 < o->dayun_n)
                      ? days_from_civil(o->dayun[i + 1].start_year, yun_mo, yun_d)
                      : days_from_civil(o->dayun[i].end_year + 1, yun_mo, yun_d);
        if (s <= today_days && today_days < e) {
            snprintf(o->current_dayun, BZ_GZ_LEN, "%s", o->dayun[i].gan_zhi);
            break;
        }
    }

    // ── 当下:流年/流月/流日 ──
    int64_t tn = secs_of(ny, nmo, nd, nh, nmi, 0);
    int ngz[4];
    if (!pillars_at(tn, ngz)) return false;
    gz_str(ngz[0], o->liunian);
    gz_str(ngz[1], o->liuyue);
    gz_str(ngz[2], o->liuri);
    snprintf(o->solar_date, sizeof(o->solar_date), "%04d-%02d-%02d", ny, nmo, nd);
    lunar_date_str(today_days, o->lunar_date, sizeof(o->lunar_date));

    // 下一个节 + 交节后的流月
    {
        int i = jie_index_at(tn);
        if (i + 1 < BZ_JIE_N) {
            int nx = i + 1;
            snprintf(o->next_jie_name, sizeof(o->next_jie_name), "%s", JIE_NAME[(nx + 11) % 12]);
            int32_t jd = (int32_t)(jie_secs(nx) / 86400);
            int jy, jm, jdy;
            civil_from_days(jd, &jy, &jm, &jdy);
            snprintf(o->next_jie_date, sizeof(o->next_jie_date), "%04d-%02d-%02d", jy, jm, jdy);
            int after[4];
            if (pillars_at((int64_t)(jd + 1) * 86400 + 12 * 3600, after))
                gz_str(after[1], o->next_liuyue);
        }
    }

    // ── 五行档案 ──
    // get_bazi_profile 的口径:经度=120 时完全不校正;时辰未知时也不校正。
    int64_t tp = (b->lng100 != 12000 && !b->hour_unknown)
                   ? to_true_solar(b->year, b->month, b->day, bh, bmi, b->lng100)
                   : secs_of(b->year, b->month, b->day, bh, bmi, 0);
    int pgz[4];
    if (!pillars_at(tp, pgz)) return false;

    double score[5] = { 0 };
    for (int p = 0; p < 4; p++) score[bz_gan_wx[pgz[p] % 10]] += GAN_W[p];
    for (int p = 0; p < 4; p++) {
        const uint8_t *hide = bz_zhi_hide[pgz[p] % 12];
        for (int k = 0; k < 3 && hide[k] != 0xFF; k++)
            score[bz_gan_wx[hide[k]]] += HIDE_W[p][k];
    }
    double total = 0;
    for (int i = 0; i < 5; i++) total += score[i];
    for (int i = 0; i < 5; i++)
        o->wx_pct[i] = total > 0 ? (float)round1(score[i] / total * 100.0) : 0.0f;

    int day_wx = bz_gan_wx[pgz[2] % 10];
    o->fav = favorable_of(o->wx_pct, day_wx, &o->strong);

    // ── 今日运势 ──
    const char *rel = relation_to(o->fav, bz_gan_wx[ngz[2] % 10]);
    const char *rel_m = relation_to(o->fav, bz_gan_wx[ngz[1] % 10]);
    int sc = rel_score(rel) + rel_bonus(rel_m);
    if (sc < 30) sc = 30;
    if (sc > 97) sc = 97;
    o->score = sc;
    snprintf(o->liuri_relation, sizeof(o->liuri_relation), "%s", rel);
    snprintf(o->color_name, sizeof(o->color_name), "%s", WX_COLOR_NAME[o->fav]);
    o->color_rgb = WX_COLOR_RGB[o->fav];
    snprintf(o->number, sizeof(o->number), "%s", WX_NUMBER[o->fav]);
    snprintf(o->direction, sizeof(o->direction), "%s", WX_DIRECTION[o->fav]);

    // 黄历宜忌:按今日的月柱/日柱查表
    {
        int idx = (ngz[1] * 60 + ngz[2]) * 2;
        for (int k = 0; k < 2; k++) {
            uint8_t a = bz_day_yi[idx + k], c = bz_day_ji[idx + k];
            snprintf(o->yi[k], sizeof(o->yi[k]), "%s", a == BZ_YIJI_NONE ? "" : bz_yiji_pool[a]);
            snprintf(o->ji[k], sizeof(o->ji[k]), "%s", c == BZ_YIJI_NONE ? "" : bz_yiji_pool[c]);
        }
    }
    return true;
}
