# SparkEngine Startup Splash

The default SparkEngine runtime signature: a 2.8-second, 16:9 animation built around the project's orange starburst, near-black industrial palette, and technical blueprint language.

## Sequence

- **0.00–0.58 s** — a pinpoint ignition expands into the eight-ray SparkEngine mark.
- **0.52–1.42 s** — a hot scan line resolves the `SPARKENGINE` wordmark and restrained `POWERED BY` label.
- **1.42–2.45 s** — clean logo hold with a small `C++23 / RUNTIME` registration detail.
- **2.45–2.80 s** — fade to black for a seamless hand-off into the game.

The accompanying electrical ignition/resolve cue is deliberately short and quiet. Games can mute it while retaining the animation.

## Deliverables

- `sparkengine_splash_1080p.mp4` — H.264/AAC distribution master, 1920×1080 at 60 fps.
- `sparkengine_splash_1080p.webm` — VP9/Opus open-codec master, 1920×1080 at 60 fps.
- `sparkengine_splash_preview.gif` — lightweight 960×540 preview.
- `sparkengine_splash.wav` — 48 kHz, mono, 16-bit PCM sound cue.
- `sparkengine_wordmark.svg` — scalable mark and wordmark.
- `sparkengine_wordmark_transparent.png` — transparent raster wordmark.
- `generate_splash.py` — deterministic source generator.

## Engine behavior target

Ship this as the runtime default for graphical builds. Play once per process launch before the first game frame, skip it for headless/dedicated servers and automated tests, and support `-no-splash` plus a project-level override. Preserve aspect ratio with black letterboxing rather than cropping the logo.
