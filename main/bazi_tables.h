// main/bazi_tables.h —— 由 tools/gen_bazi_tables.py 生成,勿手改。
// 排盘所需的全部预计算数据:节气时刻、农历月首、黄历宜忌、干支静态表。
#pragma once

#include <stdint.h>

#define BZ_YEAR_MIN 1900
#define BZ_YEAR_MAX 2100

// ── 节(每年12个,月柱边界;其中立春为年柱边界) ──
// 时刻 = bz_jie_min[i] 分钟 + bz_jie_sec[i] 秒,基准 1900-01-01 00:00:00。
// 索引 i 的节名 = JIE[(i+11)%12](i=0 为小寒);i%12==1 的是立春。
#define BZ_JIE_N            2412
#define BZ_JIE_FIRST_MONTH  13       // 首节之后的月干支序(0..59),之后每节 +1
#define BZ_JIE_FIRST_YEAR   35       // 首节之后的年干支序(0..59),每过立春 +1
#define BZ_DAY_GZ_EPOCH     10       // 基准日的日干支序
extern const uint32_t bz_jie_min[BZ_JIE_N];
extern const uint8_t  bz_jie_sec[BZ_JIE_N];

// ── 农历月首(朔日),单位:距基准日的天数 ──
#define BZ_LM_N             2487
extern const uint32_t bz_lm_day[BZ_LM_N];
extern const uint8_t  bz_lm_flag[BZ_LM_N];       // bit0-3 月序1-12, bit4 闰月
extern const uint16_t bz_lm_year[BZ_LM_N];       // 农历年

// ── 黄历宜忌:按 (月干支*60 + 日干支) 索引,各取前两条 ──
#define BZ_YIJI_N           93
#define BZ_YIJI_NONE        0xFF
extern const char *const bz_yiji_pool[BZ_YIJI_N];
extern const uint8_t bz_day_yi[3600 * 2];
extern const uint8_t bz_day_ji[3600 * 2];

// ── 干支静态表(UTF-8,每字3字节) ──
extern const char *const bz_gan[10];
extern const char *const bz_zhi[12];
extern const char *const bz_wuxing[5];           // 金木水火土
extern const uint8_t bz_gan_wx[10];              // 天干五行(索引 bz_wuxing)
extern const uint8_t bz_zhi_wx[12];              // 地支五行
extern const uint8_t bz_zhi_hide[12][3];         // 藏干(天干序),0xFF 结束
extern const char *const bz_nayin[60];
extern const char *const bz_xunkong[60];
extern const char *const bz_shishen[10][10];     // [日干][他干]

// ── 农历中文(设备字库无 U+3007「〇」,年份数字直接用「零」) ──
extern const char *const bz_digit[10];           // 零一二三四五六七八九
extern const char *const bz_lmonth[13];          // 正二…十冬腊
extern const char *const bz_lday[31];            // 初一…三十
