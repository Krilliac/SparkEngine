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

The audit rules are the binding spec section 2.5 rules, unrelaxed. Two of them cannot be met by
moving foreground text alone, so the generator also moves (lightness only):
  * the accent fills: BorderFocus must reach 3:1 against AccentFill as well as against Base,
    Raised and Selected, which no focus colour can do next to a mid-luminance accent such as the
    #f1823a seed. The accent moves by the smallest step that leaves room for a focus colour.
  * BorderFocus itself, by the smallest step that then passes.
Every value that differs from the Ember spec table is printed by --report and recorded in
DESIGN.md's token appendix (spec-amendment item 5).

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
# Every pair of {Success, Warning, Error, Info, AccentFill} stays apart under every simulated
# deficiency (spec section 2.5).
MIN_CVD_DELTA = 0.08
CVD_KINDS = ("Protan", "Deutan", "Tritan")
FOCUS_BACKGROUNDS = ("SurfaceBase", "SurfaceRaised", "SurfaceSelected", "AccentFill")
MIN_FOCUS_HUE_DEGREES = 60.0
# A hovered / pressed destructive button lays ErrorText at these alphas over ErrorFill; the label
# (ErrorText) must keep 4.5:1 on the result. Mirrors kDangerHoverTint / kDangerPressTint in
# SparkUI/Include/SparkUI/Tokens.h.
DANGER_TINTS = (0.12, 0.20)
# The generator corrects past the audit thresholds by these margins so float rounding in a
# second implementation can never flip a pass.
CONTRAST_MARGIN = 0.06
DELTA_MARGIN = 0.006
HUE_MARGIN = 1.0
# The accent is the brand colour, so moving it costs more than moving the focus ring or a status.
ACCENT_MOVE_WEIGHT = 4.0

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


def danger_fill(t: dict, tint: float):
    """Opaque fill of a hovered / pressed destructive button (ErrorText over ErrorFill)."""
    return composite(with_alpha(t["ErrorText"], tint), t["ErrorFill"])


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
    for tint in DANGER_TINTS:
        ratio = contrast(t["ErrorText"], danger_fill(t, tint))
        if ratio < 4.5:
            failures.append(f"ErrorText on danger tint {tint}: {ratio:.2f} < 4.5")
    for bg in ("SurfaceBase", "SurfaceRaised", "SurfaceSunken"):
        need("BorderStrong", bg, 3.0)
    for bg in FOCUS_BACKGROUNDS:
        need("BorderFocus", bg, 3.0)
    need("AccentFill", "SurfaceBase", 3.0)
    for graph in GRAPHS + CATEGORIES:
        need(graph, "SurfaceSunken", 3.0)
    for kind in CVD_KINDS:
        for x, y in itertools.combinations(STATUS_TEXT + ("AccentFill",), 2):
            d = delta(simulate(t[x], kind), simulate(t[y], kind))
            if d < MIN_CVD_DELTA:
                failures.append(f"{x} vs {y} under {kind}: {d:.3f} < {MIN_CVD_DELTA}")
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


# --- correction (OKLab lightness only) ---------------------------------------------------------

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
    text_min = text_minimum(appearance)
    for role in BODY_TEXT:
        lift_contrast(t, role, SURFACES, text_min)
    lift_contrast(t, "TextDisabled", SURFACES, 3.0)
    lift_contrast(t, "BorderStrong", ("SurfaceBase", "SurfaceRaised", "SurfaceSunken"), 3.0)
    for role in GRAPHS + CATEGORIES:
        if role not in ("Graph1",):
            lift_contrast(t, role, ("SurfaceSunken",), 3.0)


def separated(a: dict, b: dict) -> bool:
    limit = MIN_CVD_DELTA + DELTA_MARGIN
    return all(delta(a[kind], b[kind]) >= limit for kind in CVD_KINDS)


def simulations(value) -> dict:
    return {kind: simulate(value, kind) for kind in MACHADO}


