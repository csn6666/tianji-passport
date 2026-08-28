"""
八字神煞与干支关系查表模块

lunar_python 不提供命理类神煞和详细的干支关系，这里用纯查表实现：
- 8 个常用神煞：天乙贵人/文昌/禄神/羊刃/桃花/驿马/华盖/将星
- 干支关系：天干五合/相冲、地支六合/三合(含半合)/三会/六冲/三刑(含子卯互刑/自刑)/六害/相破
"""
from collections import Counter
from typing import Dict, List, Optional, Tuple


# ============ 神煞查表 ============

# 天乙贵人（以日干起例，看四柱地支）
TIANYI_GUIREN = {
    "甲": ("丑", "未"), "戊": ("丑", "未"), "庚": ("丑", "未"),
    "乙": ("子", "申"), "己": ("子", "申"),
    "丙": ("亥", "酉"), "丁": ("亥", "酉"),
    "壬": ("巳", "卯"), "癸": ("巳", "卯"),
    "辛": ("午", "寅"),
}

# 文昌（以日干起例）
WENCHANG = {
    "甲": "巳", "乙": "午", "丙": "申", "戊": "申",
    "丁": "酉", "己": "酉", "庚": "亥", "辛": "子",
    "壬": "寅", "癸": "卯",
}

# 禄神（以日干起例，临官位）
LUSHEN = {
    "甲": "寅", "乙": "卯", "丙": "巳", "戊": "巳",
    "丁": "午", "己": "午", "庚": "申", "辛": "酉",
    "壬": "亥", "癸": "子",
}

# 羊刃（主流只论阳干，禄前一位）
YANGREN = {
    "甲": "卯", "丙": "午", "戊": "午", "庚": "酉", "壬": "子",
}

# 桃花/驿马/华盖/将星（以年支或日支起例，三合局四柱）
# 顺序: (桃花, 驿马, 华盖, 将星)
SAN_HE_BU = {
    "申": ("酉", "寅", "辰", "子"), "子": ("酉", "寅", "辰", "子"), "辰": ("酉", "寅", "辰", "子"),
    "寅": ("卯", "申", "戌", "午"), "午": ("卯", "申", "戌", "午"), "戌": ("卯", "申", "戌", "午"),
    "巳": ("午", "亥", "丑", "酉"), "酉": ("午", "亥", "丑", "酉"), "丑": ("午", "亥", "丑", "酉"),
    "亥": ("子", "巳", "未", "卯"), "卯": ("子", "巳", "未", "卯"), "未": ("子", "巳", "未", "卯"),
}


# ============ 干支关系查表 ============

# 天干五合 (合化五行)
TG_HE = {
    frozenset(["甲", "己"]): "土",
    frozenset(["乙", "庚"]): "金",
    frozenset(["丙", "辛"]): "水",
    frozenset(["丁", "壬"]): "木",
    frozenset(["戊", "癸"]): "火",
}

# 天干相冲（戊己居中不冲）
TG_CHONG = {
    frozenset(["甲", "庚"]),
    frozenset(["乙", "辛"]),
    frozenset(["丙", "壬"]),
    frozenset(["丁", "癸"]),
}

# 地支六合
DZ_LIU_HE = {
    frozenset(["子", "丑"]): "土", frozenset(["寅", "亥"]): "木",
    frozenset(["卯", "戌"]): "火", frozenset(["辰", "酉"]): "金",
    frozenset(["巳", "申"]): "水", frozenset(["午", "未"]): "土",
}

# 地支三合局 (三支齐全)
DZ_SAN_HE = {
    frozenset(["申", "子", "辰"]): ("水", "子"),
    frozenset(["寅", "午", "戌"]): ("火", "午"),
    frozenset(["巳", "酉", "丑"]): ("金", "酉"),
    frozenset(["亥", "卯", "未"]): ("木", "卯"),
}

# 地支三会方
DZ_SAN_HUI = {
    frozenset(["寅", "卯", "辰"]): "木",
    frozenset(["巳", "午", "未"]): "火",
    frozenset(["申", "酉", "戌"]): "金",
    frozenset(["亥", "子", "丑"]): "水",
}

# 地支六冲
DZ_LIU_CHONG = {
    frozenset(["子", "午"]), frozenset(["丑", "未"]), frozenset(["寅", "申"]),
    frozenset(["卯", "酉"]), frozenset(["辰", "戌"]), frozenset(["巳", "亥"]),
}

