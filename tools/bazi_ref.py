#!/usr/bin/env python3
"""用 server/bazi 那套引擎吐出对拍基准。

stdin 每行一个用例: YYYY MM DD HH MI hour_unknown male lng100
"当下"由 --now 固定,否则 engine.py 里的 datetime.now() 会让结果不可复现。

用法: server/venv/bin/python tools/bazi_ref.py --now "2026-08-26 12:00" < cases.txt
"""
import argparse
import json
import os
import sys
from datetime import datetime

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "server"))


def install_fake_now(fixed: datetime):
    """把 engine/fortune 里的 datetime.now() 钉死,否则每次跑结果都在漂。"""
    class FakeDateTime(datetime):
        @classmethod
        def now(cls, tz=None):
            return fixed

    import bazi.engine as E
    import bazi.fortune as F
    E.datetime = FakeDateTime
    F.datetime = FakeDateTime


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--now", required=True)
    a = ap.parse_args()
    fixed = datetime.strptime(a.now, "%Y-%m-%d %H:%M")
    install_fake_now(fixed)

    from bazi.fortune import today_fortune, WX_EN

    for line in sys.stdin:
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        y, mo, d, h, mi, hu, male, lng100 = (int(x) for x in line.split())
        if hu:
            birth = "%04d-%02d-%02d" % (y, mo, d)
        else:
            birth = "%04d-%02d-%02d %02d:%02d" % (y, mo, d, h, mi)
        res = today_fortune(birth, "男" if male else "女", lng100 / 100.0)
        c, t = res["chart"], res["today"]

        # 设备字库没有 U+3007「〇」,服务器发下去前也会换成「零」,对拍时对齐
        lunar_date = c["lunar_date"].replace("〇", "零")

        out = {
            "case": line,
            "eight_char": c["eight_char"],
            "pillars": [c["year_pillar"], c["month_pillar"], c["day_pillar"], c["hour_pillar"]],
            "day_master": c["day_master"],
            "true_solar": c["true_solar_time"],
            "start_info": c["start_info"],
            "is_forward": c["is_forward"],
            "dayun": [[x["ganZhi"], x["startAge"], x["startYear"], x["endYear"]]
                      for x in c["da_yun_list"]],
            "current_dayun": c["current_dayun"],
            "liunian": c["current_liunian"],
            "liuyue": c["current_liuyue"],
            "liuri": c["current_liuri"],
            "solar_date": c["solar_date"],
            "lunar_date": lunar_date,
            "next_jie_name": c["next_jie_name"],
            "next_jie_date": c["next_jie_date"],
            "next_liuyue": c["next_liuyue"],
            "wx_pct": [res["chart"] and 0][:0] or None,
            "strong": t["day_master_strong"],
            "favorable": t["favorable"],
            "score": t["score"],
            "color_name": t["color_name"],
            "number": t["number"],
            "direction": t["direction"],
            "liuri_relation": t["liuri_relation"],
            "yi": list(t["yi"]),
            "ji": list(t["ji"]),
        }
        # 五行百分比按 金木水火土 顺序
        from bazi.engine import get_bazi_profile
        lng = lng100 / 100.0
        prof = get_bazi_profile(birth, lng if lng != 120.0 else None)
        out["wx_pct"] = [prof["wu_xing"][WX_EN[z]]["percentage"] for z in "金木水火土"]

        print(json.dumps(out, ensure_ascii=False, sort_keys=True))


if __name__ == "__main__":
    main()
