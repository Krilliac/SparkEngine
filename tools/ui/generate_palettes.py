#!/usr/bin/env python3
"""Generate SparkUI/Source/Palettes.inl, the built-in Ember Signal colour tables.

The nine built-in token sets are {Ember, Cobalt, Graphite} x {Dark, Light, HighContrast}.
Ember's values are the canonical spec table (docs: Ember Signal spec, section 2.2). Cobalt
and Graphite are derived from their seeds by the spec's family rules (section 2.3). Every set
then passes through one correction step that may move only a foreground role's OKLab
lightness (hue and chroma fixed) until the set meets the token audit rules that
Gated_SparkUi_TokenAudit_AllBuiltinSetsPass enforces in C++ (SparkUI/Source/TokenAudit.cpp).
Status roles are shared by all three families of one appearance, so they are corrected once
against the union of the families' surfaces.

Usage:
  python tools/ui/generate_palettes.py           # rewrite SparkUI/Source/Palettes.inl
  python tools/ui/generate_palettes.py --check   # exit 1 when the committed file is stale
  python tools/ui/generate_palettes.py --report  # print every corrected value
"""

from __future__ import annotations

import argparse
import itertools
import math
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUTPUT = ROOT / "SparkUI" / "Source" / "Palettes.inl"

ROLES = (
    "SurfaceSunken", "SurfaceBase", "SurfaceRaised", "SurfaceOverlay", "SurfaceHover", "SurfaceSelected",
    "SurfaceSelectedUnfocused", "BorderSubtle", "BorderStrong", "BorderFocus", "TextPrimary", "TextSecondary",
    "TextDisabled", "TextOnAccent", "TextLink", "AccentFill", "AccentFillHover", "AccentFillActive", "AccentText",
    "Secondary", "SuccessText", "WarningText", "ErrorText", "InfoText", "SuccessFill", "WarningFill", "ErrorFill",
    "InfoFill", "Graph1", "Graph2", "Graph3", "Graph4", "Graph5", "AxisX", "AxisY", "AxisZ", "Category1",
    "Category2", "Category3", "Category4", "Category5", "Category6", "Category7", "Category8", "ModalScrim",
    "DropTarget", "SelectionTextBg",
)
APPEARANCES = ("Dark", "Light", "HighContrast")
FAMILIES = ("Ember", "Cobalt", "Graphite")

# Audit constants; they mirror SparkUI/Source/TokenAudit.cpp exactly.
SURFACES = ("SurfaceSunken", "SurfaceBase", "SurfaceRaised", "SurfaceOverlay", "SurfaceHover", "SurfaceSelected",
            "SurfaceSelectedUnfocused")
BODY_TEXT = ("TextPrimary", "TextSecondary", "TextLink", "AccentText")
STATUS = ("Success", "Warning", "Error", "Info")
STATUS_TEXT = tuple(f"{name}Text" for name in STATUS)
GRAPHS = ("Graph1", "Graph2", "Graph3", "Graph4", "Graph5")
CATEGORIES = tuple(f"Category{index}" for index in range(1, 9))
# Status texts must stay apart from each other under every simulated deficiency, and from the
# accent under the common red-green deficiencies (spec section 2.5, with the separation the
# L-only tuning rule can reach; see SparkUI/Source/TokenAudit.cpp).
MIN_CVD_DELTA = 0.06
STATUS_CVD_KINDS = ("Protan", "Deutan", "Tritan")
ACCENT_CVD_KINDS = ("Protan", "Deutan")
MIN_FOCUS_ACCENT_DELTA = 0.08
MIN_FOCUS_HUE_DEGREES = 60.0
# The generator corrects past the audit thresholds by these margins so float rounding in a
# second implementation can never flip a pass.
CONTRAST_MARGIN = 0.06
DELTA_MARGIN = 0.006

