"""确定性生日文本解析（不用 LLM——用户明确要求排盘链路全部确定性）。

输入是 ASR 识别出的口语文本，如:
  "1998年3月15日早上7点半" / "一九九八年三月十五日辰时" / "农历九五年八月初三晚上十点"
输出结构化生日(公历)。农历输入用 lunar_python 转公历。解析不到的字段明确报错,
绝不猜测——设备端会把结果回显给用户确认。
"""
import re
from datetime import datetime

import cn2an
from lunar_python import Lunar

# 十二时辰 -> 起始整点(取时辰中点更合理,但命理排盘按时辰归属,取区间内任一小时等价;
# 用区间中点小时,如子时 23-1 取 0)
SHICHEN_HOUR = {
    "子时": 0, "丑时": 2, "寅时": 4, "卯时": 6, "辰时": 8, "巳时": 10,
    "午时": 12, "未时": 14, "申时": 16, "酉时": 18, "戌时": 20, "亥时": 22,
}

# 农历日的"初X/廿X/卅"写法
CN_LUNAR_DAY = {}
_digits = ["一", "二", "三", "四", "五", "六", "七", "八", "九", "十"]
for i, d in enumerate(_digits, 1):
    CN_LUNAR_DAY[f"初{d}"] = i
for i, d in enumerate(_digits[:9], 1):
    CN_LUNAR_DAY[f"十{d}"] = 10 + i
    CN_LUNAR_DAY[f"廿{d}"] = 20 + i
    CN_LUNAR_DAY[f"二十{d}"] = 20 + i
CN_LUNAR_DAY["二十"] = 20
CN_LUNAR_DAY["三十"] = 30
CN_LUNAR_DAY["卅"] = 30


class BirthParseError(ValueError):
    pass


def _normalize(text: str) -> str:
    t = text.replace("两", "二").replace("號", "号").replace("：", ":")
    # 先把农历"初三/廿五"这类替换成数字+日,避免被 cn2an 干扰
    for word in sorted(CN_LUNAR_DAY, key=len, reverse=True):
        t = re.sub(word + r"(?![点时分日号])", f"{CN_LUNAR_DAY[word]}日", t, count=1) \
            if ("初" in word or "廿" in word or "卅" in word) and word in t else t
    try:
        t = cn2an.transform(t, "cn2an")
    except Exception:
        pass  # cn2an 偶发失败时退回原文,数字若本来就是阿拉伯的仍可解析
    return t


def parse_birth(text: str) -> dict:
    """解析口语生日 -> {"year","month","day","hour","minute","hour_unknown","calendar","echo"}

    公历字段 year/month/day 恒为转换后的公历值;calendar 记录用户口述用的历法。
    """
    t = _normalize(text)
    is_lunar = bool(re.search(r"农历|阴历|旧历", t))

    m = re.search(r"(\d{4})\s*年", t)
    if m:
        year = int(m.group(1))
    else:
        m2 = re.search(r"(\d{2})\s*年", t)
        if not m2:
            raise BirthParseError("没听清年份，请说完整年份，比如 1998 年")
        yy = int(m2.group(1))
        year = 1900 + yy if yy >= 30 else 2000 + yy

    m = re.search(r"(\d{1,2})\s*月", t)
    if not m:
        raise BirthParseError("没听清月份")
    month = int(m.group(1))

    m = re.search(r"(\d{1,2})\s*[日号]", t)
    if not m:
        raise BirthParseError("没听清日期(几号)")
    day = int(m.group(1))

    # ── 时辰 ──
    hour = minute = None
    hour_unknown = False
    sc = next((w for w in SHICHEN_HOUR if w in t), None)
    if sc:
        hour, minute = SHICHEN_HOUR[sc], 0
    else:
        # 排除"X日"后紧跟的干扰,取"点/时"格式
        mt = re.search(r"(\d{1,2})\s*[点:](\s*(\d{1,2})\s*分?|半)?", t)
        if mt:
            hour = int(mt.group(1))
            tail = mt.group(2) or ""
            minute = 30 if "半" in tail else int(mt.group(3)) if mt.group(3) else 0
            # 12 小时制修正
            if re.search(r"下午|晚上|傍晚|夜里|夜晚", t) and hour < 12:
                hour += 12
            elif re.search(r"凌晨|半夜", t) and hour == 12:
                hour = 0
            elif re.search(r"中午", t) and hour < 3:   # "中午12点/1点"
                hour += 12
        else:
            hour_unknown = True

    if not (1 <= month <= 12) or not (1 <= day <= 31):
        raise BirthParseError(f"日期数字不对: {month}月{day}日")
    if hour is not None and not (0 <= hour <= 23):
        raise BirthParseError(f"小时数字不对: {hour}点")

    # ── 农历转公历 ──
    calendar = "lunar" if is_lunar else "solar"
    if is_lunar:
        try:
            solar = Lunar.fromYmd(year, month, day).getSolar()
        except Exception as e:
            raise BirthParseError(f"农历日期无效: {year}年{month}月{day}日") from e
        year, month, day = solar.getYear(), solar.getMonth(), solar.getDay()

    # 合法性校验 + 回显文本(公历+农历对照,给设备屏幕确认用)
    try:
        dt = datetime(year, month, day, hour or 0, minute or 0)
    except ValueError as e:
        raise BirthParseError(f"日期无效: {year}-{month}-{day}") from e
    lunar = Lunar.fromDate(dt)
    time_part = "时辰未知" if hour_unknown else f"{hour:02d}:{minute:02d} {lunar.getTimeZhi()}时"
    echo = (f"公历{year}年{month}月{day}日 {time_part}\n"
            f"农历{lunar.getYearInChinese()}年{lunar.getMonthInChinese()}月{lunar.getDayInChinese()}")

    return {
        "year": year, "month": month, "day": day,
        "hour": hour, "minute": minute,
        "hour_unknown": hour_unknown,
        "calendar": calendar,
        "echo": echo,
    }


def birth_to_engine_str(birth: dict) -> str:
    """转成排盘引擎的 birthday 字符串格式。"""
    if birth.get("hour_unknown"):
        return f"{birth['year']:04d}-{birth['month']:02d}-{birth['day']:02d}"
    return (f"{birth['year']:04d}-{birth['month']:02d}-{birth['day']:02d} "
            f"{birth['hour']:02d}:{birth['minute'] or 0:02d}")
