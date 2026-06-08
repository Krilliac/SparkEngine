# Third-Party Library Evaluation

> **Audience:** Programmers | Mixed
>
> **Thread Context:** N/A (research/reference)
>
> **Platform/Backend Scope:** All platforms (cross-platform dependency strategy)

## Overview

Comprehensive evaluation of third-party libraries for SparkEngine, addressing
cross-platform gaps (texture loading, model formats, audio) and deciding where to
replace hand-rolled implementations with battle-tested alternatives. Five libraries
were introduced as integration-ready dependencies, four more were rejected after
evaluation, and a short-list of future candidates was identified.

The guiding principle: introduce a library only when it closes a real cross-platform
gap or removes meaningful maintenance burden. Permissive licenses only — no GPL
contamination risk.

## Libraries Introduced

| Library | Location | Purpose | License | CMake Define |
|---------|----------|---------|---------|-------------|
| stb_image (+write) | `ThirdParty/Utils/stb/` | Cross-platform texture loading (PNG/JPG/BMP/TGA/HDR) | Public Domain | `SPARK_HAS_STB_IMAGE` |
| cgltf | `ThirdParty/Utils/cgltf/` | glTF 2.0 model parsing | MIT | `SPARK_HAS_CGLTF` |
| miniaudio | `ThirdParty/Audio/miniaudio/` | Cross-platform audio (WASAPI/PulseAudio/CoreAudio/ALSA) | Public Domain | `SPARK_HAS_MINIAUDIO` |
| nlohmann/json | `ThirdParty/Utils/json/` | JSON parsing (header-only) | MIT | `SPARK_HAS_NLOHMANN_JSON` |
| tinyexr | `ThirdParty/Utils/tinyexr/` | OpenEXR/HDR image loading | BSD 3-Clause | `SPARK_HAS_TINYEXR` |

> **Status note (verified 2026-06-08):** These remain **stub headers** that provide
> the correct API surface and compile/link cleanly, but full functionality requires
> dropping in the real single-header libraries from their upstream repos. The header
> banners explicitly say "This is a STUB header". Each now has a companion
> `*_impl.cpp` translation unit (`stb_impl.cpp`, `cgltf_impl.cpp`,
> `miniaudio_impl.cpp`, `tinyexr_impl.cpp`) so the implementation is compiled in one
> place once the real header is dropped in.

### How to Activate Full Implementations

```bash
# stb_image (replaces BMP/TGA-only stub with full PNG/JPG/HDR support)
curl -L https://raw.githubusercontent.com/nothings/stb/master/stb_image.h -o ThirdParty/Utils/stb/stb_image.h
curl -L https://raw.githubusercontent.com/nothings/stb/master/stb_image_write.h -o ThirdParty/Utils/stb/stb_image_write.h

# cgltf (replaces format-detection stub with full glTF parser)
curl -L https://raw.githubusercontent.com/jkuhlmann/cgltf/master/cgltf.h -o ThirdParty/Utils/cgltf/cgltf.h

# miniaudio (replaces no-op stub with real audio backend)
curl -L https://raw.githubusercontent.com/mackron/miniaudio/master/miniaudio.h -o ThirdParty/Audio/miniaudio/miniaudio.h

# nlohmann/json (replaces minimal parser with full-featured JSON)
curl -L https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp -o ThirdParty/Utils/json/nlohmann_json.h

# tinyexr (replaces stub with real EXR loader)
curl -L https://raw.githubusercontent.com/syoyo/tinyexr/master/tinyexr.h -o ThirdParty/Utils/tinyexr/tinyexr.h
```

## Gaps Addressed

| Gap | Before | After |
|-----|--------|-------|
| Texture loading on Linux | No-op (WIC is Windows-only) | stb_image loads PNG/JPG/BMP/TGA/HDR |
| glTF model loading | `E_NOTIMPL` stub | cgltf parses glTF 2.0 with PBR materials |
| Audio on Linux/macOS | Complete silence (XAudio2 stubs) | miniaudio backend (WASAPI/PulseAudio/CoreAudio) |
| JSON parsing | Hand-rolled 600-line parser | nlohmann/json (industry standard) |
| HDR/EXR image loading | Not supported | tinyexr loads OpenEXR for lightmaps/IBL |

## Integration Points

- **TextureSystem** (Linux path, `TextureSystemLinux.cpp`): `CreateFromFile` uses
  `stbi_load()` on non-Windows when `SPARK_HAS_STB_IMAGE` is defined. Also wired into
  the Linux `TextureAsset::Load` path (`AssetTypesLinux.cpp`) which decodes image bytes
  via stb_image and uploads through the RHI bridge.
