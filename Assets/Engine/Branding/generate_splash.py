#!/usr/bin/env python3
"""Generate the SparkEngine startup splash master frames and brand assets."""

from __future__ import annotations

import math
import wave
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter, ImageFont


ROOT = Path(__file__).resolve().parent
FRAMES = ROOT / "frames"
WIDTH, HEIGHT = 1920, 1080
FPS = 60
DURATION = 2.8
FRAME_COUNT = round(FPS * DURATION)

BLACK = (5, 6, 7)
OFF_WHITE = (236, 232, 222)
ORANGE = (255, 82, 0)
MUTED = (123, 126, 126)
GRID = (56, 61, 62)


def clamp(value: float, low: float = 0.0, high: float = 1.0) -> float:
    return max(low, min(high, value))


def smoothstep(a: float, b: float, value: float) -> float:
    if a == b:
        return float(value >= b)
    t = clamp((value - a) / (b - a))
    return t * t * (3.0 - 2.0 * t)


def ease_out_back(value: float) -> float:
    t = clamp(value)
    c1 = 1.70158
    c3 = c1 + 1.0
    return 1.0 + c3 * (t - 1.0) ** 3 + c1 * (t - 1.0) ** 2


def font(size: int, bold: bool = False) -> ImageFont.FreeTypeFont:
    name = "DejaVuSans-Bold.ttf" if bold else "DejaVuSans.ttf"
    return ImageFont.truetype(f"/usr/share/fonts/truetype/dejavu/{name}", size=size)


def tracked_text_mask(text: str, face: ImageFont.FreeTypeFont, tracking: int) -> Image.Image:
    widths = [round(face.getlength(ch)) for ch in text]
    box = face.getbbox(text)
    height = box[3] - box[1] + 10
    width = sum(widths) + tracking * max(0, len(text) - 1) + 10
    mask = Image.new("L", (width, height), 0)
    draw = ImageDraw.Draw(mask)
    x = 5
    for ch, advance in zip(text, widths):
        draw.text((x, 3 - box[1]), ch, font=face, fill=255)
        x += advance + tracking
    return mask


def paste_color(canvas: Image.Image, mask: Image.Image, xy: tuple[int, int], color: tuple[int, ...]) -> None:
    layer = Image.new("RGBA", canvas.size, color)
    full_mask = Image.new("L", canvas.size, 0)
    full_mask.paste(mask, xy)
    canvas.alpha_composite(Image.composite(layer, Image.new("RGBA", canvas.size), full_mask))


def add_screen(base: Image.Image, glow: Image.Image) -> Image.Image:
    # Pillow's screen operation keeps the hot orange glow luminous without clipping edges.
    return Image.new("RGBA", base.size, (0, 0, 0, 255))._new(
        Image.core.chop_screen(base.im, glow.im)
    )


