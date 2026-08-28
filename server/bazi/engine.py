"""
八字排盘引擎:真太阳时换算、四柱、精确起运、大运/流年。

口径以 lunar_python 为准;端侧的 C 版本(main/bazi_engine.c)与本文件逐字段对拍。
"""
import math
import logging
from datetime import datetime, timedelta
from lunar_python import Lunar, Solar

from .bazi_shensha import compute_shensha, compute_relations, compute_dynamic_relations

logger = logging.getLogger(__name__)

# 中国标准时区经度 (东经120度)
STANDARD_LONGITUDE = 120.0


def _equation_of_time(day_of_year: int) -> float:
    """
    计算均时差（分钟）
    使用天文学近似公式

    Args:
        day_of_year: 一年中的第几天 (1-366)
    Returns:
        均时差，单位为分钟
    """
    B = 2 * math.pi * (day_of_year - 81) / 365.0
    eot = (
        9.87 * math.sin(2 * B)
        - 7.53 * math.cos(B)
        - 1.5 * math.sin(B)
    )
    return eot


def convert_to_true_solar_time(birthday_dt: datetime, longitude: float) -> datetime:
    """
    将北京时间转换为真太阳时

    真太阳时 = 北京时间 + 经度修正 + 均时差
    经度修正 = (出生地经度 - 120°) × 4分钟/度

    Args:
        birthday_dt: 北京时间的出生时间
        longitude: 出生地经度（东经为正）
    Returns:
        真太阳时的datetime
    """
    day_of_year = birthday_dt.timetuple().tm_yday

    # 经度修正（分钟）
    longitude_correction = (longitude - STANDARD_LONGITUDE) * 4.0

    # 均时差（分钟）
    eot = _equation_of_time(day_of_year)

    # 总修正量
    total_correction_minutes = longitude_correction + eot

    true_solar_dt = birthday_dt + timedelta(minutes=total_correction_minutes)

    logger.info(
        f"真太阳时换算: 北京时间={birthday_dt}, 经度={longitude}, "
        f"经度修正={longitude_correction:.1f}分钟, 均时差={eot:.1f}分钟, "
        f"真太阳时={true_solar_dt}"
    )
    return true_solar_dt


def _build_chart_extras(ec, hour_unknown: bool) -> dict:
    """从 EightChar 抽取藏干/十神/纳音/十二长生/旬空/胎元/命宫等衍生项

    返回的字段都按"四柱位置"组织，时柱在 hour_unknown 时统一置 None。
    """
    pillars = ["year", "month", "day", "time"]
    pillar_zh = {"year": "年", "month": "月", "day": "日", "time": "时"}

    def _per_pillar(method_name):
        out = {}
        for p in pillars:
            if hour_unknown and p == "time":
                out[p] = None
                continue
            method = getattr(ec, f"get{p.capitalize()}{method_name}")
            out[p] = method()
        return out

    # 藏干（list）
    hide_gan = _per_pillar("HideGan")
    # 天干十神（日主自己不论）
    shi_shen_gan = {
        "year": ec.getYearShiShenGan(),
        "month": ec.getMonthShiShenGan(),
        "day": None,  # 日主
        "time": None if hour_unknown else ec.getTimeShiShenGan(),
    }
    # 藏干十神（list, 与 hide_gan 一一对应）
    shi_shen_zhi = _per_pillar("ShiShenZhi")
    # 纳音
    na_yin = _per_pillar("NaYin")
    # 十二长生（地势，以日主为基准）
    di_shi = _per_pillar("DiShi")

    # 旬空：以日柱起例，对照其他三柱地支判定
    day_xun_kong = ec.getDayXunKong()  # e.g. "子丑"
    branches = {
        "year": ec.getYearZhi(),
        "month": ec.getMonthZhi(),
        "day": ec.getDayZhi(),
        "time": None if hour_unknown else ec.getTimeZhi(),
    }
    kong_wang = []  # 哪些柱空亡
    for p in ["year", "month", "time"]:  # 日柱本身不论自身空亡
        zhi = branches[p]
        if zhi and zhi in day_xun_kong:
            kong_wang.append(f"{pillar_zh[p]}支{zhi}")

    # 胎元 / 命宫（时辰未知时命宫不准，跳过）
    tai_yuan = ec.getTaiYuan()
    ming_gong = None if hour_unknown else ec.getMingGong()

    # 神煞（命理类，自写查表）
    four_gan = {
        "year": ec.getYearGan(),
        "month": ec.getMonthGan(),
        "day": ec.getDayGan(),
        "time": None if hour_unknown else ec.getTimeGan(),
    }
    shensha = compute_shensha(ec.getDayGan(), ec.getYearZhi(), branches)
    relations = compute_relations(four_gan, branches)

    return {
        "hide_gan": hide_gan,
        "shi_shen_gan": shi_shen_gan,
        "shi_shen_zhi": shi_shen_zhi,
        "na_yin": na_yin,
        "di_shi": di_shi,
        "day_xun_kong": day_xun_kong,
        "kong_wang_pillars": kong_wang,
        "tai_yuan": tai_yuan,
        "ming_gong": ming_gong,
        "shensha": shensha,
        "gan_zhi_relations": relations,
    }