- **ModelLoading** (`ModelLoadingLinux.cpp`): `LoadGLTF()` uses cgltf when
  `SPARK_HAS_CGLTF` is defined.
- **AudioEngine.h**: `IsAudioBackendAvailable()` returns true on non-Windows when
  `SPARK_HAS_MINIAUDIO` is defined.
- **JsonUtils.h** now provides the project JSON façade (`Spark::Json::Parse`). It is
  currently a self-contained minimal parser and switches to nlohmann/json when
  `SPARK_HAS_NLOHMANN_JSON` is available.
- tinyexr is available for use; EXR-specific wiring is opportunistic.

## Libraries NOT Recommended (Evaluated & Rejected)

| Library | Reason |
|---------|--------|
| assimp | Too heavy (~5 MB). cgltf/tinygltf is sufficient for glTF. Only if 40+ format support is ever needed. |
| spdlog | Custom Logger is adequate (async, category-filtered). Migration cost exceeds benefit. |
| Google Test / Catch2 | Custom TestFramework handles the full suite (~6,000 tests). Migration not justified. |
| GLM | Thousands of DirectXMath references across the codebase. Massive migration; DirectXMath stubs suffice on Linux. |
| TBB / Enki | JobSystem works. Replace only if work-stealing becomes a measured bottleneck. |
| protobuf | Save system works. Adds build complexity for marginal gain. |
| FreeType + HarfBuzz | MSDF text rendering already works. Runtime rasterization not needed. |

## Future Libraries to Consider

| Library | Purpose | Priority | Status (2026-06-08) |
|---------|---------|----------|---------------------|
| Basis Universal (transcoder) | GPU texture compression (4-8x VRAM savings) | High | **Partial** — `Graphics/BasisTranscoder.h` now exists, transcoding .basis/.ktx2 to BC/ASTC/ETC per backend. Real Basis transcoder library still needs dropping in. |
| Tracy Profiler | Production profiling with remote debugging | Medium | **Open** — wrap existing `PROFILE_SCOPE` macros. |
| meshoptimizer | The real upstream library | Low | **Open** — custom `Graphics/MeshOptimizer.h` already covers core cases. |
| FastNoiseLite | The real upstream library | Low | **Open** — custom `Graphics/FastNoiseLite.h` already integrated. |

## Architecture Notes

- All new libraries follow the existing pattern: `if(TARGET lib)` in CMake,
  `#if SPARK_HAS_*` in source.
- Stub headers provide a correct API surface so the engine compiles/links without the
  real libraries; a single `*_impl.cpp` per library centralizes the implementation.
- stb and miniaudio are single-file C libraries compiled as static libs.
- nlohmann/json is header-only (INTERFACE target).
- All licenses are permissive — no GPL contamination risk.

## Source & Freshness

- **Original entry date:** 2026-03-31 (`.claude/knowledge/third-party-library-evaluation.md`)
- **Verified against codebase 2026-06-08.**

Updates / status changes since the original:

- All five introduced libraries are **still stub headers** (header banners confirm
  "STUB"); not yet swapped for real upstream single-header libraries.
- Each library now has a companion `*_impl.cpp` translation unit centralizing the
  single-header implementation point — new since the original write-up.
- JSON usage now flows through `SparkEngine/Source/Utils/JsonUtils.h`
  (`Spark::Json`), a self-contained parser that upgrades to nlohmann/json under
  `SPARK_HAS_NLOHMANN_JSON`. The original noted nlohmann was "available but not yet
  wired" — there is now a project-wide JSON façade.
- stb_image is wired into the Linux `TextureAsset::Load` path
  (`AssetTypesLinux.cpp`), not just `TextureSystem` — newer integration.
- **Basis Universal** moved from "future / High priority" to **Partial**:
  `Graphics/BasisTranscoder.h` now exists (transcodes to BC1/BC3/BC7, ASTC, ETC1/ETC2
  per RHI backend). Real transcoder library still pending.
- Tracy, meshoptimizer, FastNoiseLite remain Open; the latter two are covered by
  custom in-tree headers.

## Related Pages

- [Engine Viability Evaluation](Engine-Viability-Evaluation.md)
- [Mac Compatibility Analysis](Mac-Compatibility-Analysis.md)
- [Engine Feature Recommendations](Engine-Feature-Recommendations.md)
- [Project Recommendations](Project-Recommendations.md)
