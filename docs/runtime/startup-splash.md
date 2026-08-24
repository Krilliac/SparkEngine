# Runtime startup splash

Graphical SparkEngine launches play a 2.8-second, 16:9 engine signature before
the game window renders its first frame. The runtime animation is drawn by a
small deterministic CPU renderer, so startup does not depend on H.264, VP9, or
an installed media framework. The supplied 1080p masters and generator remain
in `Assets/Engine/Branding` for trailers, storefront media, and regeneration.

The splash is skipped automatically for `-headless`, `-dedicated`,
`-test-frames`, and `-test-seconds` launches. Pass `-no-splash` (or
`--no-splash`) to skip it for an ordinary graphical launch.

## Project override

Create `Config/StartupSplash.ini` in the project root:

```ini
[StartupSplash]
enabled=true
mute=false
duration=2.8
accent=#FF7818
image=Assets/Branding/my_startup.bmp
audio=Assets/Audio/my_startup.wav
```

`image` is optional. The built-in animated SparkEngine mark is used when it is
empty or cannot be read. Override images must be uncompressed 24-bit or 32-bit
BMP files; they are fitted without cropping and retain the scan/fade animation.
`audio` is optional PCM WAV. Missing or unavailable audio never blocks startup.
Set `mute=true` or pass `-no-splash-audio` to retain the animation without its cue.
Override paths must begin with `Assets/`, may not be absolute, and may not use
`..`; existing symlink/junction escapes are rejected. The root may be supplied by
`-project <directory-or.sparkproject>` or `SPARK_PROJECT_ROOT`; otherwise the
current working directory is used.

Command-line values override the project file:

```text
-splash <image.bmp>
-splash-audio <cue.wav>
-splash-duration <seconds>
```

Duration is clamped to 0.25–15 seconds. `-no-splash` always wins over all other
settings.

The runtime follows the 1920×1080 master's 16:9 design space and computes a
centered, aspect-preserving destination rectangle for every presentation
surface. Any remaining area is cleared to black, producing letterboxing
instead of crop or stretch.