def calculate_bazi_and_dayun(birthday: str, gender: str, longitude: float) -> dict:
    """
    计算八字和大运（使用真太阳时）

    Args:
        birthday: 阳历出生时间 'YYYY-MM-DD HH:MM'
        gender: '男' 或 '女'
        longitude: 出生地经度
    Returns:
        包含八字、大运等信息的字典
    """
    # 解析时间
    birthday_dt = None
    hour_unknown = False
    for fmt in ["%Y-%m-%d %H:%M:%S", "%Y-%m-%d %H:%M"]:
        try:
            birthday_dt = datetime.strptime(birthday, fmt)
            break
        except ValueError:
            continue
    if birthday_dt is None:
        # 仅有日期，无时辰
        try:
            birthday_dt = datetime.strptime(birthday, "%Y-%m-%d")
            hour_unknown = True
        except ValueError:
            raise ValueError(f"无法解析日期: {birthday}")

    # 真太阳时换算
    true_solar_dt = convert_to_true_solar_time(birthday_dt, longitude)

    # 用真太阳时计算八字
    birthday_lunar = Lunar.fromDate(true_solar_dt)
    ec = birthday_lunar.getEightChar()

    year_pillar = ec.getYear()
    month_pillar = ec.getMonth()
    day_pillar = ec.getDay()
    hour_pillar = ec.getTime()
    if hour_unknown:
        eight_char = f"{year_pillar} {month_pillar} {day_pillar} ？？"
    else:
        eight_char = f"{year_pillar} {month_pillar} {day_pillar} {hour_pillar}"

    # 日主
    day_master = ec.getDayGan()

    # ── 排盘衍生项 ──
    chart_extras = _build_chart_extras(ec, hour_unknown)

    # 大运计算 (1=男, 0=女)
    gender_flag = 1 if gender == "男" else 0
    yun = ec.getYun(gender_flag)

    # 起运信息
    start_info = f"{yun.getStartYear()}年{yun.getStartMonth()}月{yun.getStartDay()}日起运"
    is_forward = yun.isForward()

    # 大运列表（精确到月日）
    da_yun_list = yun.getDaYun()
    start_solar = yun.getStartSolar()
    # 起运精确日期（第一个有效大运的起始日）
    yun_start_month = start_solar.getMonth()
    yun_start_day = start_solar.getDay()

    da_yun_items = []
    for dy in da_yun_list:
        gz = dy.getGanZhi()
        if gz:
            # 计算精确的起始和结束日期（月日与起运日相同）
            start_y = dy.getStartYear()
            end_y = dy.getEndYear()
            da_yun_items.append({
                "ganZhi": gz,
                "startAge": dy.getStartAge(),
                "startYear": start_y,
                "endYear": end_y,
                "startMonth": yun_start_month,
                "startDay": yun_start_day,
            })

    # 用精确日期判断当前所在大运
    now = datetime.now()
    current_date = (now.year, now.month, now.day)
    current_dayun = ""
    for i, item in enumerate(da_yun_items):
        start_date = (item["startYear"], item["startMonth"], item["startDay"])
        # 下一个大运的起始日作为当前大运的结束日
        if i + 1 < len(da_yun_items):
            end_date = (da_yun_items[i + 1]["startYear"], da_yun_items[i + 1]["startMonth"], da_yun_items[i + 1]["startDay"])
        else:
            end_date = (item["endYear"] + 1, item["startMonth"], item["startDay"])
        if start_date <= current_date < end_date:
            current_dayun = item["ganZhi"]
            break

    # 当前流年流月流日及公历农历
    now_solar = Solar.fromYmdHms(now.year, now.month, now.day, now.hour, now.minute, 0)
    now_lunar = now_solar.getLunar()
    now_ec = now_lunar.getEightChar()

    current_liunian = now_ec.getYear()   # 流年
    current_liuyue = now_ec.getMonth()   # 流月
    current_liuri = now_ec.getDay()      # 流日

    solar_date_str = str(now_solar)  # 公历 YYYY-MM-DD
    lunar_date_str = f"{now_lunar.getYearInChinese()}年{now_lunar.getMonthInChinese()}月{now_lunar.getDayInChinese()}"

    # 动态干支关系（流年/大运/流月 vs 四柱）
    four_gan_for_dyn = {
        "year": ec.getYearGan(),
        "month": ec.getMonthGan(),
        "day": ec.getDayGan(),
        "time": None if hour_unknown else ec.getTimeGan(),
    }
    four_zhi_for_dyn = {
        "year": ec.getYearZhi(),
        "month": ec.getMonthZhi(),
        "day": ec.getDayZhi(),
        "time": None if hour_unknown else ec.getTimeZhi(),
    }
    dyn_extras = {}
    if current_liunian:
        dyn_extras["流年"] = current_liunian
    if current_dayun:
        dyn_extras["大运"] = current_dayun
    if current_liuyue:
        dyn_extras["流月"] = current_liuyue
    chart_extras["dynamic_relations"] = compute_dynamic_relations(
        four_gan_for_dyn, four_zhi_for_dyn, dyn_extras
    )

    result = {
        "eight_char": eight_char,
        "year_pillar": year_pillar,
        "month_pillar": month_pillar,
        "day_pillar": day_pillar,
        "hour_pillar": hour_pillar,
        "day_master": day_master,
        "true_solar_time": true_solar_dt.strftime("%Y-%m-%d %H:%M"),
        "original_time": birthday_dt.strftime("%Y-%m-%d %H:%M"),
        "start_info": start_info,
        "is_forward": is_forward,
        "da_yun_list": da_yun_items,
        "current_dayun": current_dayun,
        "gender": gender,
        "hour_unknown": hour_unknown,
        "current_liunian": current_liunian,
        "current_liuyue": current_liuyue,
        "current_liuri": current_liuri,
        "solar_date": solar_date_str,
        "lunar_date": lunar_date_str,
        **_liuyue_transition_info(now_lunar),
        **chart_extras,
    }

    logger.info(f"八字计算结果: {eight_char}, 日主: {day_master}, 当前大运: {current_dayun}")
    return result


