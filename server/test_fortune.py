#!/usr/bin/env python3
"""排盘引擎 + 解析器测试。

1. 黄金对照:本引擎输出 vs 另一套参照实现(可选,见下)逐字段相等
2. 生日文本解析器覆盖公历/农历/中文数字/缺时辰/时段词
3. 运势规则确定性冒烟
用 server venv 运行: ./venv/bin/python test_fortune.py
"""
import json
import os
import subprocess
import sys

sys.path.insert(0, ".")
from bazi import calculate_bazi_and_dayun
from bazi.parse import parse_birth, birth_to_engine_str, BirthParseError
from bazi.fortune import today_fortune, favorable_element
from bazi.engine import get_bazi_profile

# 可选的黄金对照:拿另一套排盘实现做基准,逐字段比对。
# 不是公开依赖 —— 没配这几个环境变量就跳过第 1 节,其余测试照常跑。
#   BAZI_REF_DIR=/path/to/ref \\
#   BAZI_REF_PY=/path/to/ref/venv/bin/python \\
#   BAZI_REF_IMPORT="mymodule.services:calculate_bazi_and_dayun" \\
#   ./venv/bin/python test_fortune.py
# 端侧 C 引擎的对拍另见 tools/verify_bazi.sh,那个不需要任何外部实现。
ORIG_DIR = os.environ.get("BAZI_REF_DIR")
ORIG_PY = os.environ.get("BAZI_REF_PY")
ORIG_IMPORT = os.environ.get("BAZI_REF_IMPORT", "")

GOLDEN_KEYS = ["eight_char", "day_master", "start_info", "is_forward",
               "current_dayun", "da_yun_list", "hide_gan", "na_yin",
               "shensha", "tai_yuan", "ming_gong"]

CASES = [
    ("1998-03-15 07:30", "男", 120.0),
    ("1990-01-01", "女", 120.0),            # 缺时辰
    ("2000-12-31 23:40", "男", 104.06),     # 晚子时 + 真太阳时(成都经度)
]

passed = failed = 0


def check(name, ok, detail=""):
    global passed, failed
    if ok:
        passed += 1
        print(f"  ✓ {name}")
    else:
        failed += 1
        print(f"  ✗ {name}  {detail}")


print("== 1. 黄金对照参照引擎 ==")
if not (ORIG_DIR and ORIG_PY and ORIG_IMPORT and os.path.exists(ORIG_PY)):
    print("  - 跳过(未设 BAZI_REF_DIR / BAZI_REF_PY / BAZI_REF_IMPORT)。"
          "端侧引擎的对拍见 tools/verify_bazi.sh")
    CASES_GOLDEN = []
else:
    CASES_GOLDEN = CASES
for birthday, gender, lng in CASES_GOLDEN:
    code = (
        "import json,sys,importlib; sys.path.insert(0, %r);"
        "_m,_f = %r.split(':'); f=getattr(importlib.import_module(_m), _f);"
        "r=f(%r,%r,%r); print(json.dumps({k:r[k] for k in %r}, ensure_ascii=False))"
    ) % (ORIG_DIR, ORIG_IMPORT, birthday, gender, lng, GOLDEN_KEYS)
    out = subprocess.run([ORIG_PY, "-c", code], capture_output=True, text=True)
    golden = json.loads(out.stdout.strip().splitlines()[-1])
    ours = calculate_bazi_and_dayun(birthday, gender, lng)
    for k in GOLDEN_KEYS:
        check(f"{birthday} {gender} .{k}", ours[k] == golden[k],
              f"ours={ours[k]!r} golden={golden[k]!r}")

print("== 2. 生日文本解析 ==")
P = [
    ("1998年3月15日早上7点半", dict(year=1998, month=3, day=15, hour=7, minute=30)),
    ("一九九八年三月十五日辰时", dict(year=1998, month=3, day=15, hour=8)),
    ("2000年12月31号晚上11点40分", dict(year=2000, month=12, day=31, hour=23, minute=40)),
    ("90年6月8日中午12点", dict(year=1990, month=6, day=8, hour=12)),
    ("1985年10月1日", dict(year=1985, month=10, day=1, hour_unknown=True)),
    ("我是2003年7月20日凌晨2点出生的", dict(year=2003, month=7, day=20, hour=2)),
]
for text, expect in P:
    try:
        r = parse_birth(text)
        ok = all(r.get(k) == v for k, v in expect.items())
        check(f"解析 {text!r}", ok, f"got={ {k: r.get(k) for k in expect} }")
    except BirthParseError as e:
        check(f"解析 {text!r}", False, f"raised {e}")

# 农历转公历:农历1995年八月初三 = 公历1995-08-28(权威万年历核对)
r = parse_birth("农历1995年8月初三晚上10点")
check("农历95年八月初三→公历8月28日",
      (r["year"], r["month"], r["day"], r["hour"]) == (1995, 8, 28, 22),
      f"got={r['year']}-{r['month']}-{r['day']} {r['hour']}点")

# 解析失败要明确报错而不是猜
try:
    parse_birth("早上七点出生的")
    check("缺年份应报错", False)
except BirthParseError:
    check("缺年份应报错", True)

print("== 3. 运势规则确定性 ==")
r1 = today_fortune("1998-03-15 07:30", "男")
r2 = today_fortune("1998-03-15 07:30", "男")
check("同日重复计算结果一致", r1["today"] == r2["today"])
t = r1["today"]
check("幸运色字段齐全", all(k in t for k in
      ["color_name", "color_rgb", "number", "direction", "yi", "ji", "score", "favorable"]))
check("指数在 30-97", 30 <= t["score"] <= 97, t["score"])
prof = get_bazi_profile("1998-03-15 07:30")
fav = favorable_element(prof)
check("喜神是五行之一", fav["element"] in "木火土金水", fav)
print(f"  (样例: 日主{prof['day_master']} {'强' if fav['strong'] else '弱'} "
      f"喜{fav['element']} 今日{t['liuri']} 色={t['color_name']} 指数={t['score']})")

print(f"\n{'ALL PASS' if failed == 0 else 'FAILED'}: {passed} passed, {failed} failed")
sys.exit(1 if failed else 0)
