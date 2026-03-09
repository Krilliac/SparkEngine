# Editor & Development Tools

Context: `#prompt:copilot-instructions` for project overview, coding standards, and assertion macros.

## SparkEditor

`SparkEditor/` — ImGui (docking branch) editor, separate executable.

### Subsystems (`SparkEditor/Source/`)

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

- **Docking**: All panels dockable and rearrangeable
- **DPI**: Scale with `ImGui::GetIO().FontGlobalScale`
- **Unique IDs**: `##` suffix or `PushID`/`PopID` for duplicate labels
- **Layout persistence**: `editor_layout_save` / `editor_layout_load` commands

### Adding an Editor Panel

```cpp
if (ImGui::Begin("My Panel")) {
    ImGui::Text("Value: %.2f", myValue);
    if (ImGui::Button("Reset")) { myValue = 0.0f; }
}
ImGui::End();
```

### Console Commands

| Command | Description |
|---------|-------------|
| `editor_theme <name>` | Switch color theme |
| `panel_toggle <name>` | Show/hide panel |
| `editor_layout_save <name>` | Save layout |
| `editor_layout_load <name>` | Load layout |
| `editor_reset_layout` | Reset to default |
| `asset_refresh` | Rescan asset directory |

---

## Asset Pipeline

`AssetDatabase` → `AsyncLoader` → format-specific loaders → runtime caches with LOD.

- **Formats**: OBJ, FBX, glTF (Assimp), PNG, JPG, DDS, WAV, MP3
- **Streaming**: Distance-based LOD, async with progress callbacks
- **Hot-reload**: File watcher detects changes, reloads at runtime

Console: `assets_refresh`, `assets_load <path>`, `assets_memory_usage`, `assets_hot_reload`, `assets_cache_clear`

---

## Debugging & Profiling

### CrashHandler (`Utils/CrashHandler`)

Generates minidumps (`.dmp`), stack traces, screenshots, and system info on crash.

### Profiler

Frame timing, GPU queries, memory tracking. Console: `profile_start`, `profile_stop`, `memory_snapshot`, `debug_overlay`

### Debug Draw Overlay

Visualizes physics shapes, AI nav, audio sources. Console: `debug_draw_physics`, `debug_draw_ai`, `debug_draw_audio`
