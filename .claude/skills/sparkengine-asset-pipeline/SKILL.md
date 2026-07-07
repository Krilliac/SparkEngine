---
name: sparkengine-asset-pipeline
description: >-
  Reference for SparkEngine's asset import/load pipeline in
  SparkEngine/Source/Graphics — how a source file (.obj/.fbx/.gltf/.glb, .png/.tga/.dds,
  .wav) becomes a runtime MeshAsset/TextureAsset/AudioAsset, what AssetMetadata tracks
  (guid, checksum, timestamps, fbx.* custom properties), and the non-obvious Windows/Linux
  platform-split (AssetPipelineWindows.cpp vs AssetPipelineLinux.cpp, D3D11 vs RHI/tinyobj/cgltf/stb_image).
  TRIGGER when: adding a new asset type or source format, editing AssetPipeline / AssetTypes /
  AssetMetadata / FBXImporter / ModelLoading, wondering "why is FBX loading Linux-only?",
  "why are there two .cpp files per asset class?", or "where does mesh loading actually happen?".
  DO NOT TRIGGER when: writing render-graph / RHI backend code (that is the RHI layer, not
  the asset layer — read SparkEngine/Source/Graphics/RHI directly), authoring shaders, or
  doing runtime ECS/gameplay work — use sparkengine-ecs-query-patterns for ECS instead.
  Phrases: "import an fbx", "add a texture format", "asset metadata", "hot reload assets",
  "why two files windows linux graphics".
trilobite_compatible: true
trilobite_role: reference
---

# SparkEngine asset pipeline

Runbook for the code under `SparkEngine/Source/Graphics/Asset*.{h,cpp}`,
`FBXImporter.{h,cpp}`, and `ModelLoading*.cpp`. It explains how a source
asset on disk becomes something the renderer can draw, what metadata is
tracked, and the platform-split file convention that trips up newcomers.

Definitions used once here:
- **RHI** — Render Hardware Interface, SparkEngine's cross-API graphics
  abstraction (`SparkEngine/Source/Graphics/RHI/`). The non-Windows path
  uploads geometry through it instead of raw D3D11.
- **TU** — translation unit (one `.cpp` as the compiler sees it after
  preprocessing).
- **ComPtr** — `Microsoft::WRL::ComPtr`, a smart pointer for D3D11 COM
  objects (Windows only).
- **Platform-split** — one logical class implemented across a
  `*Windows.cpp` + `*Linux.cpp` pair, each fully wrapped in an `#ifdef`
  so exactly one compiles per platform. Both files are always in the build.

## When NOT to use this skill

- You are changing an **RHI backend** (D3D12/Vulkan/Metal/OpenGL) or the
  render graph — that is `Graphics/RHI/` and `Graphics/RenderGraph/`, a
  different layer. This skill stops at "asset produces CPU data + one
  GPU/RHI buffer".
- You are touching **ECS components** that reference assets — that is
  `Engine/ECS/`.
- You want to author shaders — `SparkShaderCompiler/` and `Graphics/Shader*`.

## 30-second mental model

```
source file on disk (.obj/.fbx/.gltf/.png/.tga/.wav ...)
        |
   AssetPipeline::LoadAsset(path, type)      <- entry point, dedup + cache
        |  DetectAssetType() picks MeshAsset / TextureAsset / AudioAsset
        v
   LoadMeshFromFile / LoadTextureFromFile / LoadAudioFromFile
        |  constructs the Asset, calls Asset::Load(device)
        v
   MeshAsset::Load()  --- parses the file into MeshAssetData (CPU-side)
        |                 and creates GPU/RHI buffers
        v
   stored in m_assets (path -> shared_ptr<Asset>) + AssetCache (LRU)
        |
   runtime: BindMesh(path) / DrawBoundMesh(), GraphicsEngine::ProcessDrawList
```

