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

## Logomark

The four-point "spark" logomark used as the app icon of SparkEditor, SparkLauncher, SparkInstaller and SparkCrashReporter, and drawn in-engine by `Spark::Ui::DrawLogomark`.

- `sparkengine_logomark.svg` — 256×256 outlined paths (no live text) filled with the ember gradient `#e8611a` → `#f1823a` → `#ffc857`, top-left to bottom-right.
- `sparkengine.ico` — PNG-in-ICO with the 16, 20, 24, 32, 40, 48, 64 and 256 px sizes.
- `generate_logomark.py` — stdlib-only deterministic generator (`python generate_logomark.py [--out-dir DIR]`). Its geometry constants (unit square, four-point concave star centred at 0.5/0.5, outer radius 0.46, inner radius 0.13, ember dot at 0.78/0.22 with radius 0.075) are shared verbatim with `Spark::Ui::DrawLogomark`; change both together.

The three files are recorded under `LicenseRef-SparkOpenLicense-1.0` by the `engine-logomark` rule in `tools/asset-integrity/provenance.json`, bound to their SHA-256 digests. After regenerating, re-pin the digests in that rule and regenerate `Assets/assets.integrity.json` with `python tools/asset-integrity/verify_asset_integrity.py generate Assets`.
