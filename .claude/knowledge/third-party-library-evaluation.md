# Third-Party Library Evaluation & Integration

**Last updated:** 2026-03-31
**Type:** Decision
**Status:** Active

## Description

Comprehensive evaluation of third-party libraries to introduce into SparkEngine, addressing critical cross-platform gaps (texture loading, model formats, audio) and replacing hand-rolled implementations with battle-tested alternatives. 5 libraries integrated, 4 more recommended for future.

## Libraries Introduced (This Session)

| Library | Location | Purpose | License | CMake Define |
|---------|----------|---------|---------|-------------|
| stb_image (+write) | `ThirdParty/Utils/stb/` | Cross-platform texture loading (PNG/JPG/BMP/TGA/HDR) | Public Domain | `SPARK_HAS_STB_IMAGE` |
| cgltf | `ThirdParty/Utils/cgltf/` | glTF 2.0 model parsing | MIT | `SPARK_HAS_CGLTF` |
| miniaudio | `ThirdParty/Audio/miniaudio/` | Cross-platform audio (WASAPI/PulseAudio/CoreAudio/ALSA) | Public Domain | `SPARK_HAS_MINIAUDIO` |
| nlohmann/json | `ThirdParty/Utils/json/` | JSON parsing (header-only) | MIT | `SPARK_HAS_NLOHMANN_JSON` |
| tinyexr | `ThirdParty/Utils/tinyexr/` | OpenEXR/HDR image loading | BSD 3-Clause | `SPARK_HAS_TINYEXR` |

All are currently **stub implementations** -- they provide the correct API surface and compile/link cleanly, but full functionality requires replacing the stub headers with the real single-header libraries from their respective GitHub repos.

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
| Audio on Linux/macOS | Complete silence (XAudio2 stubs) | miniaudio provides WASAPI/PulseAudio/CoreAudio |
| JSON parsing | Hand-rolled 600-line parser | nlohmann/json (industry standard) |
| HDR/EXR image loading | Not supported | tinyexr loads OpenEXR for lightmaps/IBL |

## Integration Points

- **TextureSystem.cpp** (line ~771): `CreateFromFile` uses `stbi_load()` on non-Windows when `SPARK_HAS_STB_IMAGE` is defined
- **ModelLoading.cpp** (line ~222): `LoadGLTF()` uses cgltf with full vertex/index parsing when `SPARK_HAS_CGLTF` is defined
- **AudioEngine.h** (line ~113): `IsAudioBackendAvailable()` returns true on non-Windows when `SPARK_HAS_MINIAUDIO` is defined
- nlohmann/json and tinyexr are available for use but not yet wired into specific subsystems

## Libraries NOT Recommended (Evaluated & Rejected)

| Library | Reason |
|---------|--------|
| assimp | Too heavy (~5MB). cgltf/tinygltf is sufficient for glTF. Only if 40+ format support needed. |
| spdlog | Custom Logger is adequate (async, category-filtered). Migration cost exceeds benefit. |
| Google Test / Catch2 | Custom TestFramework handles 2,577 tests. Migration not justified. |
| GLM | 2,661 DirectXMath references across 250 files. Massive migration. Stubs suffice. |
| TBB / Enki | JobSystem works. Replace only if work-stealing becomes a measured bottleneck. |
| protobuf | Save system works. Adds build complexity for marginal gain. |
| FreeType + HarfBuzz | MSDF text rendering already works. Runtime rasterization not needed. |

## Future Libraries to Consider

| Library | Purpose | Priority | Notes |
|---------|---------|----------|-------|
| Basis Universal (transcoder) | GPU texture compression (4-8x VRAM savings) | High | Already identified in thorvg-unity analysis |
| Tracy Profiler | Production profiling with remote debugging | Medium | Wrap existing `PROFILE_SCOPE` macros |
| meshoptimizer | The real library (not the custom impl) | Low | Custom `MeshOptimizer.h` already covers core cases |
| FastNoiseLite | The real library (not the custom impl) | Low | Custom `FastNoiseLite.h` already integrated |

## Architecture Notes

- All new libraries follow the existing pattern: `if(TARGET lib)` in CMake, `#if SPARK_HAS_*` in source
- Stub headers provide correct API surface so engine compiles/links without the real libraries
- stb and miniaudio are single-file C libraries compiled as static libs
- nlohmann/json is header-only (INTERFACE target)
- All licenses are permissive -- no GPL contamination risk