There is **no separate "cook" / offline-import step and no on-disk
intermediate/`.meta` file**. Import is done live at load time, in-process,
every run. "Metadata" is an in-memory `AssetMetadata` struct rebuilt from
the filesystem on demand — it is not persisted. Do not look for a
`.import`/`.asset`/`.meta` cache; there isn't one as of 2026-07-07.

## Supported source formats (verified in code)

Two things decide format support, and they differ:

1. **Type detection** (`AssetPipeline::DetectAssetTypeFromExtension`,
   in `AssetMetadataWindows.cpp` / `AssetMetadataLinux.cpp`) — maps an
   extension to an `AssetType`.
2. **Actual parsing** (`MeshAsset::Load` / `TextureAsset::Load` /
   `AudioAsset::Load`, in `AssetTypesWindows.cpp` / `AssetTypesLinux.cpp`) —
   what the loader can really decode. These are NOT the same set.

| Asset | Windows parser (`AssetTypesWindows.cpp`) | Linux/macOS parser (`AssetTypesLinux.cpp`) |
|-------|------------------------------------------|--------------------------------------------|
| Mesh  | `.obj` only, hand-written OBJ parser; **fallback = unit cube** if unparsed | `.obj` (tinyobjloader), `.fbx` (`FBXImporter`), `.gltf`/`.glb` (cgltf, gated on `SPARK_HAS_CGLTF`); no fallback cube |
| Texture | `.tga` only (hand-written, image type 2, 24/32bpp); `.dds` logs "requires DirectXTex" and falls back; **fallback = 2x2 checkerboard** | any stb_image format — `.png/.jpg/.tga/.bmp/.hdr` — decoded to RGBA8 (gated on `SPARK_HAS_STB_IMAGE`), uploaded via the RHI bridge |
| Audio | `.wav` (RIFF/WAVE, PCM `audioFormat==1`); **fallback = 1s silence** | reads raw file bytes into `m_audioData`; no WAV header parse |

Key non-obvious facts (verify before relying on them — see Provenance):
- **FBX and glTF meshes only actually load on the non-Windows path.**
  `MeshAsset::Load` in `AssetTypesWindows.cpp` handles `.obj` and then
  falls back to a cube. FBX/glTF geometry parsing lives in
  `AssetTypesLinux.cpp` and `ModelLoadingLinux.cpp`. The
  `AssetPipeline::LoadFBX` / `LoadGLTF` private methods declared in
  `AssetPipeline.h` are **only defined in `ModelLoadingLinux.cpp`**
  (grep confirms no `::LoadFBX`/`::LoadGLTF` definition on Windows).
- `DetectAssetTypeFromExtension` lists `.fbx`, `.dae`, `.gltf`, `.glb`
  as Mesh on Windows even though the Windows loader won't parse them —
  detection is broader than parsing. Do not assume "detected => loadable".
- The two detection tables are **not identical**: Linux additionally maps
  `.mat/.anim/.prefab/.scene/.glsl/.cso/.bmp/.hdr`; Windows adds `.dae`
  and `.fx`. Keep both in sync when adding an extension.

### FBX importer specifics

`FBXImporter` (`FBXImporter.h/.cpp`) is a **self-contained binary-FBX
parser — no Autodesk FBX SDK dependency.** It is a singleton
(`FBXImporter::GetInstance()`), reads the `"Kaydara FBX Binary  "` magic,
walks the node tree, and extracts:
- meshes (`FBXMeshData`: flat vertex/normal/uv float arrays + indices),
- skeleton bones (`FBXBoneData`, column-major 4x4 bind pose),
- animation clips (`FBXAnimationData` / `FBXBoneTrack` / `FBXKeyframe`).

Import is configured with `FBXImportOptions` (scaleFactor, triangulate,
flipUVs, `targetCoordSystem` = LeftHanded/D3D by default, importAnimations).
`CanImport(path)` checks magic bytes; `GetSupportedExtensions()` returns
`{".fbx"}`; `ValidateForPipeline(...)` gates on optional skeleton/animation
requirements. The Linux `MeshAsset::Load` calls `Import` then
`ValidateForPipeline(result, false, false, &err)` and, on success, copies
meshes into `MeshAssetData`, calls `GenerateTangents` + `ComputeBounds`,
and records `fbx.meshCount` / `fbx.boneCount` / `fbx.animationCount` into
metadata custom properties.