# Ember canonical table: role -> (Dark, Light, HighContrast) as #rrggbb or #rrggbbaa.
EMBER = {
    "SurfaceSunken": ("#110e0d", "#ebe7e2", "#000000"),
    "SurfaceBase": ("#1a1716", "#f6f4f1", "#000000"),
    "SurfaceRaised": ("#24201e", "#ffffff", "#0a0a0a"),
    "SurfaceOverlay": ("#2b2623", "#ffffff", "#121212"),
    "SurfaceHover": ("#412a1c", "#f3e2d6", "#1f1f1f"),
    "SurfaceSelected": ("#563520", "#f6d3bb", "#003a4d"),
    "SurfaceSelectedUnfocused": ("#3a302b", "#e9e3dd", "#262626"),
    "BorderSubtle": ("#3c3733", "#d8d2cb", "#9a9a9a"),
    "BorderStrong": ("#776e66", "#857b72", "#ffffff"),
    "BorderFocus": ("#68d3ee", "#0b7fa6", "#ffd400"),
    "TextPrimary": ("#efeeeb", "#1c1917", "#ffffff"),
    "TextSecondary": ("#b0a9a1", "#57504a", "#e6e6e6"),
    "TextDisabled": ("#7d766f", "#8c847c", "#a6a6a6"),
    "TextOnAccent": ("#1a0f08", "#ffffff", "#000000"),
    "TextLink": ("#68d3ee", "#0b6f92", "#7fe7ff"),
    "AccentFill": ("#f1823a", "#b84d14", "#ffb15c"),
    "AccentFillHover": ("#f4975a", "#a3420f", "#ffc27f"),
    "AccentFillActive": ("#d96e28", "#8c380b", "#ff9d33"),
    "AccentText": ("#f7a56f", "#96390a", "#ffc58a"),
    "Secondary": ("#68d3ee", "#0b7fa6", "#7fe7ff"),
    "SuccessText": ("#3ccfa0", "#0a7a5a", "#6ff0c2"),
    "WarningText": ("#f2c14e", "#865d00", "#ffe14d"),
    "ErrorText": ("#ff8a9a", "#b3263f", "#ff9aa8"),
    "InfoText": ("#7cc4f5", "#1561ad", "#9fd3ff"),
    "SuccessFill": ("#16302a", "#def3ec", "#000000"),
    "WarningFill": ("#33290f", "#fbefd2", "#000000"),
    "ErrorFill": ("#3a1a20", "#fbe1e5", "#000000"),
    "InfoFill": ("#142a3a", "#deebf8", "#000000"),
    "Graph1": ("#f1823a", "#b84d14", "#ffb15c"),
    "Graph2": ("#68d3ee", "#0b7fa6", "#7fe7ff"),
    "Graph3": ("#3ccfa0", "#0a7a5a", "#6ff0c2"),
    "Graph4": ("#f2c14e", "#865d00", "#ffe14d"),
    "Graph5": ("#c39bff", "#6b4fc8", "#d8b4ff"),
    "AxisX": ("#ff7f7f", "#c0392b", "#ff9a9a"),
    "AxisY": ("#8fd16a", "#2e7d32", "#a6f07a"),
    "AxisZ": ("#6fa8ff", "#1f5fbf", "#8cc0ff"),
    "Category1": ("#e69f00",) * 3,
    "Category2": ("#56b4e9",) * 3,
    "Category3": ("#009e73",) * 3,
    "Category4": ("#f0e442",) * 3,
    "Category5": ("#0072b2",) * 3,
    "Category6": ("#d55e00",) * 3,
    "Category7": ("#cc79a7",) * 3,
    "Category8": ("#bbbbbb",) * 3,
    "ModalScrim": ("#00000099", "#1c191759", "#000000cc"),
}

