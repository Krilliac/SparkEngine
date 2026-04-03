# Performance Tips

Practical optimization guide for SparkEngine. Covers rendering, physics, audio, networking, and general best practices for shipping smooth games.

---

## Measuring Performance

Before optimizing, measure. SparkEngine provides several profiling tools:

### In-Engine Profiling

| Tool | How to access | What it shows |
|------|---------------|---------------|
| FPS counter | Press **F3** or `ShowFPS = true` in settings | Frame rate and frame time |
| Profiler panel | Editor → Window → Profiler | Per-system frame timing |
| Scene Statistics panel | Editor → Window → Scene Statistics | Entity/component counts, draw calls, triangles |
| Console commands | See below | On-demand metrics |

### Useful Console Commands

```
render_stats          # Draw calls, triangles, batches
physics_metrics       # Physics step time, body count, contacts
audio_metrics         # Active sources, mix time
metrics               # All-in-one system overview
game_stats            # Game module performance
```

### GPU Timing

Enable GPU timing queries to identify GPU bottlenecks:

```ini
[Rendering]
EnableGPUTiming = true
```

---

## Rendering Optimization

### Use Quality Presets

Quality presets adjust multiple settings at once:

```
render_quality low        # Mobile/low-end
render_quality medium     # Integrated graphics
render_quality high       # Discrete GPU (default)
render_quality ultra      # High-end GPU
```

### Draw Call Budget

Keep draw calls under control. The default budget is 1000 per frame:

```ini
[Rendering]
MaxDrawCalls = 1000
```

**Reduce draw calls by:**
- Using material atlases (fewer unique materials = fewer draw calls)
- Enabling frustum culling (on by default)
- Enabling occlusion culling for dense scenes: `OcclusionCulling = true`
- Using LOD (Level of Detail) for distant objects
- Batching small objects into single meshes

### Shadow Optimization

Shadows are often the most expensive effect. Tune them:

```ini
[Graphics]
ShadowQuality = 1             # 0=Off, 1=Low, 2=Medium, 3=High

[Rendering]
ShadowMapSize = 1024          # Lower = faster (default 2048)
CascadeCount = 2              # Fewer cascades = faster (default 3)
```

Or disable shadows entirely for a large performance win:

```
shadows off
```

### Post-Processing Budget

Each post-processing effect adds a full-screen pass. Disable effects you don't need:

```ini
[PostProcess]
BloomEnabled = true            # Keep — relatively cheap
[SSAO]
Enabled = false                # Expensive — disable if needed
[SSR]
Enabled = false                # Expensive — disable if needed
[Volumetric]
Enabled = false                # Expensive — disable if needed
[MotionBlur]
Enabled = false                # Moderate cost
```

**Cost ranking** (approximate, GPU-dependent):

| Effect | Cost | Notes |
|--------|------|-------|
| FXAA | Very low | Cheap AA |
| Bloom | Low | 6 blur passes |
| TAA | Low–Medium | Good quality/perf ratio |
| MSAA 4x | Medium | Memory + bandwidth |
| Motion Blur | Medium | Depends on sample count |
| SSAO | Medium–High | 16 samples per pixel default |
| SSR | High | Ray-marching per pixel |
| Volumetric Fog | High | 3D ray-marching |

### Render Scale

Scale internal resolution for a quick FPS boost without changing window size:

```ini
[Graphics]
RenderScale = 0.75            # 75% internal resolution
```

### Dynamic Quality Scaling

Let the engine auto-adjust quality to maintain a target framerate:

```ini
[DynamicQuality]
Enabled = true
TargetFrameTimeMs = 16.67     # 60 FPS target
MinRenderScale = 0.5          # Won't go below 50% resolution
```

The scaler adjusts render scale, shadow resolution, LOD bias, and texture mip bias automatically using a PID controller.

### Texture Memory

Control texture memory usage:

```
tex_quality medium            # Reduce texture resolution
tex_memory 512                # Set 512 MB texture budget
```

```ini
[Rendering]
MaxTextureSize = 1024         # Cap texture dimensions
AnisotropyLevel = 4           # Lower anisotropy (default 16)
```

### Render Path Selection

Choose the right rendering path for your scene:

| Path | Best for | Setting |
|------|----------|---------|
| Forward | Simple scenes, few lights | `RenderPath = 0` |
| Deferred | Many lights, complex materials | `RenderPath = 1` (default) |
| Forward+ | Many lights, transparent objects | `RenderPath = 2` |
| Clustered | Very many lights (100+) | `RenderPath = 3` |

---

## Physics Optimization

### Timestep Tuning

The default physics timestep is 60 Hz (16.67ms). For simpler games, 30 Hz saves CPU:

```ini
[Physics]
FixedTimestep = 0.03333       # 30 Hz physics
MaxSubSteps = 2               # Fewer sub-steps
```

### Body Count

Monitor your physics body count:

```
physics_metrics               # Shows active/sleeping body counts
physics_list                  # List all bodies
```

**Tips:**
- Remove far-away dynamic bodies or put them to sleep
- Use static bodies for immovable geometry (cheaper than kinematic)
- Use simple collision shapes (box, sphere, capsule) instead of mesh colliders where possible
- Jolt supports multithreaded physics — this is on by default via the Job System

### Debug Draw Performance

Physics debug draw is expensive. Only enable it during development:

```
physics_debug off             # Always disable for profiling
```

---

## Audio Optimization

### Source Limits

Limit concurrent audio sources:

```ini
[AudioExtended]
MaxSources = 16               # Default 32; reduce for low-end
```

### 3D Audio

If your game doesn't need spatial audio, disable it:

```ini
[AudioExtended]
Enable3D = false              # Skip 3D spatialization
```

### Reverb and DSP

DSP effects add CPU cost:

```ini
[AudioExtended]
EnableReverb = false          # Disable reverb processing
EnableEAX = false             # Disable EAX effects
```

---

## AI Optimization

### Reduce AI Complexity

Tune AI settings for your game's needs:

```ini
[AI]
DetectionRange = 20.0         # Shorter detection = fewer checks
ReactionTime = 0.5            # Slower reactions = fewer updates
CoverSearchRadius = 10.0      # Smaller search radius
```

### AI Update Frequency

Not all AI agents need to think every frame. The AI system uses behavior trees — complex trees with many conditions are more expensive. Keep trees shallow and use simple condition checks.

---

## Networking Optimization

### Replication Rate

Lower the replication rate for games that don't need fast updates:

```ini
[Network]
ReplicationRate = 10.0        # 10 Hz instead of default 20 Hz
```

### Packet Compression

Enable compression for bandwidth-limited scenarios:

```ini
[Network]
EnableCompression = true
```

### Buffer Sizes

Adjust send/receive buffers based on your game's needs:

```ini
[Network]
SendBufferSize = 32768        # Default 65536
ReceiveBufferSize = 32768
```

---

## Scripting Optimization

### Script Limits

Prevent runaway scripts from stalling the engine:

```ini
[Scripting]
ExecutionTimeoutMs = 50.0     # Kill scripts after 50ms (default 100)
MaxCallStackDepth = 32        # Limit recursion (default 64)
MaxScriptMemoryMB = 32        # Limit script memory (default 64)
```

### Hot-Reload in Production

Disable hot-reload in shipping builds to save file-watch overhead:

```ini
[Scripting]
HotReloadEnabled = false
```

---

## Animation Optimization

### Compression

Enable animation compression to reduce memory:

```ini
[Animation]
CompressionQuality = 3        # 0=None, 1=Low, 2=Medium, 3=High
```

### LOD-Based Animation

Reduce animation quality at distance:

```ini
[Animation]
LodDistanceMultiplier = 0.5   # More aggressive LOD transitions
```

### Montage Limits

Cap concurrent animation montages:

```ini
[Animation]
MaxActiveMontages = 2         # Default 4
```

---

## Memory Tips

### Texture Budgets

Textures are typically the largest memory consumer. Guidelines:

| Texture Type | Recommended Size | Notes |
|-------------|-----------------|-------|
| Character diffuse | 2048×2048 | Main characters |
| Environment | 1024×1024 | Tiling textures |
| Props | 512×512 | Small objects |
| UI | 256×256 or atlas | UI elements |
| Normal maps | Same as diffuse | BC5 compressed |

### Asset Streaming

For large worlds, the streaming system loads/unloads areas automatically. Monitor it via the **Streaming** editor panel or console.

---

## Build Configuration Tips

### Release Builds

Always profile with Release builds. Debug builds are 5–10x slower:

```bash
cmake --build build --config Release
```

### Disable Unused Features

Disable subsystems you don't use to reduce overhead:

```bash
cmake -B build \
  -DENABLE_AI=OFF \              # If your game has no AI
  -DENABLE_NETWORKING=OFF \      # If single-player only
  -DENABLE_PROFILING=OFF         # For shipping builds
```

### Headless/Dedicated Server

For dedicated servers, disable all rendering:

```bash
./SparkEngine -headless -game MyGame.dll
```

This skips graphics initialization entirely and uses NullRHIDevice.

---

## Quick Optimization Checklist

1. **Measure first** — Use `render_stats`, `physics_metrics`, `metrics`
2. **Use quality presets** — `render_quality medium` for quick wins
3. **Check draw calls** — Keep under 1000; enable frustum/occlusion culling
4. **Tune shadows** — Often the single biggest performance lever
5. **Disable unused post-processing** — SSAO, SSR, volumetrics
6. **Use dynamic quality scaling** — Let the engine adapt automatically
7. **Monitor physics body count** — Use simple shapes, remove distant bodies
8. **Limit audio sources** — 16–32 concurrent sources is plenty
9. **Profile with Release builds** — Debug builds are not representative
10. **Test on target hardware** — Use `-test-frames 1000` for benchmarking

---

## Benchmarking

Run a reproducible benchmark:

```bash
# Run 1000 frames and exit
./SparkEngine -test-frames 1000 -window-size 1920x1080 -game MyGame.dll
```

Check frame timing in the output log. Compare across changes to catch regressions.

---

## See Also

- [Configuration Reference](Configuration-Reference) — All settings and commands
- [Profiler and Debugging](Profiler-and-Debugging) — Detailed profiling guide
- [Performance Profiling Guide](Performance-Profiling-Guide) — Frame profiling workflows
- [Rendering and Graphics](Rendering-and-Graphics) — Graphics pipeline details
- [Dynamic Quality Scaler](Rendering-and-Graphics#dynamic-quality) — Auto quality adjustment
- [Troubleshooting](Troubleshooting) — Performance-related issues
