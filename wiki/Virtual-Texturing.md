# Virtual Texturing

SparkEngine includes a virtual texturing system that keeps only the needed texture pages resident in a GPU page cache. A shader-driven feedback buffer reports which pages are visible each frame, and the CPU-side manager loads requested pages and evicts stale ones using LRU.

**Source:** `SparkEngine/Source/Graphics/VirtualTexture.h`
**Namespace:** `Spark::Graphics`

---

## Table of Contents

- [Overview](#overview)
- [Page Cache Architecture](#page-cache-architecture)
- [Feedback Loop](#feedback-loop)
- [Configuration](#configuration)
- [API Reference](#api-reference)
- [Usage Example](#usage-example)
- [Integration](#integration)
- [See Also](#see-also)

---

## Overview

Traditional texture streaming loads entire mip levels of each texture. Virtual texturing instead divides textures into tiles (pages) and loads only the tiles visible on screen, dramatically reducing VRAM usage for scenes with many large textures.

```
┌───────────────────────────────────────────────────────────┐
│                      GPU Rendering                        │
│                                                           │
│  Pixel shader writes to feedback buffer:                  │
│    "I need texture 5, mip 3 at tile (2, 1)"             │
│                                                           │
├───────────────────────────────────────────────────────────┤
│                      CPU Readback                         │
│                                                           │
│  ProcessFeedback():                                       │
│    - Parse feedback entries                               │
│    - Mark needed pages as used (update LRU)              │
│    - Queue missing pages for async loading               │
│    - Evict stale pages when cache is full                │
│                                                           │
├───────────────────────────────────────────────────────────┤
│                      Page Cache                           │
│                                                           │
│  ┌─────┬─────┬─────┬─────┬─────┬─────┐                  │
│  │ T1  │ T3  │ T5  │ T5  │ T2  │free │  Resident pages  │
│  │ M0  │ M2  │ M3  │ M1  │ M0  │     │                  │
│  │(0,0)│(1,0)│(2,1)│(0,0)│(3,2)│     │                  │
│  └─────┴─────┴─────┴─────┴─────┴─────┘                  │
│  Max 1024 pages (configurable)                            │
└───────────────────────────────────────────────────────────┘
```

---

## Page Cache Architecture

### Virtual Texture Page

Each page is one tile of a specific mip level of a source texture:

```cpp
struct VirtualTexturePage
{
    uint32_t textureId;       // Source texture identifier
    uint32_t mipLevel;        // Mip level this page belongs to
    uint32_t tileX;           // Tile column within the mip level
    uint32_t tileY;           // Tile row within the mip level
    bool resident;            // True if loaded in the cache
    uint32_t lastUsedFrame;   // LRU tracking
};
```

### Feedback Entry

Shaders write what they need into the feedback buffer:

```cpp
struct FeedbackEntry
{
    uint32_t textureId;     // Texture that was sampled
    uint32_t requiredMip;   // Mip level needed for this pixel
};
```

---

## Feedback Loop

Each frame follows this cycle:

1. **Render**: Pixel shaders sample virtual textures and write `FeedbackEntry` records to a GPU feedback buffer
2. **Readback**: The feedback buffer is read back to CPU
3. **ProcessFeedback**: The manager analyzes entries:
   - Pages already resident get their `lastUsedFrame` updated
   - Missing pages are queued for async loading
   - When the cache is full, the least-recently-used pages are evicted
4. **Upload**: Newly loaded page data is uploaded to the GPU page cache texture
5. **Update indirection**: The indirection table (maps virtual page → physical cache slot) is updated

---

## Configuration

| Parameter | Default | Description |
|-----------|---------|-------------|
| `pageCacheSize` | 1024 | Maximum resident pages |
| `tileSize` | 128 | Pixels per tile edge |
| `framesToKeep` | 60 | Frames a page stays resident without use |

At default settings: 1024 pages x 128x128 pixels x 4 bytes (RGBA8) = **64 MB** page cache.

---

## API Reference

| Method | Description |
|--------|-------------|
| `Initialize(pageCacheSize, tileSize)` | Set up page cache |
| `Shutdown()` | Release all resources |
| `ProcessFeedback(feedback, currentFrame)` | Analyze GPU feedback, schedule loads/evictions |
| `RequestPage(textureId, mip, tileX, tileY)` | Manually request a specific page |
| `EvictStalePages(currentFrame, framesToKeep)` | Remove unused pages (returns eviction count) |
| `GetResidentPageCount()` | Pages currently in cache |
| `GetPendingPageCount()` | Pages queued for loading |
| `Console_GetStatus()` | Formatted debug string |

---

## Usage Example

```cpp
using namespace Spark::Graphics;

auto& vtm = VirtualTextureManager::GetInstance();
vtm.Initialize(2048, 128);  // 2048 pages, 128px tiles

// Each frame after rendering:
std::vector<FeedbackEntry> feedback = ReadbackFeedbackBuffer();
vtm.ProcessFeedback(feedback, frameNumber);

// Periodic cleanup (e.g., every 30 frames)
if (frameNumber % 30 == 0)
{
    uint32_t evicted = vtm.EvictStalePages(frameNumber, 60);
    if (evicted > 0)
    {
        LOG_INFO("Evicted {} stale virtual texture pages", evicted);
    }
}

// Debug output
LOG_INFO("VT: {} resident, {} pending", 
         vtm.GetResidentPageCount(), vtm.GetPendingPageCount());
```

---

## Integration

- **Asset Pipeline**: Source textures are pre-tiled during asset import. See [Asset Pipeline](Asset-Pipeline)
- **Material System**: Materials reference virtual texture IDs rather than GPU texture handles
- **RHI Backend**: The indirection table and page cache physical texture are managed by the RHI layer
- **Terrain**: Large terrain textures are ideal candidates for virtual texturing. See [Terrain and Procedural Generation](Terrain-and-Procedural-Generation)

---

## See Also

- [Rendering and Graphics](Rendering-and-Graphics) — Texture streaming and material system
- [Asset Pipeline](Asset-Pipeline) — Asset import and texture processing
- [Terrain and Procedural Generation](Terrain-and-Procedural-Generation) — Terrain texture streaming