# Seeds (spec section 2.1): sunken, base, raised, borderSubtle, text, textMuted, accent, secondary.
SEEDS = {
    "Ember": ("#110e0d", "#1a1716", "#24201e", "#3c3733", "#efeeeb", "#918b85", "#f1823a", "#68d3ee"),
    "Cobalt": ("#0b1017", "#151f2a", "#202e3d", "#314557", "#e6eef7", "#94a8bb", "#55c2e8", "#a99aff"),
    "Graphite": ("#101113", "#1b1c1f", "#282a2f", "#46484e", "#ececef", "#a8aab1", "#c9d1dd", "#78d3b2"),
}
# Family exceptions (spec section 2.3).
FAMILY_FOCUS_DARK = {"Cobalt": "#ffcf5a"}
FAMILY_TEXT_ON_ACCENT = {"Ember": "#1a0f08", "Cobalt": "#07131c", "Graphite": "#111214"}
SELECTION_ALPHA = {"Ember": 0.35, "Cobalt": 0.35, "Graphite": 0.22}
HIGH_CONTRAST_SELECTION_ALPHA = 0.50
DROP_TARGET_ALPHA = 0.70


# --- colour science (mirrors SparkUI/Source/Color.cpp) ---------------------------------------

def parse_hex(text: str) -> tuple[float, float, float, float]:
    text = text.lstrip("#")
    alpha = int(text[6:8], 16) / 255.0 if len(text) == 8 else 1.0
    return (int(text[0:2], 16) / 255.0, int(text[2:4], 16) / 255.0, int(text[4:6], 16) / 255.0, alpha)


def to_hex(rgba: tuple[float, float, float, float]) -> str:
    channels = [round(min(1.0, max(0.0, c)) * 255) for c in rgba[:3]]
    text = "#" + "".join(f"{c:02x}" for c in channels)
    if rgba[3] < 1.0:
        text += f"{round(rgba[3] * 255):02x}"
    return text


def quantize(rgba):
    return parse_hex(to_hex(rgba))


def to_linear(c: float) -> float:
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def to_srgb(c: float) -> float:
    c = min(1.0, max(0.0, c))
    return c * 12.92 if c <= 0.0031308 else 1.055 * c ** (1.0 / 2.4) - 0.055


def composite(top, bottom):
    a = top[3]
    return (top[0] * a + bottom[0] * (1 - a), top[1] * a + bottom[1] * (1 - a), top[2] * a + bottom[2] * (1 - a), 1.0)


def luminance(rgba) -> float:
    r, g, b = (to_linear(c) for c in rgba[:3])
    return 0.2126 * r + 0.7152 * g + 0.0722 * b


def contrast(fg, bg) -> float:
    fg = composite(fg, bg) if fg[3] < 1.0 else fg
    lf, lb = luminance(fg), luminance(bg)
    hi, lo = max(lf, lb), min(lf, lb)
    return (hi + 0.05) / (lo + 0.05)


def cbrt(x: float) -> float:
    return math.copysign(abs(x) ** (1.0 / 3.0), x)


def oklab_from_linear(r: float, g: float, b: float) -> tuple[float, float, float]:
    l = cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b)
    m = cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b)
    s = cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b)
    return (0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s,
            1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
            0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s)


def to_oklab(rgba) -> tuple[float, float, float]:
    return oklab_from_linear(*(to_linear(c) for c in rgba[:3]))


def from_oklab(lab, alpha: float = 1.0):
    L, a, b = lab
    l = (L + 0.3963377774 * a + 0.2158037573 * b) ** 3
    m = (L - 0.1055613458 * a - 0.0638541728 * b) ** 3
    s = (L - 0.0894841775 * a - 1.2914855480 * b) ** 3
    return (to_srgb(4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s),
            to_srgb(-1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s),
            to_srgb(-0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s), alpha)


MACHADO = {
    "Protan": ((0.152286, 1.052583, -0.204868), (0.114503, 0.786281, 0.099216), (-0.003882, -0.048116, 1.051998)),
    "Deutan": ((0.367322, 0.860646, -0.227968), (0.280085, 0.672501, 0.047413), (-0.011820, 0.042940, 0.968881)),
    "Tritan": ((1.255528, -0.076749, -0.178779), (-0.078411, 0.930809, 0.147602), (0.004733, 0.691367, 0.303900)),
}