def _liuyue_transition_info(lunar) -> dict:
    """下一个「节」及交节后的流月干支（渊海子平：流月以节为界，非公历/农历月首）。

    交节当天 00:00 仍属旧月（节气有精确时刻），故用交节次日中午取新流月干支。
    """
    next_jie = lunar.getNextJie()
    jie_solar = next_jie.getSolar()
    jie_date = datetime(jie_solar.getYear(), jie_solar.getMonth(), jie_solar.getDay())
    after = jie_date + timedelta(days=1)
    next_liuyue = (
        Solar.fromYmdHms(after.year, after.month, after.day, 12, 0, 0)
        .getLunar().getEightChar().getMonth()
    )
    return {
        "next_jie_name": next_jie.getName(),
        "next_jie_date": jie_date.strftime("%Y-%m-%d"),
        "next_liuyue": next_liuyue,
    }


def format_liuyue_note(info: dict) -> str:
    """流月展示文本：'乙未（2026-08-07 立秋交节后进入丙申月）'"""
    note = info.get("current_liuyue", "")
    if info.get("next_liuyue"):
        note += f"（{info['next_jie_date']} {info['next_jie_name']}交节后进入{info['next_liuyue']}月）"
    return note


def get_current_date_info() -> dict:
    """返回"当下"的日期 / 流年 / 流月 / 流日信息。

    用于多轮对话里每一轮重新锚定"今天"，避免首轮 system_prompt 里的日期被缓存过时。
    """
    now = datetime.now()
    now_solar = Solar.fromYmdHms(now.year, now.month, now.day, now.hour, now.minute, 0)
    now_lunar = now_solar.getLunar()
    now_ec = now_lunar.getEightChar()
    return {
        "solar_date": str(now_solar),
        "lunar_date": f"{now_lunar.getYearInChinese()}年{now_lunar.getMonthInChinese()}月{now_lunar.getDayInChinese()}",
        "current_liunian": now_ec.getYear(),
        "current_liuyue": now_ec.getMonth(),
        "current_liuri": now_ec.getDay(),
        **_liuyue_transition_info(now_lunar),
    }


