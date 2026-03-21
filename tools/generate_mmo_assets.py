#!/usr/bin/env python3
"""
Generate placeholder assets for SparkGameMMO.

Creates procedurally generated textures, audio, and OBJ models for the MMO
module. All assets are CC0/public domain (procedurally generated, no
third-party content).

Usage:
    python3 tools/generate_mmo_assets.py
"""

import struct
import zlib
import math
import os
import wave
import array
import io
import json

ASSET_ROOT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "Assets")


# ---------------------------------------------------------------------------
# PNG writer (same as generate_default_assets.py)
# ---------------------------------------------------------------------------

def _make_png(width: int, height: int, pixels: list) -> bytes:
    def _chunk(chunk_type: bytes, data: bytes) -> bytes:
        c = chunk_type + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)

    header = b"\x89PNG\r\n\x1a\n"
    ihdr = _chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
    raw = b""
    for y in range(height):
        raw += b"\x00"
        for x in range(width):
            r, g, b, a = pixels[y * width + x]
            raw += struct.pack("BBBB", r, g, b, a)
    idat = _chunk(b"IDAT", zlib.compress(raw, 9))
    iend = _chunk(b"IEND", b"")
    return header + ihdr + idat + iend


def make_solid(w, h, color):
    return _make_png(w, h, [color] * (w * h))


def make_normal_flat(w, h):
    return make_solid(w, h, (128, 128, 255, 255))


def make_noise_color(w, h, base_color, variation=30, seed=42):
    """Noisy texture based on a base color for natural-looking surfaces."""
    state = seed
    pixels = []
    for _ in range(w * h):
        state = (state * 1103515245 + 12345) & 0x7FFFFFFF
        v = ((state >> 16) & 0xFF) / 255.0
        offset = int((v - 0.5) * 2 * variation)
        r = max(0, min(255, base_color[0] + offset))
        g = max(0, min(255, base_color[1] + offset))
        b = max(0, min(255, base_color[2] + offset))
        pixels.append((r, g, b, 255))
    return _make_png(w, h, pixels)