def simulate(rgba, kind: str):
    lin = [to_linear(c) for c in rgba[:3]]
    rows = MACHADO[kind]
    out = [min(1.0, max(0.0, sum(rows[i][j] * lin[j] for j in range(3)))) for i in range(3)]
    return oklab_from_linear(*out)


def delta(a, b) -> float:
    return math.sqrt(sum((x - y) ** 2 for x, y in zip(a, b)))


def hue_degrees(rgba) -> float:
    _, a, b = to_oklab(rgba)
    return math.degrees(math.atan2(b, a)) % 360.0


def hue_distance(x, y) -> float:
    d = abs(hue_degrees(x) - hue_degrees(y)) % 360.0
    return min(d, 360.0 - d)


def mix(a, b, t: float):
    return tuple(a[i] + (b[i] - a[i]) * t for i in range(3)) + (1.0,)


def with_lightness(rgba, lightness: float):
    _, a, b = to_oklab(rgba)
    return from_oklab((lightness, a, b), rgba[3])


def shift_lightness(rgba, amount: float):
    L, a, b = to_oklab(rgba)
    return from_oklab((L + amount, a, b), rgba[3])


def with_alpha(rgba, alpha: float):
    return rgba[:3] + (alpha,)


# --- audit (mirrors SparkUI/Source/TokenAudit.cpp) --------------------------------------------

def audit(t: dict, appearance: str) -> list[str]:
    failures = []
    text_min = 7.0 if appearance == "HighContrast" else 4.5

    def need(fg, bg, minimum):
        ratio = contrast(t[fg], t[bg])
        if ratio < minimum:
            failures.append(f"{fg} on {bg}: {ratio:.2f} < {minimum}")

    for fg in BODY_TEXT + STATUS_TEXT:
        for bg in SURFACES:
            need(fg, bg, text_min)
    for bg in SURFACES:
        need("TextDisabled", bg, 3.0)
    for bg in ("AccentFill", "AccentFillHover", "AccentFillActive"):
        need("TextOnAccent", bg, 4.5)
    for name in STATUS:
        need(f"{name}Text", f"{name}Fill", 4.5)
    for bg in ("SurfaceBase", "SurfaceRaised", "SurfaceSunken"):
        need("BorderStrong", bg, 3.0)
    for bg in ("SurfaceBase", "SurfaceRaised", "SurfaceSelected"):
        need("BorderFocus", bg, 3.0)
    need("AccentFill", "SurfaceBase", 3.0)
    for graph in GRAPHS + CATEGORIES:
        need(graph, "SurfaceSunken", 3.0)
    for kind in STATUS_CVD_KINDS:
        for x, y in itertools.combinations(STATUS_TEXT, 2):
            d = delta(simulate(t[x], kind), simulate(t[y], kind))
            if d < MIN_CVD_DELTA:
                failures.append(f"{x} vs {y} under {kind}: {d:.3f} < {MIN_CVD_DELTA}")
    for kind in ACCENT_CVD_KINDS:
        for x in STATUS_TEXT:
            d = delta(simulate(t[x], kind), simulate(t["AccentFill"], kind))
            if d < MIN_CVD_DELTA:
                failures.append(f"{x} vs AccentFill under {kind}: {d:.3f} < {MIN_CVD_DELTA}")
    d = delta(to_oklab(t["BorderFocus"]), to_oklab(t["AccentFill"]))
    if d < MIN_FOCUS_ACCENT_DELTA:
        failures.append(f"BorderFocus vs AccentFill: {d:.3f} < {MIN_FOCUS_ACCENT_DELTA}")
    h = hue_distance(t["BorderFocus"], t["SurfaceSelected"])
    if h < MIN_FOCUS_HUE_DEGREES:
        failures.append(f"BorderFocus hue vs SurfaceSelected: {h:.1f} < {MIN_FOCUS_HUE_DEGREES}")
    return failures


# --- family rules (spec section 2.3) ------------------------------------------------------------