def lightness_steps(limit: float = 0.30, step: float = 0.005) -> list[float]:
    count = int(round(limit / step))
    return sorted({round(sign * step * i, 3) for i in range(count + 1) for sign in (1, -1)}, key=lambda v: (abs(v), v))


def status_passes_contrast(name: str, value, sets: list[tuple[dict, str]]) -> bool:
    """Contrast rules of one status text candidate on every family's surfaces and fill."""
    for t, appearance in sets:
        if contrast(value, t[f"{name}Fill"]) < 4.5 + CONTRAST_MARGIN:
            return False
        if any(contrast(value, t[bg]) < text_minimum(appearance) + CONTRAST_MARGIN for bg in SURFACES):
            return False
        if name == "Error":
            candidate = dict(t, ErrorText=value)
            if any(contrast(value, danger_fill(candidate, tint)) < 4.5 + CONTRAST_MARGIN for tint in DANGER_TINTS):
                return False
    return True


ACCENT_FILLS = ("AccentFill", "AccentFillHover", "AccentFillActive")


def keep_ink_contrast(value, ink):
    """Step a fill's lightness away from the ink until TextOnAccent reads on it, or None."""
    step = 0.002 if luminance(ink) < luminance(value) else -0.002
    for _ in range(200):
        if contrast(ink, value) >= 4.5 + CONTRAST_MARGIN:
            return value
        value = quantize(shift_lightness(value, step))
    return None


def find_focus(t: dict, fill):
    """Smallest BorderFocus lightness move that passes every focus rule next to @p fill.

    Returns (offset, colour), or None when no lightness passes.

    The ring stays on the far side of the accent from the surfaces (brighter than the accent on
    dark surfaces, darker on light ones), so it never reads as a dimmer accent.
    """
    original = t["BorderFocus"]
    dark = lighter_wins(t)
    backgrounds = [t[bg] for bg in FOCUS_BACKGROUNDS if bg != "AccentFill"] + [fill]
    for offset in lightness_steps(0.70):
        focus = quantize(shift_lightness(original, offset))
        if (luminance(focus) > luminance(fill)) != dark:
            continue
        if any(contrast(focus, bg) < 3.0 + CONTRAST_MARGIN for bg in backgrounds):
            continue
        if hue_distance(focus, t["SurfaceSelected"]) < MIN_FOCUS_HUE_DEGREES + HUE_MARGIN:
            continue
        return offset, focus
    return None


def accent_options(t: dict) -> list[tuple[float, dict]]:
    """Accent moves that pass every accent rule except CVD separation, cheapest first.

    Each option moves AccentFill/Hover/Active together (Hover and Active further only when
    TextOnAccent needs it) and carries the BorderFocus that reaches 3:1 against the moved fill.
    Its cost is the weighted sum of the squared accent and focus lightness moves.
    """
    ink = t["TextOnAccent"]
    options = []
    for offset in lightness_steps(0.45):
        moved = {role: quantize(shift_lightness(t[role], offset)) for role in ACCENT_FILLS}
        fill = moved["AccentFill"]
        if contrast(fill, t["SurfaceBase"]) < 3.0 + CONTRAST_MARGIN or contrast(ink, fill) < 4.5 + CONTRAST_MARGIN:
            continue
        hover = keep_ink_contrast(moved["AccentFillHover"], ink)
        active = keep_ink_contrast(moved["AccentFillActive"], ink)
        focus = find_focus(t, fill)
        if hover is None or active is None or focus is None:
            continue
        focus_offset, focus_colour = focus
        options.append((ACCENT_MOVE_WEIGHT * offset * offset + focus_offset * focus_offset,
                        {"AccentFill": fill, "AccentFillHover": hover, "AccentFillActive": active,
                         "BorderFocus": focus_colour}))
    if not options:
        raise SystemExit("no accent lightness leaves room for a 3:1 focus colour")
    options.sort(key=lambda option: option[0])
    return options


