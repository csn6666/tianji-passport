#!/bin/bash
# 端侧排盘引擎对拍:同一批生日,C 引擎 vs server/bazi(lunar_python),逐字段 diff。
# 用法: ./tools/verify_bazi.sh [用例数=2000] [当下时刻]
set -e
cd "$(dirname "$0")/.."
N="${1:-2000}"
NOW="${2:-2026-08-26 12:00}"
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

echo "==== 生成用例(随机 $N 条 + 节气交接点) ===="
server/venv/bin/python - "$N" > "$OUT/cases.txt" <<'EOF'
import random, sys
from datetime import datetime, timedelta
from lunar_python import Lunar

random.seed(20260826)   # 固定种子:每次跑同一批,回归可比
n = int(sys.argv[1])
JIE = ["立春","惊蛰","清明","立夏","芒种","小暑","立秋","白露","寒露","立冬","大雪","小寒"]

# 手工边界
print("2010 1 2 8 0 0 1 12147")
print("1990 3 15 23 30 0 0 11640")   # 晚子时
print("2000 1 1 0 0 1 1 12000")      # 真太阳时倒退到上一年
print("2000 2 29 12 0 0 1 12000")    # 闰日
print("1900 2 5 0 1 0 1 12000")      # 表头附近
print("2099 12 30 23 59 0 0 12000")  # 表尾附近

# 节气交接点两侧 ±1 分钟 —— 年柱/月柱最容易翻车的地方
years = random.sample(range(1902, 2098), min(60, n // 4 + 6))
for y in years:
    tbl = Lunar.fromDate(datetime(y, 6, 1)).getJieQiTable()
    for name in random.sample(JIE, 3):
        v = tbl[name]
        t = datetime(v.getYear(), v.getMonth(), v.getDay(), v.getHour(), v.getMinute())
        for off in (-1, 0, 1):
            x = t + timedelta(minutes=off)
            print(x.year, x.month, x.day, x.hour, x.minute, 0,
                  random.randint(0, 1), 12000)

for _ in range(n):
    y = random.randint(1902, 2098)
    mo = random.randint(1, 12)
    d = random.randint(1, 28)
    h = random.randint(0, 23)
    mi = random.randint(0, 59)
    hu = 1 if random.random() < 0.12 else 0
    male = random.randint(0, 1)
    lng = random.choice([12000, 12147, 11640, 10404, 8710, 12630, 11391, 7500])
    print(y, mo, d, h, mi, hu, male, lng)
EOF

echo "==== Python 基准 ===="
server/venv/bin/python tools/bazi_ref.py --now "$NOW" < "$OUT/cases.txt" \
    > "$OUT/ref.jsonl" 2>"$OUT/ref.err" || { tail -20 "$OUT/ref.err"; exit 1; }

echo "==== 编译端侧引擎(主机) ===="
gcc -std=c99 -O2 -Wall -Wextra -Imain -o "$OUT/probe" \
    tools/bazi_probe.c main/bazi_engine.c main/bazi_tables.c -lm

echo "==== 端侧结果 ===="
"$OUT/probe" --now "$NOW" < "$OUT/cases.txt" > "$OUT/dev.jsonl"

echo "==== 逐字段比对 ===="
python3 - "$OUT/ref.jsonl" "$OUT/dev.jsonl" <<'EOF'
import json, sys
from collections import Counter

ref = [json.loads(l) for l in open(sys.argv[1], encoding="utf-8")]
dev = [json.loads(l) for l in open(sys.argv[2], encoding="utf-8")]
assert len(ref) == len(dev), "行数不一致 %d vs %d" % (len(ref), len(dev))

bad = Counter()
samples = {}
for r, d in zip(ref, dev):
    assert r["case"] == d["case"], (r["case"], d["case"])
    for k in sorted(r):
        if k == "case":
            continue
        a, b = r[k], d.get(k)
        if k == "wx_pct":
            ok = a is not None and b is not None and len(a) == len(b) and \
                 all(abs(x - y) < 0.051 for x, y in zip(a, b))
        elif k == "dayun":
            ok = [list(x) for x in a[:len(b)]] == [list(x) for x in b]
        else:
            ok = a == b
        if not ok:
            bad[k] += 1
            samples.setdefault(k, (r["case"], a, b))

total = len(ref)
print("用例 %d 条" % total)
if not bad:
    print("全部字段一致 ✅")
    sys.exit(0)
print("不一致字段:")
for k, n in bad.most_common():
    case, a, b = samples[k]
    print("  %-16s %5d/%d  例: %s" % (k, n, total, case))
    print("      py  = %r" % (a,))
    print("      dev = %r" % (b,))
sys.exit(1)
EOF