# 地支三刑（齐全为三刑，缺一为相刑）
DZ_SAN_XING = [
    (frozenset(["寅", "巳", "申"]), "无恩之刑"),
    (frozenset(["丑", "戌", "未"]), "恃势之刑"),
]
# 子卯互刑（无礼之刑）
DZ_HU_XING = frozenset(["子", "卯"])
# 自刑
DZ_ZI_XING = {"辰", "午", "酉", "亥"}

# 地支六害
DZ_LIU_HAI = {
    frozenset(["子", "未"]), frozenset(["丑", "午"]), frozenset(["寅", "巳"]),
    frozenset(["卯", "辰"]), frozenset(["申", "亥"]), frozenset(["酉", "戌"]),
}

# 地支相破
DZ_XIANG_PO = {
    frozenset(["子", "酉"]), frozenset(["丑", "辰"]), frozenset(["寅", "亥"]),
    frozenset(["卯", "午"]), frozenset(["巳", "申"]), frozenset(["未", "戌"]),
}


PILLAR_LABEL = {"year": "年", "month": "月", "day": "日", "time": "时"}


def compute_shensha(
    day_gan: str,
    year_zhi: str,
    four_zhi: Dict[str, Optional[str]],
) -> Dict[str, List[str]]:
    """计算八字常见神煞

    Args:
        day_gan: 日干
        year_zhi: 年支（用于桃花/驿马/华盖/将星 起例）
        four_zhi: {"year"/"month"/"day"/"time": 地支}, time 可为 None（时辰未知）

    Returns:
        {神煞名: [命中柱列表]}, 仅返回有命中的神煞
    """
    def _hits(targets):
        targets = (targets,) if isinstance(targets, str) else targets
        out = []
        for p, zhi in four_zhi.items():
            if zhi and zhi in targets:
                out.append(f"{PILLAR_LABEL[p]}柱{zhi}")
        return out

    raw = {}

    # 以日干起例
    if day_gan in TIANYI_GUIREN:
        raw["天乙贵人"] = _hits(TIANYI_GUIREN[day_gan])
    if day_gan in WENCHANG:
        raw["文昌"] = _hits(WENCHANG[day_gan])
    if day_gan in LUSHEN:
        raw["禄神"] = _hits(LUSHEN[day_gan])
    if day_gan in YANGREN:
        raw["羊刃"] = _hits(YANGREN[day_gan])

    # 以年支起例（三合局四柱：桃花/驿马/华盖/将星）
    if year_zhi in SAN_HE_BU:
        taohua, yima, huagai, jiangxing = SAN_HE_BU[year_zhi]
        raw["桃花"] = _hits(taohua)
        raw["驿马"] = _hits(yima)
        raw["华盖"] = _hits(huagai)
        raw["将星"] = _hits(jiangxing)

    return {k: v for k, v in raw.items() if v}


