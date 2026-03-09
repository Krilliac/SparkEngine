# Editor & Development Tools

Context: `#prompt:copilot-instructions` for project overview.

## SparkEditor

`SparkEditor/` — Professional game editor built with Dear ImGui (docking branch). Compiles as a separate executable that communicates with SparkEngine via named pipes.

### Editor Subsystems (`SparkEditor/Source/`)

| Subsystem | Purpose |
|-----------|---------|
| `Animation/` | Animation timeline, state machine editor |
| `AssetBrowser/` | File browser with thumbnails, drag-and-drop |
| `BuildSystem/` | Build configuration, one-click compilation |
| `Console/` | Embedded console panel |
| `Gizmos/` | Transform gizmos via ImGuizmo (translate/rotate/scale) |
| `Inspector/` | Component property inspector |
| `LevelStreaming/` | Level chunk management |
| `MaterialEditor/` | PBR material authoring |
| `NodeGraph/` | Node-based visual scripting (imnodes) |
| `Profiler/` | Performance graphs and frame timeline |
| `SceneHierarchy/` | Entity tree view |
| `Terrain/` | Terrain sculpting and painting tools |
| `VersionControl/` | Git integration |
| `Viewport/` | 3D game viewport with camera controls |
| `WeaponEditor/` | Weapon parameter tuning |

### ImGui Conventions

- **Docking**: All panels are dockable and rearrangeable
- **DPI awareness**: Scale UI elements with `ImGui::GetIO().FontGlobalScale`
- **Unique IDs**: Use `##` suffix or `PushID`/`PopID` for duplicate labels
- **Layout persistence**: Save/load via `editor_layout_save` / `editor_layout_load`

### Adding an Editor Panel

```cpp
// In your panel's Update/Draw method:
if (ImGui::Begin("My Panel")) {
    // Panel contents
    ImGui::Text("Value: %.2f", myValue);
    if (ImGui::Button("Reset")) { myValue = 0.0f; }
}
ImGui::End();
```

### Console Commands

| Command | Description |
|---------|-------------|
| `editor_theme <name>` | Switch editor color theme |
| `panel_toggle <name>` | Show/hide panel |
| `editor_layout_save <name>` | Save current layout |
| `editor_layout_load <name>` | Load saved layout |
| `editor_reset_layout` | Reset to default layout |
| `asset_refresh` | Rescan asset directory |

---

## Asset Pipeline (`Tools-Development`)

### Loading Pattern

```
AssetDatabase tracks all assets → AsyncLoader queues load requests →
Format-specific loaders (OBJ, FBX, PNG, WAV) process files →
Assets registered in runtime caches with LOD management
```

- **Formats**: OBJ, FBX, glTF (models via Assimp), PNG, JPG, DDS (textures), WAV, MP3 (audio)
- **Streaming**: Distance-based LOD, async loading with progress callbacks
- **Hot-reload**: File watcher detects changes, reloads assets at runtime

Console: `assets_refresh`, `assets_load <path>`, `assets_memory_usage`, `assets_hot_reload`, `assets_cache_clear`

---

## Debugging & Profiling Tools

### CrashHandler (`Utils/CrashHandler`)

- Automatic minidump (`.dmp`) generation on crash
- Stack traces for all threads
- Screenshot capture at crash time
- System info collection (OS version, GPU, RAM)

### Assertion Macros (`Utils/Assert.h`)

```cpp
ASSERT(condition);                          // Debug-only, stripped in Release
ASSERT_MSG(condition, "details: %d", val);  // With formatted message
ASSERT_ALWAYS_MSG(condition, "msg");        // Fires in all builds
```

### Profiler

- Frame time graph, per-system timing
- GPU timing queries
- Memory allocation tracking
- Console: `profile_start`, `profile_stop`, `memory_snapshot`, `debug_overlay`

### Debug Draw Overlay

- Real-time visualization of physics shapes, AI navigation, audio sources
- Wireframe bounding boxes, rays, nav mesh triangles
- Console: `debug_draw_physics`, `debug_draw_ai`, `debug_draw_audio`

### SparkConsole (External App)

`SparkConsole/` — Standalone console application. Connects to SparkEngine via named pipes (`ConsoleProcessManager`). Allows debugging without in-engine overlay. Useful for fullscreen gameplay testing.