def ember_table(appearance: str) -> dict:
    index = APPEARANCES.index(appearance)
    return {role: parse_hex(values[index]) for role, values in EMBER.items()}


def derive(family: str, appearance: str) -> dict:
    t = ember_table(appearance)
    if family == "Ember":
        return finish(t, family, appearance)
    sunken, base, raised, subtle, text, muted, accent, secondary = (parse_hex(v) for v in SEEDS[family])
    if appearance == "Dark":
        t.update({
            "SurfaceSunken": sunken, "SurfaceBase": base, "SurfaceRaised": raised, "BorderSubtle": subtle,
            "TextPrimary": text,
            "SurfaceOverlay": mix(raised, text, 0.035),
            "SurfaceHover": mix(base, accent, 0.18),
            "SurfaceSelected": mix(base, accent, 0.28),
            "SurfaceSelectedUnfocused": mix(base, muted, 0.22),
            "BorderStrong": mix(subtle, muted, 0.60),
            "TextSecondary": mix(muted, text, 0.30),
            "TextDisabled": mix(base, muted, 0.75),
            "AccentFill": accent,
            "AccentFillHover": shift_lightness(accent, 0.05),
            "AccentFillActive": shift_lightness(accent, -0.06),
            "AccentText": shift_lightness(accent, 0.08),
            "Secondary": secondary, "TextLink": secondary,
            "BorderFocus": parse_hex(FAMILY_FOCUS_DARK.get(family, to_hex(secondary))),
        })
    else:
        # Light and High Contrast keep Ember's neutral ramp; the family contributes its accent
        # and secondary hues at the lightness Ember uses for those roles in this appearance.
        for role in ("AccentFill", "AccentFillHover", "AccentFillActive", "AccentText"):
            t[role] = with_lightness(accent, to_oklab(t[role])[0])
        for role in ("Secondary", "TextLink"):
            t[role] = with_lightness(secondary, to_oklab(t[role])[0])
        if appearance == "Light":
            base_light = t["SurfaceBase"]
            t["SurfaceHover"] = mix(base_light, t["AccentFill"], 0.10)
            t["SurfaceSelected"] = mix(base_light, t["AccentFill"], 0.20)
            t["BorderFocus"] = t["Secondary"]
    t["TextOnAccent"] = parse_hex(FAMILY_TEXT_ON_ACCENT[family]) if appearance == "Dark" else t["TextOnAccent"]
    t["Graph1"] = t["AccentFill"]
    t["Graph2"] = t["Secondary"]
    return finish(t, family, appearance)


def finish(t: dict, family: str, appearance: str) -> dict:
    alpha = HIGH_CONTRAST_SELECTION_ALPHA if appearance == "HighContrast" else SELECTION_ALPHA[family]
    t["DropTarget"] = with_alpha(t["Secondary"], DROP_TARGET_ALPHA)
    t["SelectionTextBg"] = with_alpha(t["AccentFill"], alpha)
    return {role: quantize(value) for role, value in t.items()}


# --- correction (foreground OKLab lightness only) ---------------------------------------------

def lighter_wins(t: dict) -> bool:
    return luminance(t["SurfaceBase"]) < 0.18


def lift_contrast(t: dict, role: str, backgrounds, minimum: float) -> None:
    """Move role's lightness away from the backgrounds in 0.002 steps until every pair passes."""
    target = minimum + CONTRAST_MARGIN
    step = 0.002 if lighter_wins(t) else -0.002
    value = t[role]
    for _ in range(400):
        if all(contrast(value, t[bg]) >= target for bg in backgrounds):
            t[role] = value
            return
        value = quantize(shift_lightness(value, step))
    raise SystemExit(f"cannot reach {minimum}:1 for {role}")


