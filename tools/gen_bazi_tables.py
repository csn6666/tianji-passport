#!/usr/bin/env python3
"""从 lunar_python 烘出端侧排盘所需的常量表 -> main/bazi_tables.{h,c}

设备侧不做天文计算:节气时刻、朔日、黄历宜忌全部预先算好塞进 flash,
C 侧只剩查表 + 二分 + 干支取模。表由服务器同一套引擎生成,
因此端侧结果与 server/bazi 逐字段一致(verify_bazi.sh 会验)。

用法: server/venv/bin/python tools/gen_bazi_tables.py
"""
import os
import sys
from datetime import datetime, timedelta

from lunar_python import Lunar, LunarYear, Solar
from lunar_python.util import LunarUtil as L

YEAR_MIN, YEAR_MAX = 1900, 2100
EPOCH = datetime(YEAR_MIN, 1, 1)
JIE = ["立春", "惊蛰", "清明", "立夏", "芒种", "小暑",
       "立秋", "白露", "寒露", "立冬", "大雪", "小寒"]

OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "main")


def collect_jie():
    """全部「节」的精确时刻(秒级),升序。月柱以此为界,年柱以其中的立春为界。"""
    rows = set()
    for y in range(YEAR_MIN - 1, YEAR_MAX + 2):
        for name, v in Lunar.fromDate(datetime(y, 6, 1)).getJieQiTable().items():
            if name in JIE:
                rows.add((datetime(v.getYear(), v.getMonth(), v.getDay(),
                                   v.getHour(), v.getMinute(), v.getSecond()), name))
    rows = sorted(r for r in rows if EPOCH <= r[0] < datetime(YEAR_MAX + 1, 1, 1))
    # 不变量:月干支每节 +1、年干支只在立春 +1。成立才能省掉两张表。
    assert rows[0][1] == "小寒", rows[0]
    for i, (_, name) in enumerate(rows):
        assert JIE[(i + 11) % 12] == name, (i, name)
    return rows


def collect_lunar_months():
    """全部农历月的朔日(公历日)、月序、闰否、农历年。"""
    seen = {}
    for y in range(YEAR_MIN - 1, YEAR_MAX + 2):
        for m in LunarYear.fromYear(y).getMonths():
            s = Solar.fromJulianDay(m.getFirstJulianDay())
            d = datetime(s.getYear(), s.getMonth(), s.getDay())
            seen[d] = (m.getYear(), m.getMonth(), m.getDayCount())
    out = []
    for d in sorted(seen):
        ly, lm, cnt = seen[d]
        if not (EPOCH <= d < datetime(YEAR_MAX + 1, 1, 1)):
            continue
        out.append(((d - EPOCH).days, ly, abs(lm), 1 if lm < 0 else 0, cnt))
    return out


def collect_yiji():
    """(月干支, 日干支) -> 宜/忌 各取前两条。3600 组合全部预先展开。"""
    pool, idx = [], {}

    def pid(word):
        if word not in idx:
            idx[word] = len(pool)
            pool.append(word)
        return idx[word]

    yi = bytearray()
    ji = bytearray()
    for m in range(60):
        for d in range(60):
            a = L.getDayYi(L.JIA_ZI[m], L.JIA_ZI[d])[:2]
            b = L.getDayJi(L.JIA_ZI[m], L.JIA_ZI[d])[:2]
            for k in range(2):
                yi.append(pid(a[k]) if k < len(a) else 0xFF)
                ji.append(pid(b[k]) if k < len(b) else 0xFF)
    assert len(pool) < 255, len(pool)
    return pool, yi, ji


def c_str(s):
    return '"' + "".join("\\x%02x" % b for b in s.encode()) + '"'


def emit_arr(f, ctype, name, values, per_line, fmt="%d"):
    f.write("const %s %s[%d] = {\n" % (ctype, name, len(values)))
    for i in range(0, len(values), per_line):
        f.write("    " + " ".join((fmt + ",") % v for v in values[i:i + per_line]) + "\n")
    f.write("};\n\n")