## AssetMetadata — what is tracked

Struct `AssetMetadata` in `AssetPipeline.h` (lines ~140-154). Populated by
`AssetPipeline::GetAssetMetadata` and inside each `Asset::Load`:

| Field | Meaning | Where set |
|-------|---------|-----------|
| `guid` | Unique id string | declared; **not populated by any Load path** as of 2026-07-07 (candidate/unused) |
| `filePath` | Original source path | GetAssetMetadata, every Load |
| `name` | `path.stem()` (filename without extension) | GetAssetMetadata, every Load |
| `type` | `AssetType` enum from extension | DetectAssetType |
| `fileSize` | `std::filesystem::file_size` | GetAssetMetadata / Load |
| `memorySize` | `GetMemoryUsage()` result | set at end of each Load |
| `lastModified` | timestamp; ms since epoch (Win) / `st_mtime` seconds (Linux) | `GetFileTimestamp` |
| `checksum` | content hash — **NOT cryptographic**: Windows `hash*31+byte` as decimal; Linux `checksum*31+byte` as hex. The two platforms produce different strings for the same file. | `CalculateChecksum` |
| `dependencies` | asset dependency list | declared; not populated (candidate) |
| `priority`, `state` | `LoadingPriority` / `StreamingState` | Linux GetAssetMetadata sets defaults; state set in Load |
| `customProperties` | `map<string,string>`; FBX writes `fbx.meshCount/boneCount/animationCount` here | Linux FBX Load |

Do not describe the checksum as MD5/SHA — the Windows source comment says
"in production would use MD5/SHA"; it is a rolling multiply-add. Two
platforms are not comparable.

## The Windows/Linux platform-split convention (read this before editing)

Almost every asset class is spread across a `*Windows.cpp` + `*Linux.cpp`
pair. **Both files are always compiled** (the engine sources are added with
`file(GLOB_RECURSE ...)` in the root `CMakeLists.txt`, so nothing is
excluded per-platform at the CMake level). Instead, each file wraps its
entire body in a guard:

- Windows file: `#include "Core/Platform.h"` then `#ifdef SPARK_PLATFORM_WINDOWS ... #endif`
- Linux/macOS file: `#include "Core/Platform.h"` then `#ifndef SPARK_PLATFORM_WINDOWS ... #endif`

So on any given platform, one of the pair is an empty TU. This is why you
see `AssetPipeline.cpp` / `AssetMetadata.cpp` reduced to a one-comment
"redirect" stub — the real code moved into the platform files.

The file map:

| Logical unit | Cross-platform | Windows / D3D11 | Linux+macOS / RHI |
|--------------|----------------|-----------------|-------------------|
| Pipeline lifecycle, load orchestration, cache, streaming | `AssetPipeline.cpp` (stub) | `AssetPipelineWindows.cpp` | `AssetPipelineLinux.cpp` |
| Asset classes `Load/Unload/GetMemoryUsage`, `AssetCache` | `AssetTypes.cpp` (ctors/dtors, `SetRHIBuffers/Texture`) | `AssetTypesWindows.cpp` | `AssetTypesLinux.cpp` |
| Type detection, dir scan, metadata, checksums, enum-to-string | `AssetMetadata.cpp` (stub) | `AssetMetadataWindows.cpp` | `AssetMetadataLinux.cpp` |
| `LoadMeshFromFile` + format loaders (OBJ/FBX/glTF) + bind/draw | `ModelLoading.cpp` | `ModelLoadingWindows.cpp` | `ModelLoadingLinux.cpp` |
| Console commands | `AssetPipelineConsoleOps.cpp` (check its own guards) | — | — |

What genuinely differs between the two variants (not just formatting):