def correct_contrast(t: dict, appearance: str) -> None:
    text_min = 7.0 if appearance == "HighContrast" else 4.5
    for role in BODY_TEXT:
        lift_contrast(t, role, SURFACES, text_min)
    lift_contrast(t, "TextDisabled", SURFACES, 3.0)
    lift_contrast(t, "BorderStrong", ("SurfaceBase", "SurfaceRaised", "SurfaceSunken"), 3.0)
    lift_contrast(t, "BorderFocus", ("SurfaceBase", "SurfaceRaised", "SurfaceSelected"), 3.0)
    for role in GRAPHS + CATEGORIES:
        if role not in ("Graph1",):
            lift_contrast(t, role, ("SurfaceSunken",), 3.0)


def separated(a: dict, b: dict, kinds) -> bool:
    limit = MIN_CVD_DELTA + DELTA_MARGIN
    return all(delta(a[kind], b[kind]) >= limit for kind in kinds)


def simulations(value) -> dict:
    return {kind: simulate(value, kind) for kind in MACHADO}


def lightness_steps(limit: float = 0.30, step: float = 0.005) -> list[float]:
    count = int(round(limit / step))
    return sorted({round(sign * step * i, 3) for i in range(count + 1) for sign in (1, -1)}, key=lambda v: (abs(v), v))


def correct_status(sets: list[tuple[dict, str]]) -> dict:
    """Find the smallest lightness offsets for the four status texts of one appearance.

    The status roles are shared by all families, so contrast is checked on every family's
    surfaces; separation from the accent is solved against Ember's fixed seed accent (the other
    families' accents move instead, see correct_family_accent). Contrast limits each role on its
    own, so each role first gets its feasible offsets; a pruned depth-first search then picks
    the combination with the smallest total movement that keeps the CVD separation.
    """
    ember = sets[0][0]
    feasible: dict[str, list[tuple[float, tuple]]] = {}
    for name in STATUS:
        role = f"{name}Text"
        options = []
        for offset in lightness_steps():
            value = quantize(shift_lightness(ember[role], offset))
            if all(
                contrast(value, t[f"{name}Fill"]) >= 4.5 + CONTRAST_MARGIN
                and all(contrast(value, t[bg]) >= text_minimum(appearance) + CONTRAST_MARGIN for bg in SURFACES)
                for t, appearance in sets
            ):
                options.append((offset, value))
        if not options:
            raise SystemExit(f"{role} cannot meet contrast at any lightness")
        feasible[role] = options
    sims = {role: [simulations(value) for _, value in feasible[role]] for role in STATUS_TEXT}
    accent = simulations(ember["AccentFill"])
    best: list = [None, float("inf")]
    chosen: list[int] = []

    def search(depth: int, total: float) -> None:
        if depth == len(STATUS_TEXT):
            best[0], best[1] = list(chosen), total
            return
        role = STATUS_TEXT[depth]
        for index, (offset, _) in enumerate(feasible[role]):
            cost = total + abs(offset)
            if cost >= best[1]:
                break
            sim = sims[role][index]
            if not separated(sim, accent, ACCENT_CVD_KINDS):
                continue
            if not all(separated(sim, sims[STATUS_TEXT[d]][chosen[d]], STATUS_CVD_KINDS) for d in range(depth)):
                continue
            chosen.append(index)
            search(depth + 1, cost)
            chosen.pop()

    search(0, 0.0)
    if best[0] is None:
        raise SystemExit("status roles cannot be separated by lightness alone")
    return {role: feasible[role][best[0][i]][1] for i, role in enumerate(STATUS_TEXT)}


ACCENT_FAMILY = ("AccentFill", "AccentFillHover", "AccentFillActive", "AccentText")