def main():
    jie = collect_jie()
    lm = collect_lunar_months()
    pool, yi, ji = collect_yiji()

    jie_sec_abs = [int((t - EPOCH).total_seconds()) for t, _ in jie]
    jie_min = [s // 60 for s in jie_sec_abs]
    jie_sec = [s % 60 for s in jie_sec_abs]
    assert max(jie_min) < 2 ** 32
    first_month_gz = L.getJiaZiIndex(
        Solar.fromYmdHms(jie[0][0].year, jie[0][0].month, jie[0][0].day,
                         jie[0][0].hour, jie[0][0].minute + 1, 0)
        .getLunar().getEightChar().getMonth())
    first_year_gz = L.getJiaZiIndex(
        Solar.fromYmdHms(jie[0][0].year, jie[0][0].month, jie[0][0].day,
                         jie[0][0].hour, jie[0][0].minute + 1, 0)
        .getLunar().getEightChar().getYear())

    # 日柱:1900-01-01 的干支序,之后纯取模
    d0 = L.getJiaZiIndex(Solar.fromYmd(1900, 1, 1).getLunar().getEightChar().getDay())

    lm_day = [d for (d, _, _, _, _) in lm]

    h = os.path.join(OUT_DIR, "bazi_tables.h")
    c = os.path.join(OUT_DIR, "bazi_tables.c")

    with open(h, "w", encoding="utf-8") as f:
        f.write("""// main/bazi_tables.h —— 由 tools/gen_bazi_tables.py 生成,勿手改。
// 排盘所需的全部预计算数据:节气时刻、农历月首、黄历宜忌、干支静态表。
#pragma once

#include <stdint.h>

#define BZ_YEAR_MIN %d
#define BZ_YEAR_MAX %d

// ── 节(每年12个,月柱边界;其中立春为年柱边界) ──
// 时刻 = bz_jie_min[i] 分钟 + bz_jie_sec[i] 秒,基准 %d-01-01 00:00:00。
// 索引 i 的节名 = JIE[(i+11)%%12](i=0 为小寒);i%%12==1 的是立春。
#define BZ_JIE_N            %d
#define BZ_JIE_FIRST_MONTH  %d       // 首节之后的月干支序(0..59),之后每节 +1
#define BZ_JIE_FIRST_YEAR   %d       // 首节之后的年干支序(0..59),每过立春 +1
#define BZ_DAY_GZ_EPOCH     %d       // 基准日的日干支序
extern const uint32_t bz_jie_min[BZ_JIE_N];
extern const uint8_t  bz_jie_sec[BZ_JIE_N];

// ── 农历月首(朔日),单位:距基准日的天数 ──
#define BZ_LM_N             %d
extern const uint32_t bz_lm_day[BZ_LM_N];
extern const uint8_t  bz_lm_flag[BZ_LM_N];       // bit0-3 月序1-12, bit4 闰月
extern const uint16_t bz_lm_year[BZ_LM_N];       // 农历年

// ── 黄历宜忌:按 (月干支*60 + 日干支) 索引,各取前两条 ──
#define BZ_YIJI_N           %d
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
""" % (YEAR_MIN, YEAR_MAX, YEAR_MIN,
       len(jie), first_month_gz, first_year_gz, d0,
       len(lm), len(pool)))

    gan = [L.GAN[i + 1] for i in range(10)]
    zhi = [L.ZHI[i + 1] for i in range(12)]
    wx = ["金", "木", "水", "火", "土"]
    gan_wx = [wx.index(L.WU_XING_GAN[g]) for g in gan]
    zhi_wx = [wx.index(L.WU_XING_ZHI[z]) for z in zhi]
    hide = []
    for z in zhi:
        hg = [gan.index(x) for x in L.ZHI_HIDE_GAN[z]]
        hide.append((hg + [0xFF, 0xFF, 0xFF])[:3])

    with open(c, "w", encoding="utf-8") as f:
        f.write('// main/bazi_tables.c —— 由 tools/gen_bazi_tables.py 生成,勿手改。\n')
        f.write('#include "bazi_tables.h"\n\n')
        emit_arr(f, "uint32_t", "bz_jie_min", jie_min, 10)
        emit_arr(f, "uint8_t", "bz_jie_sec", jie_sec, 24)
        emit_arr(f, "uint32_t", "bz_lm_day", lm_day, 12)
        emit_arr(f, "uint8_t", "bz_lm_flag",
                 [m | (lp << 4) for (_, _, m, lp, _) in lm], 20)
        emit_arr(f, "uint16_t", "bz_lm_year", [y for (_, y, _, _, _) in lm], 16)

        f.write("const char *const bz_yiji_pool[BZ_YIJI_N] = {\n")
        for i in range(0, len(pool), 6):
            f.write("    " + " ".join(c_str(w) + "," for w in pool[i:i + 6]) + "\n")
        f.write("};\n\n")
        emit_arr(f, "uint8_t", "bz_day_yi", list(yi), 24, "0x%02x")
        emit_arr(f, "uint8_t", "bz_day_ji", list(ji), 24, "0x%02x")

        for name, arr in (("bz_gan", gan), ("bz_zhi", zhi), ("bz_wuxing", wx),
                          ("bz_nayin", [L.NAYIN[g] for g in L.JIA_ZI]),
                          ("bz_xunkong", [L.XUN_KONG[i // 10] for i in range(60)])):
            f.write("const char *const %s[%d] = {\n" % (name, len(arr)))
            for i in range(0, len(arr), 6):
                f.write("    " + " ".join(c_str(w) + "," for w in arr[i:i + 6]) + "\n")
            f.write("};\n\n")

        emit_arr(f, "uint8_t", "bz_gan_wx", gan_wx, 10)
        emit_arr(f, "uint8_t", "bz_zhi_wx", zhi_wx, 12)
        f.write("const uint8_t bz_zhi_hide[12][3] = {\n")
        for h3 in hide:
            f.write("    { %s },\n" % ", ".join("0x%02x" % x for x in h3))
        f.write("};\n\n")

        for name, arr in (("bz_digit", ["零"] + list(L.NUMBER[1:10])),
                          ("bz_lmonth", list(L.MONTH)),
                          ("bz_lday", list(L.DAY))):
            f.write("const char *const %s[%d] = {\n" % (name, len(arr)))
            for i in range(0, len(arr), 6):
                f.write("    " + " ".join(c_str(w) + "," for w in arr[i:i + 6]) + "\n")
            f.write("};\n\n")

        f.write("const char *const bz_shishen[10][10] = {\n")
        for dg in gan:
            row = [L.SHI_SHEN[dg + og] for og in gan]
            f.write("    { " + " ".join(c_str(w) + "," for w in row) + " },\n")
        f.write("};\n")

    print("节 %d 条, 农历月 %d 条, 宜忌词 %d 个" % (len(jie), len(lm), len(pool)))
    for p in (h, c):
        print("  %s  %.1f KB" % (p, os.path.getsize(p) / 1024))


if __name__ == "__main__":
    main()
