#!/usr/bin/env python3
"""
Generate default placeholder assets for SparkEngine.

Creates minimal but valid PNG textures and WAV audio files that the engine
and editor reference. These are small, procedurally generated placeholders
intended to prevent missing-asset errors during development.

All generated assets are free to use (public domain / CC0) since they are
procedurally generated and contain no third-party content.

Usage:
    python3 tools/generate_default_assets.py
"""

import struct
import zlib
import math
import os
import wave
import array

ASSET_ROOT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "Assets")


# ---------------------------------------------------------------------------
# PNG writer (minimal, no dependencies)
# ---------------------------------------------------------------------------

def _make_png(width: int, height: int, pixels: list[tuple[int, int, int, int]]) -> bytes:
    """Create a minimal RGBA PNG from a flat list of (R, G, B, A) tuples."""

    def _chunk(chunk_type: bytes, data: bytes) -> bytes:
        c = chunk_type + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)

    header = b"\x89PNG\r\n\x1a\n"
    ihdr = _chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))

    raw = b""
    for y in range(height):
        raw += b"\x00"  # filter: none
        for x in range(width):
            r, g, b, a = pixels[y * width + x]
            raw += struct.pack("BBBB", r, g, b, a)

    idat = _chunk(b"IDAT", zlib.compress(raw, 9))
    iend = _chunk(b"IEND", b"")

    return header + ihdr + idat + iend


def make_solid(width: int, height: int, color: tuple[int, int, int, int]) -> bytes:
    return _make_png(width, height, [color] * (width * height))