1. **GPU upload path.** Windows creates D3D11 `ID3D11Buffer` /
   `ID3D11Texture2D` directly inside `MeshAsset::Load` /
   `TextureAsset::Load` via the passed `ID3D11Device*`. Linux/macOS keeps
   only CPU data in `Load`, then `AssetPipeline::LoadMesh` calls
   `BuildRHIBuffersForMesh(mesh)` which uploads `mesh.GetMeshData()` into
   RHI buffers and hands ownership back via `MeshAsset::SetRHIBuffers`.
   On Windows `BuildRHIBuffersForMesh` is a deliberate **no-op** (defined
   so callers need no `#ifdef`).
2. **The RHI members exist on both** (`m_rhiVertexBuffer`,
   `m_rhiIndexBuffer`, `m_rhiTexture` in `AssetPipeline.h`) but are only
   populated on the non-Windows path; Windows leaves them `nullptr` and
   drives the `ComPtr` members instead. Accessors:
   `GetVertexBuffer()`/`GetSRV()` (D3D11, Windows) vs
   `GetRHIVertexBuffer()`/`GetRHITexture()` (RHI, non-Windows).
3. **Format coverage** differs as shown in the format table above (Windows
   is intentionally minimal + fallback shapes; Linux uses real loader libs).
4. **Detection tables** differ (see earlier warning).
5. Small behavioral differences: Linux `ScanDirectory` sorts results and
   swallows `filesystem_error`; Linux `GetAssetMetadata` first returns a
   loaded asset's live metadata; Linux `CheckForChangedAssets` actually
   reloads on timestamp change while the Windows `CheckForChangedAssets` /
   `RefreshAssetMetadata` bodies are still stubs (comment-only).

Why keep the cross-platform ctor/dtor in `AssetTypes.cpp`? The
`unique_ptr<Spark::RHI::IRHIBuffer>` members use a **forward-declared**
type in the header (to keep RHI headers out of every TU). `unique_ptr`
needs the complete type at ctor-cleanup and dtor, so those special members
are defined out-of-line in the one TU that includes `RHI/RHIResources.h`.
Do not move them inline into the header or you drag RHI transitively
everywhere and risk ODR issues.

## Runbook: add a new source format to an existing asset type

Example: add `.stl` mesh support.

1. **Detection** — add the extension to `DetectAssetTypeFromExtension` in
   **both** `AssetMetadataWindows.cpp` and `AssetMetadataLinux.cpp` (map to
   `AssetType::Mesh`). If they drift, the format loads on one OS only.
2. **Parsing** — add a branch in `MeshAsset::Load`:
   - `AssetTypesWindows.cpp` for the D3D11 path (fill `m_meshData`, then the
     existing code creates the D3D11 buffers), and/or
   - `AssetTypesLinux.cpp` for the RHI path (fill `m_meshData`, call
     `GenerateTangents` + `ComputeBounds`; buffers are built later by
     `BuildRHIBuffersForMesh`).
3. **Third-party loader?** If you pull in a library, wire it in the root
   `CMakeLists.txt` the same way tinyobj/cgltf/stb are (see lines ~663-1214),
   gate the include with a `SPARK_HAS_*` define, and add it under
   `ThirdParty/`. Keep private code out of cloud offload.
4. **Bounds + tangents** — meshes must end with valid
   `boundingBoxMin/Max`, `boundingSphere*`, and per-vertex tangents;
   reuse `ComputeBounds` / `GenerateTangents` (Linux) or the inline bbox
   loop (Windows).
5. **Metadata** — optionally record loader stats into
   `m_metadata.customProperties` (follow the `fbx.*` naming).

## Runbook: add a whole new AssetType

1. Add the enum value to `AssetType` in `AssetPipeline.h`.
2. Extend `AssetTypeToString` / `StringToAssetType` in **both**
   `AssetMetadataWindows.cpp` and `AssetMetadataLinux.cpp` (a `default`
   returning "Unknown" hides missed cases — check every switch).