def _compute_relations_generic(
    pillars: List[Tuple[str, str, str]],
    must_include: Optional[set] = None,
) -> Dict[str, List[str]]:
    """通用关系计算

    Args:
        pillars: [(label, gan, zhi), ...]，label 直接用于显示（如 "年"/"月"/"流年"/"大运"）
        must_include: 若非 None，只保留至少有一柱 label 在此集合内的关系
                      （用于过滤"动态关系"——必须涉及流年/大运等才输出）
    """
    result: Dict[str, List[str]] = {
        "天干五合": [],
        "天干相冲": [],
        "地支六合": [],
        "地支三合": [],
        "地支三会": [],
        "地支六冲": [],
        "地支三刑": [],
        "地支六害": [],
        "地支相破": [],
    }

    def _passes(labels):
        return must_include is None or any(l in must_include for l in labels)

    # ── 两两比较 ──
    for i in range(len(pillars)):
        for j in range(i + 1, len(pillars)):
            l1, g1, z1 = pillars[i]
            l2, g2, z2 = pillars[j]
            if not _passes((l1, l2)):
                continue

            if g1 and g2 and g1 != g2:
                pair = frozenset([g1, g2])
                if pair in TG_HE:
                    result["天干五合"].append(f"{l1}{g1}+{l2}{g2}=合化{TG_HE[pair]}")
                if pair in TG_CHONG:
                    result["天干相冲"].append(f"{l1}{g1}冲{l2}{g2}")

            if z1 and z2 and z1 != z2:
                pair_z = frozenset([z1, z2])
                if pair_z in DZ_LIU_HE:
                    result["地支六合"].append(f"{l1}{z1}+{l2}{z2}=合化{DZ_LIU_HE[pair_z]}")
                if pair_z in DZ_LIU_CHONG:
                    result["地支六冲"].append(f"{l1}{z1}冲{l2}{z2}")
                if pair_z in DZ_LIU_HAI:
                    result["地支六害"].append(f"{l1}{z1}害{l2}{z2}")
                if pair_z in DZ_XIANG_PO:
                    result["地支相破"].append(f"{l1}{z1}破{l2}{z2}")
                if pair_z == DZ_HU_XING:
                    result["地支三刑"].append(f"{l1}{z1}刑{l2}{z2}（无礼）")

    # ── 三柱组合（三合/三会/三刑齐齐） ──
    zhi_set = {z for _, _, z in pillars if z}

    for combo, (wx, zhongwei) in DZ_SAN_HE.items():
        present = combo & zhi_set
        if combo.issubset(zhi_set):
            members = [(l, z) for l, _, z in pillars if z in combo]
            if _passes(l for l, _ in members):
                result["地支三合"].append(f"{'+'.join(f'{l}{z}' for l, z in members)} 三合{wx}局")
        elif len(present) == 2 and zhongwei in present:
            members = [(l, z) for l, _, z in pillars if z in present]
            if _passes(l for l, _ in members):
                result["地支三合"].append(f"{'+'.join(f'{l}{z}' for l, z in members)} 半合{wx}局")

    for combo, wx in DZ_SAN_HUI.items():
        if combo.issubset(zhi_set):
            members = [(l, z) for l, _, z in pillars if z in combo]
            if _passes(l for l, _ in members):
                result["地支三会"].append(f"{'+'.join(f'{l}{z}' for l, z in members)} 三会{wx}方")

    for combo, name in DZ_SAN_XING:
        present = combo & zhi_set
        if combo.issubset(zhi_set):
            members = [(l, z) for l, _, z in pillars if z in combo]
            if _passes(l for l, _ in members):
                result["地支三刑"].append(f"{'+'.join(f'{l}{z}' for l, z in members)} {name}（三刑齐）")
        elif len(present) == 2:
            members = [(l, z) for l, _, z in pillars if z in present]
            # 仅当两柱字相同时已被同支自刑覆盖；这里只看真不同字相刑
            if len({z for _, z in members}) == 2 and _passes(l for l, _ in members):
                result["地支三刑"].append(f"{'+'.join(f'{l}{z}' for l, z in members)} 相刑")

    # ── 自刑（同支重见，按 label pair 过滤）──
    by_zhi: Dict[str, List[str]] = {}
    for l, _, z in pillars:
        if z:
            by_zhi.setdefault(z, []).append(l)
    for zhi, labels in by_zhi.items():
        if len(labels) >= 2 and zhi in DZ_ZI_XING and _passes(labels):
            result["地支三刑"].append(f"{'+'.join(f'{l}{zhi}' for l in labels)} 自刑")

    return {k: v for k, v in result.items() if v}


def compute_relations(
    four_gan: Dict[str, Optional[str]],
    four_zhi: Dict[str, Optional[str]],
) -> Dict[str, List[str]]:
    """四柱之间的干支关系（不含动态柱）"""
    pillars = [
        (PILLAR_LABEL[p], four_gan.get(p), four_zhi.get(p))
        for p in ("year", "month", "day", "time")
        if four_gan.get(p) and four_zhi.get(p)
    ]
    return _compute_relations_generic(pillars, must_include=None)


def compute_dynamic_relations(
    four_gan: Dict[str, Optional[str]],
    four_zhi: Dict[str, Optional[str]],
    extras: Dict[str, str],
) -> Dict[str, List[str]]:
    """流年/大运/流月 vs 四柱（含动态柱之间）的关系

    Args:
        extras: {label: ganzhi}, 如 {"流年": "丙午", "大运": "戊午", "流月": "壬辰"}

    Returns:
        只返回涉及至少一个 extra label 的关系
    """
    pillars: List[Tuple[str, str, str]] = []
    for p in ("year", "month", "day", "time"):
        if four_gan.get(p) and four_zhi.get(p):
            pillars.append((PILLAR_LABEL[p], four_gan[p], four_zhi[p]))

    extra_labels = set()
    for label, gz in extras.items():
        if gz and len(gz) == 2:
            pillars.append((label, gz[0], gz[1]))
            extra_labels.add(label)

    return _compute_relations_generic(pillars, must_include=extra_labels)
