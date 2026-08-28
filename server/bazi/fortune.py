"""今日运势:确定性规则为主,LLM 只写解盘文案(在 voice_server 侧调用)。

幸运色口径(简化规则,娱乐向,写死在代码里保证同一天结果稳定):
  1. 用 engine.get_bazi_profile 的加权五行百分比(月令权重最高)判日主强弱:
     同党(日主五行 + 生日主的五行) 占比 >= 50% 为强,否则为弱
  2. 弱 -> 喜生扶:在 [生我(印), 同我(比劫)] 里选占比更低者(缺谁补谁)
     强 -> 喜克泄耗:在 [克我(官), 我生(食伤), 我克(财)] 里选占比最低者
  3. 喜神五行 -> 色系/数字(河图数)/方位
  4. 每日变化:流日干支五行与喜神的生克关系 -> 指数加减与文案素材
"""
from datetime import datetime

from lunar_python import Lunar, Solar

from .engine import GAN_WUXING, calculate_bazi_and_dayun, get_bazi_profile

WX_EN = {"木": "wood", "火": "fire", "土": "earth", "金": "metal", "水": "water"}
WX_SHENG = {"木": "火", "火": "土", "土": "金", "金": "水", "水": "木"}   # 我生
WX_KE = {"木": "土", "土": "水", "水": "火", "火": "金", "金": "木"}     # 我克

WX_COLOR = {
    "木": {"name": "翠竹青", "rgb": 0x2E8B57},
    "火": {"name": "朱砂红", "rgb": 0xC3272B},
    "土": {"name": "琥珀黄", "rgb": 0xD9A404},
    "金": {"name": "鎏金白", "rgb": 0xE8E3D3},
    "水": {"name": "玄墨蓝", "rgb": 0x1F4E79},
}
WX_NUMBER = {"水": "一·六", "火": "二·七", "木": "三·八", "金": "四·九", "土": "五·十"}  # 河图数
WX_DIRECTION = {"木": "东", "火": "南", "土": "中宫", "金": "西", "水": "北"}

ZHI_WUXING = {"寅": "木", "卯": "木", "巳": "火", "午": "火", "申": "金", "酉": "金",
              "亥": "水", "子": "水", "辰": "土", "戌": "土", "丑": "土", "未": "土"}


def char_color(c: str) -> int:
    """干支单字 -> 五行显示色(设备命盘页直接渲染)。未知字给鎏金。"""
    wx = GAN_WUXING.get(c) or ZHI_WUXING.get(c)
    return WX_COLOR[wx]["rgb"] if wx else 0xD4AF37


def _sheng_me(wx: str) -> str:
    """生我者(印)"""
    return next(k for k, v in WX_SHENG.items() if v == wx)


def favorable_element(profile: dict) -> dict:
    """由五行档案推喜神。返回 {"element", "strong", "tong_dang_pct"}"""
    day_wx = profile["day_master"][-1]          # 如 "辛金" -> "金"
    pct = {zh: profile["wu_xing"][en]["percentage"] for zh, en in WX_EN.items()}

    yin = _sheng_me(day_wx)                     # 印
    tong_dang = pct[day_wx] + pct[yin]
    strong = tong_dang >= 50.0

    if strong:
        # 官(克我) / 食伤(我生) / 财(我克) 里选最缺的
        candidates = [next(k for k, v in WX_KE.items() if v == day_wx),
                      WX_SHENG[day_wx], WX_KE[day_wx]]
    else:
        candidates = [yin, day_wx]
    fav = min(candidates, key=lambda w: pct[w])
    return {"element": fav, "strong": strong, "tong_dang_pct": round(tong_dang, 1),
            "day_wx": day_wx}


def _relation_to(fav: str, other: str) -> str:
    """other 五行对喜神 fav 的作用: 同/生/克/被生(泄)/被克(耗)"""
    if other == fav:
        return "同"
    if WX_SHENG[other] == fav:
        return "生"
    if WX_KE[other] == fav:
        return "克"
    if WX_SHENG[fav] == other:
        return "泄"
    return "耗"


def today_fortune(birth_str: str, gender: str, longitude: float = 120.0) -> dict:
    """完整排盘 + 今日运势(确定性部分)。返回 chart 与 today 两块。"""
    chart = calculate_bazi_and_dayun(birth_str, gender, longitude)
    profile = get_bazi_profile(birth_str, longitude if longitude != 120.0 else None)
    fav = favorable_element(profile)
    fx = fav["element"]

    # 今日流日干支的天干五行,与喜神的生克 -> 指数
    liuri = chart["current_liuri"]                    # 如 "庚午"
    liuri_wx = GAN_WUXING[liuri[0]]
    rel = _relation_to(fx, liuri_wx)
    score = {"同": 88, "生": 92, "泄": 72, "耗": 62, "克": 48}[rel]
    # 流月加成/减分(轻权重)
    liuyue_wx = GAN_WUXING[chart["current_liuyue"][0]]
    rel_m = _relation_to(fx, liuyue_wx)
    score += {"同": 4, "生": 6, "泄": -2, "耗": -4, "克": -8}[rel_m]
    score = max(30, min(97, score))

    # 黄历宜忌(取前两条)
    now = datetime.now()
    lunar_today = Solar.fromYmd(now.year, now.month, now.day).getLunar()
    yi = lunar_today.getDayYi()[:2]
    ji = lunar_today.getDayJi()[:2]

    today = {
        "solar_date": chart["solar_date"],
        "lunar_date": chart["lunar_date"],
        "liunian": chart["current_liunian"],
        "liuyue": chart["current_liuyue"],
        "liuri": liuri,
        "favorable": fx,
        "day_master_strong": fav["strong"],
        "color_name": WX_COLOR[fx]["name"],
        "color_rgb": WX_COLOR[fx]["rgb"],
        "number": WX_NUMBER[fx],
        "direction": WX_DIRECTION[fx],
        "yi": yi,
        "ji": ji,
        "score": score,
        "liuri_relation": rel,      # 文案素材:今日五行对喜神的作用
    }
    return {"chart": chart, "today": today}


def build_caption_prompt(chart: dict, today: dict) -> list:
    """给 LLM 的解盘文案 prompt(LLM 只做解读,严禁自行推算干支)。

    红线:非宿命论,不做医疗/寿命/重大决策断言。
    """
    system = (
        "你是玄学日历上的一句话解盘师。规则:"
        "1) 只依据下面系统排好的数据,严禁自己推算任何干支或日期;"
        "2) 输出恰好一句话,12~20个汉字,古风玄学语感,可用顿号,不用标点结尾;"
        "3) 内容围绕今日五行气场与穿搭/行动的宜忌建议;"
        "4) 不做健康、疾病、重大财务决策的断言;语气积极不宿命。"
        "只输出这一句话,不要任何其他内容。"
    )
    user = (
        f"命主日主{chart['day_master']}({'身强' if today['day_master_strong'] else '身弱'}),"
        f"喜用神五行为{today['favorable']}。"
        f"今日{today['liuri']}日(天干五行对喜神构成「{today['liuri_relation']}」),"
        f"流月{today['liuyue']},流年{today['liunian']},当前大运{chart['current_dayun']}。"
        f"今日幸运色{today['color_name']}。请给出今日一句话。"
    )
    return [{"role": "system", "content": system}, {"role": "user", "content": user}]