def correct_family_accent(t: dict) -> None:
    """Move a non-Ember accent family's lightness until it separates from the shared statuses."""
    status_sims = [simulations(t[role]) for role in STATUS_TEXT]
    original = {role: t[role] for role in ACCENT_FAMILY}
    for offset in lightness_steps():
        moved = {role: quantize(shift_lightness(value, offset)) for role, value in original.items()}
        accent = moved["AccentFill"]
        if contrast(accent, t["SurfaceBase"]) < 3.0 + CONTRAST_MARGIN:
            continue
        if any(contrast(t["TextOnAccent"], moved[role]) < 4.5 + CONTRAST_MARGIN for role in ACCENT_FAMILY[:3]):
            continue
        if delta(to_oklab(t["BorderFocus"]), to_oklab(accent)) < MIN_FOCUS_ACCENT_DELTA + DELTA_MARGIN:
            continue
        accent_sim = simulations(accent)
        if not all(separated(accent_sim, sim, ACCENT_CVD_KINDS) for sim in status_sims):
            continue
        t.update(moved)
        t["Graph1"] = accent
        t["SelectionTextBg"] = quantize(with_alpha(accent, t["SelectionTextBg"][3]))
        return
    raise SystemExit("accent cannot be separated from the status roles by lightness alone")


def text_minimum(appearance: str) -> float:
    return 7.0 if appearance == "HighContrast" else 4.5


def build() -> dict:
    tables = {(family, appearance): derive(family, appearance) for family in FAMILIES for appearance in APPEARANCES}
    for appearance in APPEARANCES:
        sets = [(tables[(family, appearance)], appearance) for family in FAMILIES]
        for t, _ in sets:
            correct_contrast(t, appearance)
        status = correct_status(sets)
        for family, (t, _) in zip(FAMILIES, sets):
            t.update(status)
            t["Graph3"] = t["SuccessText"]
            t["Graph4"] = t["WarningText"]
            if family != "Ember":
                correct_family_accent(t)
                lift_contrast(t, "AccentText", SURFACES, text_minimum(appearance))
    for (family, appearance), t in tables.items():
        failures = audit(t, appearance)
        if failures:
            raise SystemExit(f"{family} {appearance} still fails: {failures}")
    return tables


def render(tables: dict) -> str:
    columns = [(family, appearance) for family in FAMILIES for appearance in APPEARANCES]
    lines = [
        "// Generated by tools/ui/generate_palettes.py from the Ember Signal seed table; do not edit by hand.",
        "// Regenerate with `python tools/ui/generate_palettes.py`; `--check` fails when this file is stale.",
        "// SPARK_UI_PALETTE_ROW(Role, 0xRRGGBBAA x 9) in the column order",
        "// " + ", ".join(f"{family}{appearance}" for family, appearance in columns) + ".",
        "// clang-format off",
    ]
    for role in ROLES:
        values = []
        for column in columns:
            r, g, b, a = tables[column][role]
            packed = (round(r * 255) << 24) | (round(g * 255) << 16) | (round(b * 255) << 8) | round(a * 255)
            values.append(f"0x{packed:08x}u")
        lines.append(f"SPARK_UI_PALETTE_ROW({role}, {', '.join(values)})")
    lines.append("// clang-format on")
    return "\n".join(lines) + "\n"


def report(tables: dict) -> None:
    for (family, appearance), t in tables.items():
        reference = ember_table(appearance) if family == "Ember" else None
        for role in ROLES:
            value = to_hex(t[role])
            if reference is not None and role in EMBER and to_hex(reference[role]) != value:
                print(f"forced: {family} {appearance} {role}: {to_hex(reference[role])} -> {value}")
            elif reference is None:
                print(f"{family} {appearance} {role}: {value}")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--check", action="store_true", help="fail when Palettes.inl is not current")
    parser.add_argument("--report", action="store_true", help="print corrected and derived values")
    args = parser.parse_args(argv)
    tables = build()
    text = render(tables)
    if args.report:
        report(tables)
    if args.check:
        current = OUTPUT.read_text(encoding="utf-8") if OUTPUT.is_file() else ""
        if current != text:
            print(f"{OUTPUT.relative_to(ROOT).as_posix()} is stale; run tools/ui/generate_palettes.py", file=sys.stderr)
            return 1
        print("Palettes.inl is current (9 token sets audited)")
        return 0
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT.write_bytes(text.encode("utf-8"))
    print(f"wrote {OUTPUT.relative_to(ROOT).as_posix()} (9 token sets audited)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