def _format_dayun_text(bazi_result: dict) -> str:
    """格式化大运列表文本"""
    text = ""
    for item in bazi_result["da_yun_list"]:
        marker = " ← 当前大运" if item["ganZhi"] == bazi_result["current_dayun"] else ""
        start_m = item.get("startMonth", 1)
        start_d = item.get("startDay", 1)
        text += f"  {item['ganZhi']}: {item['startAge']}岁起 ({item['startYear']}年{start_m}月{start_d}日 - {item['endYear']}年){marker}\n"
    return text


def _format_chart_extras_text(bazi_result: dict) -> str:
    """格式化排盘衍生项（藏干/十神/纳音/十二长生/旬空/胎元/命宫）"""
    if "hide_gan" not in bazi_result:
        return ""

    hide = bazi_result["hide_gan"]
    ssg = bazi_result["shi_shen_gan"]
    ssz = bazi_result["shi_shen_zhi"]
    nay = bazi_result["na_yin"]
    ds = bazi_result["di_shi"]

    pillars = [
        ("year", "年柱", bazi_result["year_pillar"]),
        ("month", "月柱", bazi_result["month_pillar"]),
        ("day", "日柱", bazi_result["day_pillar"]),
        ("time", "时柱", bazi_result["hour_pillar"]),
    ]

    lines = []
    for p, label, gz in pillars:
        if hide.get(p) is None:
            lines.append(f"  {label} {gz}：（时辰未知，略）")
            continue
        # 天干十神（日主写"日主"）
        gan_ss = "日主" if p == "day" else ssg.get(p) or "-"
        # 藏干 + 藏干十神 按位 zip
        hide_list = hide[p] or []
        ssz_list = ssz.get(p) or []
        hide_str = "/".join(f"{g}({s})" for g, s in zip(hide_list, ssz_list)) or "-"
        lines.append(
            f"  {label} {gz}：天干十神={gan_ss}；藏干={hide_str}；纳音={nay.get(p, '-')}；十二长生={ds.get(p, '-')}"
        )

    chart_text = "\n".join(lines)

    # 旬空
    xun_kong = bazi_result.get("day_xun_kong", "")
    kong_pillars = bazi_result.get("kong_wang_pillars", [])
    if kong_pillars:
        kong_text = f"日柱旬空={xun_kong}；空亡柱：{', '.join(kong_pillars)}"
    else:
        kong_text = f"日柱旬空={xun_kong}；其他三柱地支均不空"

    tai_yuan = bazi_result.get("tai_yuan", "-")
    ming_gong = bazi_result.get("ming_gong") or "（时辰未知，略）"

    # 神煞
    shensha = bazi_result.get("shensha") or {}
    if shensha:
        ss_lines = [f"    {name}：{', '.join(hits)}" for name, hits in shensha.items()]
        ss_text = "  神煞：\n" + "\n".join(ss_lines)
    else:
        ss_text = "  神煞：无"

    # 干支关系（四柱内部）
    relations = bazi_result.get("gan_zhi_relations") or {}
    if relations:
        rel_lines = [f"    {name}：{'; '.join(hits)}" for name, hits in relations.items()]
        rel_text = "  四柱干支关系：\n" + "\n".join(rel_lines)
    else:
        rel_text = "  四柱干支关系：无"

    # 动态关系（流年/大运/流月 vs 四柱）
    dyn = bazi_result.get("dynamic_relations") or {}
    if dyn:
        dyn_lines = [f"    {name}：{'; '.join(hits)}" for name, hits in dyn.items()]
        dyn_text = "  动态关系（流年/大运/流月 vs 四柱）：\n" + "\n".join(dyn_lines)
    else:
        dyn_text = "  动态关系：无"

    return (
        "- 排盘衍生（系统已算好，直接使用）：\n"
        f"{chart_text}\n"
        f"  旬空：{kong_text}\n"
        f"  胎元：{tai_yuan}；命宫：{ming_gong}\n"
        f"{ss_text}\n"
        f"{rel_text}\n"
        f"{dyn_text}"
    )


