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

## Setup Guide

### Initialization

Initialize the `VirtualTextureManager` singleton early in the rendering startup, after the RHI device is created:

```cpp
#include "Graphics/VirtualTexture.h"

using namespace Spark::Graphics;

// Get the singleton
auto& vtm = VirtualTextureManager::GetInstance();

// Initialize with custom settings:
//   pageCacheSize: maximum number of pages in the GPU cache
//   tileSize: pixel dimensions of each tile edge
bool ok = vtm.Initialize(2048, 128);
if (!ok)
{
    LOG_ERROR("Failed to initialize virtual texture manager");
}
```

### Shutdown

Call `Shutdown()` during engine teardown to release all GPU resources:

```cpp
vtm.Shutdown();
// After shutdown, m_initialized is false and the page cache is cleared
```

---

## Page Table Management

The page table is an indirection structure that maps virtual texture coordinates to physical cache locations. The `VirtualTextureManager` maintains two internal lists:

### Resident Pages (`m_pageCache`)

Pages currently loaded in GPU memory. Each page tracks:

```cpp
struct VirtualTexturePage
{
    uint32_t textureId;       // Which source texture this tile belongs to
    uint32_t mipLevel;        // Mip level (0 = highest resolution)
    uint32_t tileX, tileY;    // Tile coordinates within the mip level
    bool resident;            // Always true for pages in m_pageCache
    uint32_t lastUsedFrame;   // Updated every frame the page is referenced
};
```

### Pending Pages (`m_pendingLoads`)

Pages that have been requested but are not yet loaded. These are queued for async I/O:

```cpp
// Check if a page is already pending
VirtualTexturePage* pending = vtm.FindPendingPage(textureId, mipLevel, tileX, tileY);
// Returns nullptr if not in the pending queue
```

### Lookup Flow

When `ProcessFeedback()` encounters a feedback entry:

1. Search `m_pageCache` for a matching resident page (`FindResidentPage`)
2. If found, update `lastUsedFrame` to the current frame number
3. If not found, search `m_pendingLoads` (`FindPendingPage`)
4. If not pending, create a new `VirtualTexturePage` entry and add to `m_pendingLoads`

---

## Feedback Buffer Workflow

### GPU Side (Shader)

In the pixel shader, each virtual texture sample writes a `FeedbackEntry` to a UAV feedback buffer:

```hlsl
// Virtual texture sampling in HLSL
RWStructuredBuffer<FeedbackEntry> feedbackBuffer : register(u1);

float4 SampleVirtualTexture(float2 uv, uint textureId)
{
    // Calculate required mip level based on screen-space derivatives
    float2 ddxUV = ddx(uv);
    float2 ddyUV = ddy(uv);
    float maxDeriv = max(length(ddxUV), length(ddyUV));
    uint mipLevel = (uint)log2(max(1.0, maxDeriv * textureSize));

    // Write feedback entry
    uint idx;
    InterlockedAdd(feedbackCounter[0], 1, idx);
    feedbackBuffer[idx].textureId = textureId;
    feedbackBuffer[idx].requiredMip = mipLevel;

    // Sample from the indirection table -> physical cache
    return SampleFromPageCache(uv, textureId, mipLevel);
}
```

### CPU Side (Readback and Processing)

```cpp
// 1. Read back the feedback buffer from GPU
std::vector<FeedbackEntry> feedback = ReadbackFeedbackBuffer();

// 2. Process feedback: mark used pages, queue missing pages, evict stale
vtm.ProcessFeedback(feedback, currentFrameNumber);

// ProcessFeedback internally:
//   - For each entry, checks if the page is already resident
//   - If resident: updates lastUsedFrame
//   - If not resident and not pending: adds to m_pendingLoads
//   - If cache is full: evicts least-recently-used pages first
```

### Manual Page Requests

You can request specific pages outside the feedback loop (e.g., for preloading):

```cpp
// Preload mip 0 of texture 5 at tile (0,0)
bool queued = vtm.RequestPage(5, 0, 0, 0);
// Returns true if the page is already resident or was successfully queued
```

---

## Streaming Configuration

### Memory Budget Management

The page cache has a fixed maximum size. Choose settings based on your VRAM budget:

| Configuration | Pages | Tile Size | VRAM (RGBA8) | Use Case |
|---------------|-------|-----------|--------------|----------|
| Low | 512 | 128 | 32 MB | Mobile / low-spec |
| Medium | 1024 | 128 | 64 MB | Default |
| High | 2048 | 128 | 128 MB | Desktop with 4+ GB VRAM |
| Ultra | 4096 | 256 | 1 GB | High-end / terrain-heavy |