3. Add extension mapping in both `DetectAssetTypeFromExtension` bodies.
4. If it needs a new `Asset` subclass, declare it in `AssetPipeline.h`
   (mirror `MeshAsset`: out-of-line ctor/dtor if it holds RHI members),
   define `Load/Unload/GetMemoryUsage` in **both** `AssetTypes*.cpp`
   guarded by the platform `#ifdef`, and add a `LoadXFromFile` +
   `switch` case in `LoadAsset` in **both** `AssetPipeline*.cpp`.
5. Rebuild on Windows AND cross-check the Linux guard compiles (CI job
   `build-linux-gcc` will catch a missing Linux implementation because the
   `#ifndef` TU would leave the method undefined at link time).

## Runbook: add a new platform variant (e.g. a real macOS split)

macOS currently rides the `#ifndef SPARK_PLATFORM_WINDOWS` (Linux) path.
If you need a genuinely separate variant:

1. Check what `SPARK_PLATFORM_*` macros `Core/Platform.h` defines before
   inventing a new guard.
2. Follow the existing pattern: new `Asset*Mac.cpp`, whole body inside the
   platform guard, `#include "Core/Platform.h"` first. GLOB_RECURSE will
   pick it up automatically — you do **not** add it to CMake explicitly.
3. Make sure the previous default path's `#ifndef` doesn't also fire for
   your new platform, or you'll get duplicate-symbol link errors (both TUs
   non-empty). Adjust the guards to be mutually exclusive.

## Common gotchas

- "My FBX loads a cube on Windows." Expected — Windows `MeshAsset::Load`
  doesn't parse FBX and falls back to the unit cube. Test FBX/glTF on the
  Linux/RHI path (or via the CI Linux jobs).
- "`GetVertexBuffer()` returns null on Linux." Expected — use
  `GetRHIVertexBuffer()`; the D3D11 ComPtrs are Windows-only.
- Editing only the Windows file and expecting CI green: the Linux/Clang/GCC
  and macOS CI jobs compile the `#ifndef` twins. Change both.
- Checksums are not stable across OS and not cryptographic — do not use
  them for content-addressing or cross-machine dedup.

## Provenance and maintenance

Verified 2026-07-07 against the files below (all under
`SparkEngine/Source/Graphics/`). Re-verify with:

```bash
# Confirm the platform-split pairs still exist
ls SparkEngine/Source/Graphics/Asset*.cpp SparkEngine/Source/Graphics/ModelLoading*.cpp

# Confirm FBX/glTF loaders are defined only on the Linux side
grep -rn "::LoadFBX\|::LoadGLTF\|::LoadOBJ" SparkEngine/Source/Graphics/

# Confirm the extension->type tables (compare the two)
grep -n "extension ==" SparkEngine/Source/Graphics/AssetMetadataWindows.cpp SparkEngine/Source/Graphics/AssetMetadataLinux.cpp

# Confirm supported mesh/texture parsers per platform
grep -n "ext == \|stbi_load\|tinyobj::LoadObj\|cgltf_parse_file\|FBXImporter::GetInstance" \
  SparkEngine/Source/Graphics/AssetTypesWindows.cpp SparkEngine/Source/Graphics/AssetTypesLinux.cpp

# Confirm third-party loader wiring + feature macros
grep -n "SPARK_HAS_STB_IMAGE\|SPARK_HAS_CGLTF\|tinyobjloader" CMakeLists.txt

# AssetMetadata field set
grep -n "std::string\|size_t\|uint64_t\|AssetType\|customProperties" \
  SparkEngine/Source/Graphics/AssetPipeline.h | sed -n '1,60p'
```

Volatile claims to re-check if code moved:
- FBX/glTF being Linux-only in `MeshAsset::Load` (Windows may gain a real
  importer — then update the format table and gotchas).
- `guid` / `dependencies` still unpopulated (marked candidate above).
- Whether Windows `CheckForChangedAssets` / `RefreshAssetMetadata` are still
  stubs.

Validate this file after edits with the global skill linter
(`~/.claude/skills/skill-linter`), pointing `-Path` at
`.claude/skills/sparkengine-asset-pipeline`.