def make_checkerboard(width: int, height: int, size: int = 8,
                      c1: tuple = (200, 200, 200, 255),
                      c2: tuple = (100, 100, 100, 255)) -> bytes:
    pixels = []
    for y in range(height):
        for x in range(width):
            pixels.append(c1 if ((x // size) + (y // size)) % 2 == 0 else c2)
    return _make_png(width, height, pixels)


def make_normal_flat(width: int, height: int) -> bytes:
    """Flat normal map (pointing straight up): RGB = (128, 128, 255)."""
    return make_solid(width, height, (128, 128, 255, 255))


def make_grid(width: int, height: int, spacing: int = 16,
              bg: tuple = (40, 40, 40, 255),
              line: tuple = (80, 80, 80, 255)) -> bytes:
    pixels = []
    for y in range(height):
        for x in range(width):
            pixels.append(line if x % spacing == 0 or y % spacing == 0 else bg)
    return _make_png(width, height, pixels)


def make_gradient_v(width: int, height: int,
                    top: tuple = (100, 150, 220),
                    bottom: tuple = (20, 30, 50)) -> bytes:
    """Vertical gradient for sky textures."""
    pixels = []
    for y in range(height):
        t = y / max(height - 1, 1)
        r = int(top[0] * (1 - t) + bottom[0] * t)
        g = int(top[1] * (1 - t) + bottom[1] * t)
        b = int(top[2] * (1 - t) + bottom[2] * t)
        for _ in range(width):
            pixels.append((r, g, b, 255))
    return _make_png(width, height, pixels)


def make_circle(width: int, height: int,
                fg: tuple = (180, 180, 180, 255),
                bg: tuple = (0, 0, 0, 0)) -> bytes:
    """Circle/disc shape for decals."""
    cx, cy = width / 2, height / 2
    radius = min(cx, cy) - 1
    pixels = []
    for y in range(height):
        for x in range(width):
            dx, dy = x - cx + 0.5, y - cy + 0.5
            dist = math.sqrt(dx * dx + dy * dy)
            if dist <= radius:
                alpha = int(fg[3] * max(0.0, 1.0 - (dist / radius) ** 2))
                pixels.append((fg[0], fg[1], fg[2], alpha))
            else:
                pixels.append(bg)
    return _make_png(width, height, pixels)


def make_noise(width: int, height: int, seed: int = 42) -> bytes:
    """Simple pseudo-random noise texture."""
    # Simple LCG for reproducibility without importing random
    state = seed
    pixels = []
    for _ in range(width * height):
        state = (state * 1103515245 + 12345) & 0x7FFFFFFF
        v = (state >> 16) & 0xFF
        pixels.append((v, v, v, 255))
    return _make_png(width, height, pixels)


# ---------------------------------------------------------------------------
# WAV writer
# ---------------------------------------------------------------------------

def make_wav_sine(duration_s: float, freq_hz: float, sample_rate: int = 22050,
                  amplitude: float = 0.5) -> bytes:
    """Generate a mono 16-bit WAV with a sine tone."""
    import io
    n_samples = int(sample_rate * duration_s)
    samples = array.array("h")  # signed short
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


def make_wav_noise(duration_s: float, sample_rate: int = 22050,
                   amplitude: float = 0.3, seed: int = 123) -> bytes:
    """Generate a mono 16-bit WAV with pseudo-random noise."""
    import io
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


def make_wav_impact(duration_s: float = 0.15, sample_rate: int = 22050) -> bytes:
    """Short impact/click sound with fast decay."""
    import io
    n_samples = int(sample_rate * duration_s)
    state = 777
    samples = array.array("h")
    for i in range(n_samples):
        t = i / sample_rate
        env = max(0.0, 1.0 - t / duration_s) ** 3  # fast exponential decay
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


def make_wav_sweep(duration_s: float = 0.5, freq_start: float = 800,
                   freq_end: float = 100, sample_rate: int = 22050) -> bytes:
    """Frequency sweep (descending) for explosion-like sounds."""
    import io
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
        # Mix sine with noise for richness
        state = (state * 1103515245 + 12345) & 0x7FFFFFFF
        noise = (state >> 16) / 32768.0 - 1.0
        val = int(0.6 * 32767 * env * (0.6 * math.sin(phase) + 0.4 * noise))
        samples.append(max(-32768, min(32767, val)))

    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(sample_rate)
        w.writeframes(samples.tobytes())
    return buf.getvalue()


# ---------------------------------------------------------------------------
# Asset generation
# ---------------------------------------------------------------------------

def write_file(rel_path: str, data: bytes) -> None:
    full = os.path.join(ASSET_ROOT, rel_path)
    os.makedirs(os.path.dirname(full), exist_ok=True)
    with open(full, "wb") as f:
        f.write(data)
    size_kb = len(data) / 1024
    print(f"  {rel_path:50s}  ({size_kb:.1f} KB)")


def generate_textures() -> None:
    print("\n=== Textures ===")

    # Default engine textures
    write_file("Textures/Default/checkerboard.png",
               make_checkerboard(64, 64, 8))
    write_file("Textures/Default/white.png",
               make_solid(4, 4, (255, 255, 255, 255)))
    write_file("Textures/Default/black.png",
               make_solid(4, 4, (0, 0, 0, 255)))
    write_file("Textures/Default/normal_flat.png",
               make_normal_flat(4, 4))
    write_file("Textures/Default/grid.png",
               make_grid(64, 64, 16))
    write_file("Textures/Default/uv_test.png",
               make_checkerboard(64, 64, 8,
                                 c1=(255, 0, 255, 255),
                                 c2=(0, 255, 0, 255)))
    write_file("Textures/Default/noise.png",
               make_noise(64, 64))

    # Textures referenced by SearchPanel / editor code
    write_file("Textures/concrete_diffuse.png",
               make_solid(32, 32, (160, 155, 145, 255)))
    write_file("Textures/concrete_normal.png",
               make_normal_flat(32, 32))
    write_file("Textures/metal_diffuse.png",
               make_solid(32, 32, (180, 185, 190, 255)))
    write_file("Textures/wood_diffuse.png",
               make_solid(32, 32, (140, 100, 60, 255)))

    # Terrain textures
    write_file("Textures/Terrain/grass.png",
               make_solid(32, 32, (80, 130, 60, 255)))
    write_file("Textures/Terrain/dirt.png",
               make_solid(32, 32, (120, 90, 60, 255)))
    write_file("Textures/Terrain/rock.png",
               make_solid(32, 32, (130, 125, 120, 255)))
    write_file("Textures/Terrain/sand.png",
               make_solid(32, 32, (210, 195, 150, 255)))
    write_file("Textures/Terrain/snow.png",
               make_solid(32, 32, (235, 240, 245, 255)))

    # Decal textures (referenced by DecalSystem.cpp)
    write_file("Textures/Decals/bullet_hole.png",
               make_circle(32, 32, fg=(30, 30, 30, 220)))
    write_file("Textures/Decals/bullet_hole_normal.png",
               make_normal_flat(32, 32))
    write_file("Textures/Decals/scorch_mark.png",
               make_circle(32, 32, fg=(50, 40, 30, 200)))
    write_file("Textures/Decals/blood_splatter.png",
               make_circle(32, 32, fg=(130, 20, 20, 200)))

    # Sky
    write_file("Textures/Sky/sky_gradient.png",
               make_gradient_v(64, 64, top=(135, 180, 235), bottom=(50, 70, 120)))

    # Loading screen
    write_file("Textures/loading_bg.png",
               make_gradient_v(128, 64, top=(30, 30, 40), bottom=(10, 10, 15)))


def generate_audio() -> None:
    print("\n=== Audio ===")

    # Referenced by SearchPanel and prompt docs
    write_file("Audio/gunshot.wav",
               make_wav_impact(0.12))
    write_file("Audio/explosion.wav",
               make_wav_sweep(0.6, 600, 60))
    write_file("Audio/footstep.wav",
               make_wav_impact(0.08))
    write_file("Audio/ambient_wind.wav",
               make_wav_noise(2.0, amplitude=0.1, seed=314))
    write_file("Audio/music_combat.wav",
               make_wav_sine(2.0, 220, amplitude=0.3))

    # Common FPS sounds
    write_file("Audio/reload.wav",
               make_wav_impact(0.2))
    write_file("Audio/hit_marker.wav",
               make_wav_sine(0.1, 1200, amplitude=0.4))
    write_file("Audio/ui_click.wav",
               make_wav_sine(0.05, 800, amplitude=0.3))
    write_file("Audio/ui_hover.wav",
               make_wav_sine(0.03, 600, amplitude=0.2))
    write_file("Audio/pickup.wav",
               make_wav_sine(0.15, 520, amplitude=0.35))
    write_file("Audio/jump.wav",
               make_wav_sweep(0.2, 200, 400))
    write_file("Audio/land.wav",
               make_wav_impact(0.1))
    write_file("Audio/damage.wav",
               make_wav_sweep(0.25, 500, 150))
    write_file("Audio/death.wav",
               make_wav_sweep(0.8, 400, 40))


def generate_materials() -> None:
    print("\n=== Materials ===")

    materials = {
        "Default.json": {
            "name": "Default",
            "shader": "PBR",
            "albedo": "Textures/Default/checkerboard.png",
            "normal": "Textures/Default/normal_flat.png",
            "metallic": 0.0,
            "roughness": 0.5,
            "ao": 1.0,
        },
        "Concrete.json": {
            "name": "Concrete",
            "shader": "PBR",
            "albedo": "Textures/concrete_diffuse.png",
            "normal": "Textures/concrete_normal.png",
            "metallic": 0.0,
            "roughness": 0.85,
            "ao": 1.0,
        },
        "Metal.json": {
            "name": "Metal",
            "shader": "PBR",
            "albedo": "Textures/metal_diffuse.png",
            "normal": "Textures/Default/normal_flat.png",
            "metallic": 0.9,
            "roughness": 0.3,
            "ao": 1.0,
        },
        "Wood.json": {
            "name": "Wood",
            "shader": "PBR",
            "albedo": "Textures/wood_diffuse.png",
            "normal": "Textures/Default/normal_flat.png",
            "metallic": 0.0,
            "roughness": 0.7,
            "ao": 1.0,
        },
        "Terrain_Grass.json": {
            "name": "Terrain_Grass",
            "shader": "PBR",
            "albedo": "Textures/Terrain/grass.png",
            "normal": "Textures/Default/normal_flat.png",
            "metallic": 0.0,
            "roughness": 0.9,
            "ao": 1.0,
        },
        "Terrain_Dirt.json": {
            "name": "Terrain_Dirt",
            "shader": "PBR",
            "albedo": "Textures/Terrain/dirt.png",
            "normal": "Textures/Default/normal_flat.png",
            "metallic": 0.0,
            "roughness": 0.95,
            "ao": 1.0,
        },
    }

    import json
    for filename, mat in materials.items():
        data = json.dumps(mat, indent=4).encode("utf-8")
        write_file(f"Materials/{filename}", data)


def generate_default_scene() -> None:
    print("\n=== Scenes ===")

    import json
    scene = {
        "name": "Default",
        "version": 1,
        "description": "Default empty scene with basic setup",
        "entities": [
            {
                "name": "DirectionalLight",
                "components": {
                    "Transform": {"position": [0, 10, 0], "rotation": [50, -30, 0], "scale": [1, 1, 1]},
                    "Light": {"type": "Directional", "color": [1.0, 0.95, 0.85], "intensity": 1.0,
                              "castShadows": True},
                },
            },
            {
                "name": "Camera",
                "components": {
                    "Transform": {"position": [0, 2, -5], "rotation": [0, 0, 0], "scale": [1, 1, 1]},
                    "Camera": {"fov": 75.0, "near": 0.1, "far": 1000.0, "primary": True},
                },
            },
            {
                "name": "Floor",
                "components": {
                    "Transform": {"position": [0, 0, 0], "rotation": [0, 0, 0], "scale": [50, 1, 50]},
                    "MeshRenderer": {"mesh": "Primitive/Cube", "material": "Materials/Default.json"},
                    "RigidBody": {"type": "Static", "mass": 0},
                    "BoxCollider": {"halfExtents": [25, 0.5, 25]},
                },
            },
            {
                "name": "SpawnPoint",
                "components": {
                    "Transform": {"position": [0, 1, 0], "rotation": [0, 0, 0], "scale": [1, 1, 1]},
                    "SpawnPoint": {"team": 0, "index": 0},
                },
            },
        ],
        "environment": {
            "ambientColor": [0.15, 0.15, 0.2],
            "skyTexture": "Textures/Sky/sky_gradient.png",
            "fogEnabled": False,
            "fogColor": [0.7, 0.75, 0.8],
            "fogStart": 50.0,
            "fogEnd": 200.0,
        },
    }

    data = json.dumps(scene, indent=4).encode("utf-8")
    write_file("Scenes/Default.scene", data)


def main() -> None:
    print(f"Generating default assets in: {ASSET_ROOT}")
    generate_textures()
    generate_audio()
    generate_materials()
    generate_default_scene()
    print(f"\nDone! All default assets generated in {ASSET_ROOT}/")


if __name__ == "__main__":
    main()