```cpp
// Low-VRAM configuration
vtm.Initialize(512, 128);

// High-VRAM configuration with larger tiles (fewer page faults)
vtm.Initialize(4096, 256);
```

### Tile Size Trade-offs

| Tile Size | Pros | Cons |
|-----------|------|------|
| 64 px | Fine-grained streaming, low waste | More page faults, higher management overhead |
| 128 px | Good balance (default) | Moderate waste at edges |
| 256 px | Fewer page management operations | Higher VRAM per page, coarser granularity |

---

## Quality Settings

### Eviction Tuning

The `framesToKeep` parameter controls how aggressively pages are evicted:

```cpp
// Aggressive eviction: pages expire after 30 frames (~0.5s at 60fps)
// Good for rapidly moving cameras (e.g., vehicle games)
uint32_t evicted = vtm.EvictStalePages(currentFrame, 30);

// Conservative eviction: pages stay for 120 frames (~2s at 60fps)
// Good for exploration games where players revisit areas
uint32_t evicted = vtm.EvictStalePages(currentFrame, 120);

// Default: 60 frames (~1 second at 60fps)
uint32_t evicted = vtm.EvictStalePages(currentFrame, 60);
```

### Mip Level Biasing

To reduce page faults at the cost of quality, bias the mip level upward:

```hlsl
// In the shader: add a bias to request a lower-resolution mip
uint mipLevel = (uint)log2(max(1.0, maxDeriv * textureSize));
mipLevel += mipBias; // mipBias = 1 halves page requests
```

---

## Console Commands

```
vt_status            # Display current page cache statistics
vt_resident          # Show count of resident pages
vt_pending           # Show count of pages waiting to load
vt_evict             # Force eviction of all stale pages
vt_cache_size <n>    # Set maximum resident page count
vt_tile_size <n>     # Set tile size (requires reinitialize)
```

The `Console_GetStatus()` method returns a formatted string:

```cpp
std::string status = vtm.Console_GetStatus();
// Example output:
// "VirtualTexture: 847/2048 resident, 12 pending, tile=128px"
```

---

## Performance Tuning

### Reducing Page Faults

Page faults (missing pages that must be loaded) cause visual pop-in. Strategies to minimize them:

1. **Increase cache size** -- More pages = fewer evictions = fewer re-loads
2. **Preload visible pages** -- Use `RequestPage()` during loading screens to warm the cache
3. **Increase `framesToKeep`** -- Keep pages resident longer to handle camera oscillation
4. **Bias mip levels** -- Request lower-resolution mips to reduce unique page count

### Monitoring Cache Efficiency

```cpp
// Per-frame monitoring
uint32_t resident = vtm.GetResidentPageCount();
uint32_t pending = vtm.GetPendingPageCount();
float utilization = static_cast<float>(resident) / maxPages;

// Log warnings for cache pressure
if (pending > resident / 4)
{
    LOG_WARN("VT: High page fault rate ({} pending vs {} resident)", pending, resident);
    LOG_WARN("VT: Consider increasing pageCacheSize or mip bias");
}

if (utilization > 0.95f)
{
    LOG_WARN("VT: Cache nearly full ({:.1f}% utilized), eviction rate may increase",
             utilization * 100.0f);
}
```

### Frame Budget Spreading

To avoid per-frame spikes, spread eviction across multiple frames:

```cpp
// Instead of evicting all stale pages at once:
if (frameNumber % 30 == 0)
{
    vtm.EvictStalePages(frameNumber, 60);
}

// Consider processing feedback in chunks for very large scenes:
constexpr size_t MAX_FEEDBACK_PER_FRAME = 4096;
if (feedback.size() > MAX_FEEDBACK_PER_FRAME)
{
    // Process the most recent entries first (likely most relevant)
    auto truncated = std::vector<FeedbackEntry>(
        feedback.end() - MAX_FEEDBACK_PER_FRAME, feedback.end());
    vtm.ProcessFeedback(truncated, frameNumber);
}
else
{
    vtm.ProcessFeedback(feedback, frameNumber);
}
```

---

## See Also

- [Rendering and Graphics](Rendering-and-Graphics) — Texture streaming and material system
- [Asset Pipeline](Asset-Pipeline) — Asset import and texture processing
- [Terrain and Procedural Generation](Terrain-and-Procedural-Generation) — Terrain texture streaming