def apply_accent(t: dict, values: dict) -> None:
    t.update(values)
    t["Graph1"] = values["AccentFill"]
    t["SelectionTextBg"] = quantize(with_alpha(values["AccentFill"], t["SelectionTextBg"][3]))


def correct_status(sets: list[tuple[dict, str]]) -> dict:
    """Choose the four shared status texts and every family's accent of one appearance together.

    The status roles are shared by all families, so contrast is checked on every family's
    surfaces and each family's accent must stay CVD-separated from them. Each status role first
    gets its contrast-feasible offsets and each family its accent options (accent_options); a
    pruned depth-first search then picks Ember's accent, the status offsets and the cheapest
    still-separated accent of each other family, minimising the sum of squared moves so
    the movement spreads instead of pushing one role to an extreme. The chosen accents are
    applied in place; the status values are returned.
    """
    ember = sets[0][0]
    feasible: dict[str, list[tuple[float, tuple]]] = {}
    for name in STATUS:
        role = f"{name}Text"
        options = []
        for offset in lightness_steps(0.40):
            value = quantize(shift_lightness(ember[role], offset))
            if status_passes_contrast(name, value, sets):
                options.append((offset, value))
        if not options:
            raise SystemExit(f"{role} cannot meet contrast at any lightness")
        feasible[role] = options
    sims = {role: [simulations(value) for _, value in feasible[role]] for role in STATUS_TEXT}
    accents = [[(cost, values, simulations(values["AccentFill"])) for cost, values in accent_options(t)]
               for t, _ in sets]
    best = {"cost": float("inf"), "status": None, "accents": None}
    chosen: list[int] = []

    def search(depth: int, total: float, ember_accent: tuple, others: list[list[tuple]]) -> None:
        # others: per non-Ember family, its accent options (cheapest first) still separated from
        # every status chosen so far; the cheapest of each is a lower bound on the final cost.
        bound = total + sum(options[0][0] for options in others)
        if bound >= best["cost"]:
            return
        if depth == len(STATUS_TEXT):
            best["cost"] = bound
            best["status"] = list(chosen)
            best["accents"] = [ember_accent[1]] + [options[0][1] for options in others]
            return
        role = STATUS_TEXT[depth]
        for index, (offset, _) in enumerate(feasible[role]):
            cost = total + offset * offset
            if cost >= best["cost"]:
                break
            sim = sims[role][index]
            if not separated(sim, ember_accent[2]):
                continue
            if not all(separated(sim, sims[STATUS_TEXT[d]][chosen[d]]) for d in range(depth)):
                continue
            narrowed = [[option for option in options if separated(option[2], sim)] for options in others]
            if any(not options for options in narrowed):
                continue
            chosen.append(index)
            search(depth + 1, cost, ember_accent, narrowed)
            chosen.pop()

    for option in accents[0]:
        if option[0] >= best["cost"]:
            break
        search(0, option[0], option, accents[1:])
    if best["status"] is None:
        raise SystemExit("status roles and accents cannot be separated by lightness alone")
    for (t, _), values in zip(sets, best["accents"]):
        apply_accent(t, values)
    return {role: feasible[role][best["status"][i]][1] for i, role in enumerate(STATUS_TEXT)}


def text_minimum(appearance: str) -> float:
    return 7.0 if appearance == "HighContrast" else 4.5


def build() -> dict:
    tables = {(family, appearance): derive(family, appearance) for family in FAMILIES for appearance in APPEARANCES}
    for appearance in APPEARANCES:
        sets = [(tables[(family, appearance)], appearance) for family in FAMILIES]
        for t, _ in sets:
            correct_contrast(t, appearance)
        status = correct_status(sets)
        for t, _ in sets:
            t.update(status)
            t["Graph3"] = t["SuccessText"]
            t["Graph4"] = t["WarningText"]
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