def make_brick_pattern(w, h, brick_color=(140, 120, 100), mortar_color=(90, 85, 75),
                       brick_w=16, brick_h=8, mortar=1):
    """Brick/cobblestone pattern."""
    pixels = []
    for y in range(h):
        row = y // brick_h
        offset = (brick_w // 2) if row % 2 == 1 else 0
        for x in range(w):
            ax = (x + offset) % w
            if y % brick_h < mortar or ax % brick_w < mortar:
                pixels.append((*mortar_color, 255))
            else:
                # Add subtle noise
                v = ((x * 7 + y * 13 + row * 37) % 20) - 10
                r = max(0, min(255, brick_color[0] + v))
                g = max(0, min(255, brick_color[1] + v))
                b = max(0, min(255, brick_color[2] + v))
                pixels.append((r, g, b, 255))
    return _make_png(w, h, pixels)


def make_gradient_radial(w, h, center_color, edge_color):
    """Radial gradient from center to edges."""
    cx, cy = w / 2, h / 2
    max_dist = math.sqrt(cx * cx + cy * cy)
    pixels = []
    for y in range(h):
        for x in range(w):
            dx, dy = x - cx, y - cy
            t = min(1.0, math.sqrt(dx * dx + dy * dy) / max_dist)
            r = int(center_color[0] * (1 - t) + edge_color[0] * t)
            g = int(center_color[1] * (1 - t) + edge_color[1] * t)
            b = int(center_color[2] * (1 - t) + edge_color[2] * t)
            pixels.append((r, g, b, 255))
    return _make_png(w, h, pixels)


# ---------------------------------------------------------------------------
# WAV writer
# ---------------------------------------------------------------------------

def make_wav_noise(duration_s, sample_rate=22050, amplitude=0.3, seed=123):
    n_samples = int(sample_rate * duration_s)
    state = seed
    samples = array.array("h")
    for _ in range(n_samples):
        state = (state * 1103515245 + 12345) & 0x7FFFFFFF
        val = int(amplitude * 32767 * ((state >> 16) / 32768.0 - 1.0))
        samples.append(max(-32768, min(32767, val)))
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(sample_rate)
        w.writeframes(samples.tobytes())
    return buf.getvalue()


def make_wav_sine(duration_s, freq_hz, sample_rate=22050, amplitude=0.5):
    n_samples = int(sample_rate * duration_s)
    samples = array.array("h")
    for i in range(n_samples):
        t = i / sample_rate
        val = int(amplitude * 32767 * math.sin(2 * math.pi * freq_hz * t))
        samples.append(max(-32768, min(32767, val)))
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(sample_rate)
        w.writeframes(samples.tobytes())
    return buf.getvalue()


def make_wav_ambient(duration_s, base_freq=80, harmonics=4, sample_rate=22050, amplitude=0.15, seed=42):
    """Low-frequency ambient drone with harmonics and filtered noise."""
    n_samples = int(sample_rate * duration_s)
    state = seed
    samples = array.array("h")
    for i in range(n_samples):
        t = i / sample_rate
        # Base drone
        val = 0.0
        for h in range(1, harmonics + 1):
            val += math.sin(2 * math.pi * base_freq * h * t) / h
        val *= 0.5
        # Filtered noise
        state = (state * 1103515245 + 12345) & 0x7FFFFFFF
        noise = ((state >> 16) / 32768.0 - 1.0) * 0.2
        val = (val + noise) * amplitude
        samples.append(max(-32768, min(32767, int(val * 32767))))
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(sample_rate)
        w.writeframes(samples.tobytes())
    return buf.getvalue()


def make_wav_impact(duration_s=0.15, sample_rate=22050):
    n_samples = int(sample_rate * duration_s)
    state = 777
    samples = array.array("h")
    for i in range(n_samples):
        t = i / sample_rate
        env = max(0.0, 1.0 - t / duration_s) ** 3
        state = (state * 1103515245 + 12345) & 0x7FFFFFFF
        noise = (state >> 16) / 32768.0 - 1.0
        val = int(0.7 * 32767 * env * noise)
        samples.append(max(-32768, min(32767, val)))
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(sample_rate)
        w.writeframes(samples.tobytes())
    return buf.getvalue()


def make_wav_sweep(duration_s, freq_start, freq_end, sample_rate=22050, amplitude=0.5):
    n_samples = int(sample_rate * duration_s)
    samples = array.array("h")
    state = 999
    phase = 0.0
    for i in range(n_samples):
        t = i / sample_rate
        progress = t / duration_s
        freq = freq_start * (1 - progress) + freq_end * progress
        env = max(0.0, 1.0 - progress) ** 2
        phase += 2 * math.pi * freq / sample_rate
        state = (state * 1103515245 + 12345) & 0x7FFFFFFF
        noise = (state >> 16) / 32768.0 - 1.0
        val = int(amplitude * 32767 * env * (0.6 * math.sin(phase) + 0.4 * noise))
        samples.append(max(-32768, min(32767, val)))
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(sample_rate)
        w.writeframes(samples.tobytes())
    return buf.getvalue()


def make_wav_fanfare(duration_s=2.0, sample_rate=22050):
    """Rising chord fanfare for achievement/level-up."""
    n_samples = int(sample_rate * duration_s)
    samples = array.array("h")
    notes = [261.6, 329.6, 392.0, 523.3]  # C4 E4 G4 C5
    for i in range(n_samples):
        t = i / sample_rate
        progress = t / duration_s
        env = min(1.0, t * 5) * max(0.0, 1.0 - progress * 0.5)
        val = 0.0
        for j, freq in enumerate(notes):
            start = j * 0.15
            if t >= start:
                local_env = min(1.0, (t - start) * 8) * env
                val += math.sin(2 * math.pi * freq * t) * local_env / len(notes)
        samples.append(max(-32768, min(32767, int(val * 0.4 * 32767))))
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(sample_rate)
        w.writeframes(samples.tobytes())
    return buf.getvalue()


# ---------------------------------------------------------------------------
# OBJ writer
# ---------------------------------------------------------------------------

def make_obj_cube() -> str:
    """Unit cube centered at origin."""
    return """# Cube mesh
v -0.5 -0.5  0.5
v  0.5 -0.5  0.5
v  0.5  0.5  0.5
v -0.5  0.5  0.5
v -0.5 -0.5 -0.5
v  0.5 -0.5 -0.5
v  0.5  0.5 -0.5
v -0.5  0.5 -0.5
vn  0  0  1
vn  0  0 -1
vn  1  0  0
vn -1  0  0
vn  0  1  0
vn  0 -1  0
f 1//1 2//1 3//1 4//1
f 6//2 5//2 8//2 7//2
f 2//3 6//3 7//3 3//3
f 5//4 1//4 4//4 8//4
f 4//5 3//5 7//5 8//5
f 5//6 6//6 2//6 1//6
"""


def make_obj_cylinder(segments=12, height=1.0, radius=0.5) -> str:
    """Vertical cylinder centered at origin."""
    lines = ["# Cylinder mesh"]
    # Vertices
    for i in range(segments):
        angle = 2 * math.pi * i / segments
        x = radius * math.cos(angle)
        z = radius * math.sin(angle)
        lines.append(f"v {x:.4f} {-height/2:.4f} {z:.4f}")
        lines.append(f"v {x:.4f} {height/2:.4f} {z:.4f}")
    # Top/bottom centers
    lines.append(f"v 0.0 {-height/2:.4f} 0.0")
    lines.append(f"v 0.0 {height/2:.4f} 0.0")
    lines.append("vn 0 -1 0")
    lines.append("vn 0  1 0")

    bottom_center = segments * 2 + 1
    top_center = segments * 2 + 2

    for i in range(segments):
        i0 = i * 2 + 1
        i1 = i * 2 + 2
        j0 = ((i + 1) % segments) * 2 + 1
        j1 = ((i + 1) % segments) * 2 + 2
        # Side quad
        lines.append(f"f {i0} {j0} {j1} {i1}")
        # Bottom triangle
        lines.append(f"f {bottom_center}//1 {j0}//1 {i0}//1")
        # Top triangle
        lines.append(f"f {top_center}//2 {i1}//2 {j1}//2")

    return "\n".join(lines) + "\n"


def make_obj_pyramid(base=1.0, height=1.5) -> str:
    """Simple pyramid."""
    h2 = base / 2
    return f"""# Pyramid mesh
v {-h2} 0 {h2}
v {h2} 0 {h2}
v {h2} 0 {-h2}
v {-h2} 0 {-h2}
v 0 {height} 0
vn 0 -1 0
f 1 4 3 2
f 1 2 5
f 2 3 5
f 3 4 5
f 4 1 5
"""


# ---------------------------------------------------------------------------
# File writer
# ---------------------------------------------------------------------------

def write_file(rel_path: str, data) -> None:
    full = os.path.join(ASSET_ROOT, rel_path)
    os.makedirs(os.path.dirname(full), exist_ok=True)
    mode = "wb" if isinstance(data, bytes) else "w"
    with open(full, mode) as f:
        f.write(data)
    size_kb = len(data) / 1024 if isinstance(data, bytes) else len(data.encode()) / 1024
    print(f"  {rel_path:55s}  ({size_kb:.1f} KB)")


# ---------------------------------------------------------------------------
# MMO Textures
# ---------------------------------------------------------------------------

def generate_mmo_textures():
    print("\n=== MMO Textures ===")

    # Environment textures
    write_file("Textures/MMO/cobblestone_diffuse.png",
               make_brick_pattern(64, 64, brick_color=(140, 130, 115), mortar_color=(85, 80, 70),
                                  brick_w=12, brick_h=6))
    write_file("Textures/MMO/cobblestone_normal.png", make_normal_flat(64, 64))

    write_file("Textures/MMO/stone_diffuse.png",
               make_noise_color(64, 64, (130, 125, 118), variation=20, seed=101))
    write_file("Textures/MMO/stone_normal.png", make_normal_flat(64, 64))

    write_file("Textures/MMO/dungeon_stone_diffuse.png",
               make_brick_pattern(64, 64, brick_color=(55, 50, 60), mortar_color=(30, 28, 35),
                                  brick_w=16, brick_h=8, mortar=2))
    write_file("Textures/MMO/dungeon_stone_normal.png", make_normal_flat(64, 64))

    write_file("Textures/MMO/foliage_diffuse.png",
               make_noise_color(64, 64, (50, 120, 40), variation=35, seed=202))

    write_file("Textures/MMO/fabric_diffuse.png",
               make_noise_color(32, 32, (140, 100, 70), variation=15, seed=303))

    write_file("Textures/MMO/npc_guard_diffuse.png",
               make_noise_color(32, 32, (100, 110, 130), variation=10, seed=404))

    # Terrain variants
    write_file("Textures/Terrain/grass_diffuse.png",
               make_noise_color(64, 64, (75, 130, 55), variation=25, seed=505))
    write_file("Textures/Terrain/rock_diffuse.png",
               make_noise_color(64, 64, (130, 125, 115), variation=20, seed=606))
    write_file("Textures/Terrain/sand_diffuse.png",
               make_noise_color(64, 64, (210, 195, 150), variation=15, seed=707))

    # UI textures
    write_file("Textures/MMO/UI/panel_bg.png",
               make_solid(64, 64, (20, 22, 28, 220)))
    write_file("Textures/MMO/UI/panel_border.png",
               make_solid(4, 4, (100, 85, 55, 255)))
    write_file("Textures/MMO/UI/button_normal.png",
               make_noise_color(32, 16, (60, 55, 45), variation=5, seed=801))
    write_file("Textures/MMO/UI/button_hover.png",
               make_noise_color(32, 16, (80, 70, 50), variation=5, seed=802))
    write_file("Textures/MMO/UI/button_pressed.png",
               make_noise_color(32, 16, (45, 40, 35), variation=5, seed=803))
    write_file("Textures/MMO/UI/health_bar.png",
               make_solid(64, 8, (180, 40, 40, 255)))
    write_file("Textures/MMO/UI/mana_bar.png",
               make_solid(64, 8, (40, 80, 200, 255)))
    write_file("Textures/MMO/UI/xp_bar.png",
               make_solid(64, 8, (160, 140, 40, 255)))
    write_file("Textures/MMO/UI/stamina_bar.png",
               make_solid(64, 8, (60, 170, 60, 255)))
    write_file("Textures/MMO/UI/bar_bg.png",
               make_solid(64, 8, (20, 20, 20, 200)))
    write_file("Textures/MMO/UI/minimap_frame.png",
               make_gradient_radial(64, 64, (40, 45, 55), (15, 15, 20)))
    write_file("Textures/MMO/UI/tooltip_bg.png",
               make_solid(4, 4, (15, 15, 20, 240)))

    # Item rarity border colors
    write_file("Textures/MMO/UI/rarity_common.png",
               make_solid(4, 4, (160, 160, 160, 255)))
    write_file("Textures/MMO/UI/rarity_uncommon.png",
               make_solid(4, 4, (30, 180, 30, 255)))
    write_file("Textures/MMO/UI/rarity_rare.png",
               make_solid(4, 4, (30, 100, 255, 255)))
    write_file("Textures/MMO/UI/rarity_epic.png",
               make_solid(4, 4, (160, 50, 220, 255)))
    write_file("Textures/MMO/UI/rarity_legendary.png",
               make_solid(4, 4, (255, 165, 0, 255)))

    # Class/race icons (simple colored squares for now)
    class_colors = {
        "warrior": (180, 50, 50), "ranger": (50, 150, 50), "mage": (80, 80, 220),
        "healer": (220, 220, 80), "engineer": (180, 120, 50)
    }
    for name, color in class_colors.items():
        write_file(f"Textures/MMO/UI/icon_{name}.png",
                   make_gradient_radial(32, 32, color, (color[0] // 3, color[1] // 3, color[2] // 3)))

    race_colors = {
        "human": (200, 180, 160), "elf": (140, 200, 160), "dwarf": (180, 150, 100), "orc": (120, 160, 100)
    }
    for name, color in race_colors.items():
        write_file(f"Textures/MMO/UI/icon_{name}.png",
                   make_gradient_radial(32, 32, color, (color[0] // 3, color[1] // 3, color[2] // 3)))

    # Chat channel icons
    channel_colors = {
        "chat_area": (200, 200, 200), "chat_global": (255, 200, 50),
        "chat_party": (100, 150, 255), "chat_whisper": (220, 100, 220)
    }
    for name, color in channel_colors.items():
        write_file(f"Textures/MMO/UI/{name}.png",
                   make_solid(16, 16, (*color, 255)))


# ---------------------------------------------------------------------------
# MMO Audio
# ---------------------------------------------------------------------------

def generate_mmo_audio():
    print("\n=== MMO Audio ===")

    # Ambient loops (3 seconds each for placeholder)
    write_file("Audio/MMO/ambient_town.wav",
               make_wav_ambient(3.0, base_freq=120, harmonics=3, amplitude=0.08, seed=100))
    write_file("Audio/MMO/ambient_wilderness.wav",
               make_wav_ambient(3.0, base_freq=60, harmonics=5, amplitude=0.12, seed=200))
    write_file("Audio/MMO/ambient_dungeon.wav",
               make_wav_ambient(3.0, base_freq=40, harmonics=6, amplitude=0.1, seed=300))
    write_file("Audio/MMO/ambient_arena.wav",
               make_wav_ambient(3.0, base_freq=100, harmonics=2, amplitude=0.06, seed=400))

    # Music stubs (5 seconds each)
    write_file("Audio/MMO/music_town.wav",
               make_wav_sine(5.0, 262, amplitude=0.2))   # C4
    write_file("Audio/MMO/music_combat.wav",
               make_wav_sine(5.0, 165, amplitude=0.25))  # E3
    write_file("Audio/MMO/music_dungeon.wav",
               make_wav_sine(5.0, 110, amplitude=0.2))   # A2
    write_file("Audio/MMO/music_boss.wav",
               make_wav_sine(5.0, 82.4, amplitude=0.3))  # E2
    write_file("Audio/MMO/music_victory.wav",
               make_wav_fanfare(2.0))

    # UI sounds
    write_file("Audio/MMO/ui_login.wav",
               make_wav_fanfare(1.0))
    write_file("Audio/MMO/ui_error.wav",
               make_wav_sweep(0.3, 400, 200, amplitude=0.4))
    write_file("Audio/MMO/ui_success.wav",
               make_wav_sine(0.2, 880, amplitude=0.3))
    write_file("Audio/MMO/ui_levelup.wav",
               make_wav_fanfare(1.5))
    write_file("Audio/MMO/ui_achievement.wav",
               make_wav_fanfare(1.0))
    write_file("Audio/MMO/ui_chat_msg.wav",
               make_wav_sine(0.05, 600, amplitude=0.15))
    write_file("Audio/MMO/ui_inventory_open.wav",
               make_wav_sine(0.1, 500, amplitude=0.2))
    write_file("Audio/MMO/ui_equip.wav",
               make_wav_impact(0.15))
    write_file("Audio/MMO/ui_craft_start.wav",
               make_wav_sweep(0.3, 300, 600, amplitude=0.3))
    write_file("Audio/MMO/ui_craft_complete.wav",
               make_wav_sine(0.3, 660, amplitude=0.3))
    write_file("Audio/MMO/ui_trade_complete.wav",
               make_wav_sine(0.25, 523, amplitude=0.25))
    write_file("Audio/MMO/ui_quest_complete.wav",
               make_wav_fanfare(1.2))

    # Combat sounds
    write_file("Audio/MMO/combat_sword_hit.wav",
               make_wav_impact(0.12))
    write_file("Audio/MMO/combat_magic_cast.wav",
               make_wav_sweep(0.4, 200, 800, amplitude=0.35))
    write_file("Audio/MMO/combat_heal.wav",
               make_wav_sweep(0.5, 400, 800, amplitude=0.25))
    write_file("Audio/MMO/combat_block.wav",
               make_wav_impact(0.08))
    write_file("Audio/MMO/combat_crit.wav",
               make_wav_sweep(0.2, 1000, 300, amplitude=0.5))
    write_file("Audio/MMO/combat_death.wav",
               make_wav_sweep(0.8, 400, 40, amplitude=0.4))
    write_file("Audio/MMO/combat_respawn.wav",
               make_wav_sweep(0.6, 200, 500, amplitude=0.3))

    # World boss
    write_file("Audio/MMO/boss_roar.wav",
               make_wav_sweep(1.0, 80, 40, amplitude=0.6))
    write_file("Audio/MMO/boss_ability_warn.wav",
               make_wav_sine(0.5, 440, amplitude=0.4))
    write_file("Audio/MMO/boss_phase_change.wav",
               make_wav_sweep(1.5, 100, 300, amplitude=0.5))
    write_file("Audio/MMO/boss_defeated.wav",
               make_wav_fanfare(2.0))


# ---------------------------------------------------------------------------
# MMO Models (simple OBJ placeholders)
# ---------------------------------------------------------------------------

def generate_mmo_models():
    print("\n=== MMO Models ===")

    # Simple placeholder models
    write_file("Models/MMO/fountain.obj", make_obj_cylinder(16, 2.0, 1.5))
    write_file("Models/MMO/market_stall.obj", make_obj_cube())
    write_file("Models/MMO/guild_hall.obj", make_obj_cube())
    write_file("Models/MMO/forge.obj", make_obj_cube())
    write_file("Models/MMO/alchemy_shop.obj", make_obj_cube())
    write_file("Models/MMO/gate.obj", make_obj_cube())
    write_file("Models/MMO/tree_pine.obj", make_obj_pyramid(2.0, 4.0))
    write_file("Models/MMO/rock_large.obj", make_obj_cube())
    write_file("Models/MMO/tent.obj", make_obj_pyramid(3.0, 2.0))
    write_file("Models/MMO/torch.obj", make_obj_cylinder(8, 1.0, 0.1))
    write_file("Models/MMO/chest.obj", make_obj_cube())
    write_file("Models/MMO/anvil.obj", make_obj_cube())
    write_file("Models/MMO/cauldron.obj", make_obj_cylinder(12, 0.8, 0.5))
    write_file("Models/MMO/banner.obj", make_obj_cube())
    write_file("Models/MMO/gravestone.obj", make_obj_cube())
    write_file("Models/MMO/pillar.obj", make_obj_cylinder(8, 3.0, 0.3))


# ---------------------------------------------------------------------------
# Asset manifest
# ---------------------------------------------------------------------------

def generate_asset_manifest():
    print("\n=== Asset Manifest ===")

    manifest = {
        "name": "SparkGameMMO Assets",
        "version": "1.0.0",
        "description": "Placeholder assets for the SparkGameMMO module",
        "license": "CC0 (procedurally generated, no third-party content)",
        "generator": "tools/generate_mmo_assets.py",
        "recommended_replacements": {
            "models": [
                {"source": "Kenney.nl", "url": "https://kenney.nl/assets", "license": "CC0",
                 "packs": ["Fantasy Town Kit", "Nature Kit", "Dungeon Pack"]},
                {"source": "Quaternius", "url": "https://quaternius.com/packs/", "license": "CC0",
                 "packs": ["Ultimate Fantasy Pack", "Ultimate Modular Dungeon"]},
                {"source": "Kay Lousberg", "url": "https://kaylousberg.com/game-assets", "license": "CC0",
                 "packs": ["Fantasy Characters", "Dungeon Assets"]}
            ],
            "textures": [
                {"source": "ambientCG", "url": "https://ambientcg.com/", "license": "CC0",
                 "note": "PBR materials: ground, stone, metal, wood, fabric"},
                {"source": "Polyhaven Textures", "url": "https://polyhaven.com/textures", "license": "CC0",
                 "note": "High-quality PBR texture sets"},
                {"source": "Kenney.nl", "url": "https://kenney.nl/assets/category:Textures", "license": "CC0",
                 "note": "Pixel-art and stylized texture packs"}
            ],
            "audio": [
                {"source": "Kenney.nl", "url": "https://kenney.nl/assets/category:Audio", "license": "CC0",
                 "packs": ["RPG Audio", "UI Audio", "Impact Sounds"]},
                {"source": "Freesound.org", "url": "https://freesound.org/", "license": "CC0 (filter)",
                 "note": "Filter by CC0 license for royalty-free sounds"},
                {"source": "OpenGameArt.org", "url": "https://opengameart.org/art-search-advanced?keys=&field_art_type_tid%5B%5D=13", "license": "Various CC",
                 "note": "RPG/Fantasy sound effects and music"}
            ],
            "fonts": [
                {"source": "Google Fonts", "url": "https://fonts.google.com/", "license": "OFL/Apache",
                 "note": "Cinzel for fantasy headings, Roboto for UI"},
                {"source": "Font Squirrel", "url": "https://www.fontsquirrel.com/", "license": "Various",
                 "note": "Many free commercial-use fonts"}
            ]
        },
        "categories": {
            "textures": {
                "environment": [
                    "Textures/MMO/cobblestone_diffuse.png",
                    "Textures/MMO/stone_diffuse.png",
                    "Textures/MMO/dungeon_stone_diffuse.png",
                    "Textures/MMO/foliage_diffuse.png",
                    "Textures/MMO/fabric_diffuse.png"
                ],
                "ui": [
                    "Textures/MMO/UI/panel_bg.png",
                    "Textures/MMO/UI/health_bar.png",
                    "Textures/MMO/UI/mana_bar.png",
                    "Textures/MMO/UI/xp_bar.png"
                ],
                "icons": [
                    "Textures/MMO/UI/icon_warrior.png",
                    "Textures/MMO/UI/icon_ranger.png",
                    "Textures/MMO/UI/icon_mage.png",
                    "Textures/MMO/UI/icon_healer.png",
                    "Textures/MMO/UI/icon_engineer.png"
                ]
            },
            "audio": {
                "ambient": [
                    "Audio/MMO/ambient_town.wav",
                    "Audio/MMO/ambient_wilderness.wav",
                    "Audio/MMO/ambient_dungeon.wav",
                    "Audio/MMO/ambient_arena.wav"
                ],
                "music": [
                    "Audio/MMO/music_town.wav",
                    "Audio/MMO/music_combat.wav",
                    "Audio/MMO/music_dungeon.wav",
                    "Audio/MMO/music_boss.wav"
                ],
                "ui": [
                    "Audio/MMO/ui_login.wav",
                    "Audio/MMO/ui_levelup.wav",
                    "Audio/MMO/ui_achievement.wav",
                    "Audio/MMO/ui_craft_complete.wav"
                ],
                "combat": [
                    "Audio/MMO/combat_sword_hit.wav",
                    "Audio/MMO/combat_magic_cast.wav",
                    "Audio/MMO/combat_heal.wav",
                    "Audio/MMO/combat_crit.wav"
                ]
            },
            "models": [
                "Models/MMO/fountain.obj",
                "Models/MMO/market_stall.obj",
                "Models/MMO/guild_hall.obj",
                "Models/MMO/tree_pine.obj",
                "Models/MMO/torch.obj"
            ],
            "scenes": [
                "Scenes/MMO/town_square.scene",
                "Scenes/MMO/wilderness.scene",
                "Scenes/MMO/shadow_crypt.scene",
                "Scenes/MMO/battleground.scene"
            ]
        }
    }

    write_file("MMO/asset_manifest.json", json.dumps(manifest, indent=2).encode())


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    print(f"Generating MMO assets in: {ASSET_ROOT}")

    generate_mmo_textures()
    generate_mmo_audio()
    generate_mmo_models()
    generate_asset_manifest()

    print(f"\nDone! All MMO placeholder assets generated in {ASSET_ROOT}/")
    print("\nTo replace with production-quality assets, see Assets/MMO/asset_manifest.json")
    print("for recommended free asset sources (all CC0/public domain).\n")


if __name__ == "__main__":
    main()