# ═══ 加权五行档案(幸运色判定的基础) ═══
from collections import defaultdict
from typing import Dict, List, Optional

GAN_WUXING = {
    "甲": "木", "乙": "木", "丙": "火", "丁": "火", "戊": "土",
    "己": "土", "庚": "金", "辛": "金", "壬": "水", "癸": "水",
}

ALL_WUXING = ["金", "木", "水", "火", "土"]

# 天干位置权重
GAN_WEIGHTS = {
    "year": 7,
    "month": 10,
    "day": 10,
    "time": 7,
}

# 地支藏干权重：(本气, 中气, 余气)
ZHI_HIDE_WEIGHTS = {
    "year":  (7, 3, 1),
    "month": (15, 5, 3),
    "day":   (10, 4, 2),
    "time":  (7, 3, 1),
}


def _to_true_solar_time(dt: datetime, longitude: float) -> datetime:
    """
    将北京时间转换为真太阳时

    真太阳时 = 北京时间 + 经度时差 + 均时差
    - 经度时差 = (longitude - 120) * 4 分钟
    - 均时差(Equation of Time): 由地球椭圆轨道和地轴倾斜引起的修正
    """
    # 经度时差
    lng_correction = (longitude - 120) * 4  # 分钟

    # 均时差 (Spencer 近似公式)
    day_of_year = dt.timetuple().tm_yday
    B = 2 * math.pi * (day_of_year - 81) / 365
    eot = 9.87 * math.sin(2 * B) - 7.53 * math.cos(B) - 1.5 * math.sin(B)  # 分钟

    total_adj = lng_correction + eot
    result = dt + timedelta(minutes=total_adj)
    logger.info("真太阳时: %s -> %s (经度%.1f, 经度差%.1fmin, 均时差%.1fmin)",
                dt.strftime("%H:%M"), result.strftime("%H:%M"), longitude, lng_correction, eot)
    return result