def draw_grid(canvas: Image.Image, opacity: float, pulse: float) -> None:
    if opacity <= 0.001:
        return
    overlay = Image.new("RGBA", canvas.size, (0, 0, 0, 0))
    draw = ImageDraw.Draw(overlay)
    spacing = 72
    alpha = round(22 * opacity)
    for x in range(WIDTH // 2 % spacing, WIDTH, spacing):
        draw.line((x, 0, x, HEIGHT), fill=(*GRID, alpha), width=1)
    for y in range(HEIGHT // 2 % spacing, HEIGHT, spacing):
        draw.line((0, y, WIDTH, y), fill=(*GRID, alpha), width=1)
    radius = 120 + 880 * pulse
    center = (WIDTH / 2, HEIGHT / 2)
    for ring in range(3):
        r = radius + ring * 8
        ring_alpha = round((34 - ring * 9) * opacity * (1.0 - pulse))
        draw.ellipse((center[0] - r, center[1] - r, center[0] + r, center[1] + r),
                     outline=(*ORANGE, max(0, ring_alpha)), width=2)
    canvas.alpha_composite(overlay)


def draw_spark(canvas: Image.Image, center: tuple[float, float], progress: float, alpha: float) -> None:
    if progress <= 0.0 or alpha <= 0.0:
        return
    cx, cy = center
    directions = [
        (-1.0, 0.0, 76), (1.0, 0.0, 76), (0.0, -1.0, 76), (0.0, 1.0, 76),
        (-0.707, -0.707, 54), (0.707, -0.707, 54), (-0.707, 0.707, 54), (0.707, 0.707, 54),
    ]
    scale = ease_out_back(progress)
    glow = Image.new("RGBA", canvas.size, (0, 0, 0, 0))
    core = Image.new("RGBA", canvas.size, (0, 0, 0, 0))
    gd = ImageDraw.Draw(glow)
    cd = ImageDraw.Draw(core)
    for dx, dy, length in directions:
        inner = 8 * scale
        outer = length * scale
        x1, y1 = cx + dx * inner, cy + dy * inner
        x2, y2 = cx + dx * outer, cy + dy * outer
        gd.line((x1, y1, x2, y2), fill=(*ORANGE, round(180 * alpha)), width=22)
        cd.line((x1, y1, x2, y2), fill=(*ORANGE, round(255 * alpha)), width=5)
    gd.ellipse((cx - 18, cy - 18, cx + 18, cy + 18), fill=(*ORANGE, round(210 * alpha)))
    cd.ellipse((cx - 5, cy - 5, cx + 5, cy + 5), fill=(*OFF_WHITE, round(255 * alpha)))
    glow = glow.filter(ImageFilter.GaussianBlur(18))
    canvas.alpha_composite(glow)
    canvas.alpha_composite(core)


def draw_scan(canvas: Image.Image, x: float, alpha: float) -> None:
    if alpha <= 0.0:
        return
    glow = Image.new("RGBA", canvas.size, (0, 0, 0, 0))
    draw = ImageDraw.Draw(glow)
    y0, y1 = HEIGHT // 2 - 112, HEIGHT // 2 + 112
    draw.line((x, y0, x, y1), fill=(*ORANGE, round(220 * alpha)), width=4)
    glow = glow.filter(ImageFilter.GaussianBlur(14))
    canvas.alpha_composite(glow)
    hot = Image.new("RGBA", canvas.size, (0, 0, 0, 0))
    ImageDraw.Draw(hot).line((x, y0 + 12, x, y1 - 12), fill=(*OFF_WHITE, round(210 * alpha)), width=2)
    canvas.alpha_composite(hot)


def make_brand_assets() -> tuple[Image.Image, Image.Image]:
    wordmark = tracked_text_mask("SPARKENGINE", font(128, bold=True), 2)
    powered = tracked_text_mask("POWERED BY", font(26, bold=True), 9)

    logo = Image.new("RGBA", (2048, 512), (0, 0, 0, 0))
    draw_spark(logo, (190, 256), 1.0, 1.0)
    paste_color(logo, wordmark, (315, 180), OFF_WHITE + (255,))
    logo.save(ROOT / "sparkengine_wordmark_transparent.png", optimize=True)

    svg = """<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 2048 512\">
  <g stroke=\"#ff5200\" stroke-width=\"12\" stroke-linecap=\"square\">
    <path d=\"M114 256H32M266 256h82M190 180V98M190 332v82\"/>
    <path d=\"M136 202L78 144M244 202l58-58M136 310l-58 58M244 310l58 58\"/>
  </g>
  <circle cx=\"190\" cy=\"256\" r=\"8\" fill=\"#ece8de\"/>
  <text x=\"315\" y=\"326\" fill=\"#ece8de\" font-family=\"DejaVu Sans,Arial,sans-serif\" font-weight=\"700\" font-stretch=\"condensed\" font-size=\"170\" letter-spacing=\"2\">SPARKENGINE</text>
</svg>
"""
    (ROOT / "sparkengine_wordmark.svg").write_text(svg, encoding="utf-8")
    return wordmark, powered


def render_frames(wordmark: Image.Image, powered: Image.Image) -> None:
    FRAMES.mkdir(parents=True, exist_ok=True)
    logo_w = 154 + 52 + wordmark.width
    left = round((WIDTH - logo_w) / 2)
    spark_center = (left + 77, HEIGHT / 2)
    word_x = left + 154 + 52
    word_y = round(HEIGHT / 2 - wordmark.height / 2)
    power_x = word_x + 4
    power_y = word_y - 65

    for index in range(FRAME_COUNT):
        t = index / FPS
        fade_in = smoothstep(0.00, 0.18, t)
        fade_out = 1.0 - smoothstep(2.45, DURATION, t)
        master = fade_in * fade_out
        canvas = Image.new("RGBA", (WIDTH, HEIGHT), (*BLACK, 255))

        grid_alpha = smoothstep(0.05, 0.45, t) * (1.0 - smoothstep(2.15, 2.65, t))
        ring_pulse = smoothstep(0.20, 1.05, t)
        draw_grid(canvas, 0.55 * grid_alpha * master, ring_pulse)

        ignition = clamp((t - 0.18) / 0.55)
        settle = 1.0 - 0.14 * math.exp(-max(0.0, t - 0.73) * 4.0) * math.cos(max(0.0, t - 0.73) * 15.0)
        draw_spark(canvas, spark_center, ignition * settle, master)

        reveal = smoothstep(0.58, 1.24, t)
        word_reveal = Image.new("L", wordmark.size, 0)
        reveal_width = round(wordmark.width * reveal)
        if reveal_width > 0:
            word_reveal.paste(wordmark.crop((0, 0, reveal_width, wordmark.height)), (0, 0))
        paste_color(canvas, word_reveal, (word_x, word_y), (*OFF_WHITE, round(255 * master)))

        powered_alpha = smoothstep(0.92, 1.42, t) * master
        paste_color(canvas, powered, (power_x, power_y), (*MUTED, round(210 * powered_alpha)))

        if 0.52 <= t <= 1.38:
            scan_t = smoothstep(0.52, 1.30, t)
            scan_x = word_x - 22 + (wordmark.width + 44) * scan_t
            draw_scan(canvas, scan_x, math.sin(math.pi * scan_t) * master)

        # Technical registration marks give the hold a restrained blueprint character.
        marks_alpha = smoothstep(1.25, 1.62, t) * (1.0 - smoothstep(2.20, 2.58, t)) * master
        marks = Image.new("RGBA", canvas.size, (0, 0, 0, 0))
        md = ImageDraw.Draw(marks)
        y = HEIGHT / 2 + 137
        md.line((word_x, y, word_x + 108, y), fill=(*ORANGE, round(155 * marks_alpha)), width=2)
        md.line((word_x + 124, y, word_x + wordmark.width, y), fill=(*GRID, round(100 * marks_alpha)), width=1)
        md.text((word_x + wordmark.width - 174, y + 16), "C++23 / RUNTIME", font=font(19),
                fill=(*MUTED, round(145 * marks_alpha)))
        canvas.alpha_composite(marks)

        canvas.convert("RGB").save(FRAMES / f"frame_{index:04d}.png", compress_level=2)


def synthesize_audio() -> None:
    sample_rate = 48_000
    samples = []
    for i in range(round(sample_rate * DURATION)):
        t = i / sample_rate
        value = 0.0
        # Compact ignition transient.
        if 0.18 <= t < 0.55:
            x = t - 0.18
            env = math.exp(-x * 14.0)
            value += env * (0.46 * math.sin(2 * math.pi * (92 + 420 * x) * x))
            value += env * 0.14 * math.sin(2 * math.pi * 1800 * x)
        # Quiet rising electrical tone under the wordmark reveal.
        if 0.54 <= t < 1.42:
            x = (t - 0.54) / 0.88
            env = math.sin(math.pi * x) ** 2
            phase = 2 * math.pi * (310 * (t - 0.54) + 260 * (t - 0.54) ** 2)
            value += env * 0.11 * math.sin(phase)
        # Low, restrained resolve.
        if 1.08 <= t < 2.25:
            x = t - 1.08
            env = min(1.0, x * 9.0) * math.exp(-x * 2.4)
            value += env * (0.10 * math.sin(2 * math.pi * 82.41 * x) + 0.06 * math.sin(2 * math.pi * 123.47 * x))
        value = max(-0.95, min(0.95, value))
        samples.append(round(value * 32767))

    with wave.open(str(ROOT / "sparkengine_splash.wav"), "wb") as out:
        out.setnchannels(1)
        out.setsampwidth(2)
        out.setframerate(sample_rate)
        out.writeframes(b"".join(v.to_bytes(2, "little", signed=True) for v in samples))


def main() -> None:
    wordmark, powered = make_brand_assets()
    render_frames(wordmark, powered)
    synthesize_audio()


if __name__ == "__main__":
    main()