def get_bazi_profile(birthday: str, longitude: Optional[float] = None) -> dict:
    """
    根据阳历出生时间计算八字五行档案

    五行占比算法：天干按位置赋权，地支按藏干的本气/中气/余气赋权，
    月支权重最大（代表月令），加总后换算百分比。

    Args:
        birthday: 阳历出生时间（北京时间），支持 'YYYY-MM-DD HH:MM:SS' / 'YYYY-MM-DD HH:MM' / 'YYYY-MM-DD'
        longitude: 出生地经度，用于计算真太阳时。None 则默认 120（即北京时间不校正）

    Returns:
        dict: {
            "eight_char": "乙亥 壬午 丁丑 乙巳",
            "wu_xing": { ... }
        }
    """
    # 解析日期
    birthday_dt = None
    has_time = False
    for fmt in ["%Y-%m-%d %H:%M:%S", "%Y-%m-%d %H:%M", "%Y-%m-%d"]:
        try:
            birthday_dt = datetime.strptime(birthday, fmt)
            has_time = fmt != "%Y-%m-%d"
            break
        except ValueError:
            continue
    if birthday_dt is None:
        raise ValueError(f"无法解析日期格式: {birthday}")

    # 真太阳时校正（仅在提供了具体时间时才校正，否则午夜校正会推回前一天导致日柱错误）
    if longitude is not None and has_time:
        birthday_dt = _to_true_solar_time(birthday_dt, longitude)

    lunar = Lunar.fromDate(birthday_dt)
    ec = lunar.getEightChar()

    eight_char = f"{ec.getYear()} {ec.getMonth()} {ec.getDay()} {ec.getTime()}"

    # 日主五行
    day_master_wx = GAN_WUXING[ec.getDayGan()]

    # ── 加权统计五行 ──
    wx_score: Dict[str, float] = defaultdict(float)

    # 天干权重
    gan_data = [
        ("year", ec.getYearGan()),
        ("month", ec.getMonthGan()),
        ("day", ec.getDayGan()),
        ("time", ec.getTimeGan()),
    ]
    for pos, gan in gan_data:
        wx_score[GAN_WUXING[gan]] += GAN_WEIGHTS[pos]

    # 地支藏干权重（本气→中气→余气）
    zhi_hide_data = [
        ("year", ec.getYearHideGan()),
        ("month", ec.getMonthHideGan()),
        ("day", ec.getDayHideGan()),
        ("time", ec.getTimeHideGan()),
    ]
    for pos, hide_gans in zhi_hide_data:
        weights = ZHI_HIDE_WEIGHTS[pos]
        for i, gan in enumerate(hide_gans):
            w = weights[i] if i < len(weights) else 0
            wx_score[GAN_WUXING[gan]] += w

    # 换算百分比
    total_score = sum(wx_score.values())

    # ── 收集每个五行对应的十神 ──
    wx_shi_shen: Dict[str, List[str]] = defaultdict(list)

    # 天干十神（日干为"日主"，跳过）
    gan_shi_shens = [
        (ec.getYearGan(), ec.getYearShiShenGan()),
        (ec.getMonthGan(), ec.getMonthShiShenGan()),
        (ec.getTimeGan(), ec.getTimeShiShenGan()),
    ]
    for gan, ss in gan_shi_shens:
        wx = GAN_WUXING[gan]
        if ss not in wx_shi_shen[wx]:
            wx_shi_shen[wx].append(ss)

    # 地支十神（藏干与十神一一对应，按藏干的五行归类）
    zhi_shi_shen_data = [
        (ec.getYearHideGan(), ec.getYearShiShenZhi()),
        (ec.getMonthHideGan(), ec.getMonthShiShenZhi()),
        (ec.getDayHideGan(), ec.getDayShiShenZhi()),
        (ec.getTimeHideGan(), ec.getTimeShiShenZhi()),
    ]
    for hide_gans, shi_shens in zhi_shi_shen_data:
        for gan, ss in zip(hide_gans, shi_shens):
            wx = GAN_WUXING[gan]
            if ss not in wx_shi_shen[wx]:
                wx_shi_shen[wx].append(ss)

    # 组装结果
    wx_to_en = {"金": "metal", "木": "wood", "水": "water", "火": "fire", "土": "earth"}
    wu_xing = {}
    for wx in ALL_WUXING:
        score = wx_score.get(wx, 0)
        wu_xing[wx_to_en[wx]] = {
            "percentage": round(score / total_score * 100, 1) if total_score > 0 else 0.0,
            "is_day_master": wx == day_master_wx,
            "shi_shen_list": wx_shi_shen.get(wx, []),
        }

    logger.info("get_bazi_profile: birthday=%s, eight_char=%s, day_master=%s", birthday, eight_char, day_master_wx)

    day_gan = ec.getDayGan()
    day_master = day_gan + GAN_WUXING[day_gan]

    return {
        "eight_char": eight_char,
        "wu_xing": wu_xing,
        "day_master": day_master,
    }
