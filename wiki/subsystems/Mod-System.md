# Mod System

SparkEngine provides a mod loading and management system for user-created content. Mods can include scripts, assets, entity definitions, and configuration overrides, loaded at runtime with dependency resolution.

**Source:** `SparkEngine/Source/Engine/Modding/ModSystem.h`

## Overview

| Class | Responsibility |
|-------|---------------|
| `ModSystem` | Discovers, loads, manages, and persists mod configuration |
| `ModInfo` | Metadata for a single mod (name, author, version, dependencies) |

## Mod Directory Structure

```
MyMod/
  +-- mod.json          (metadata: name, version, author, dependencies)
  +-- Scripts/          (AngelScript files)
  +-- Assets/           (meshes, textures, sounds)
  +-- Data/             (JSON config overrides)
  +-- Preview.png       (mod preview image)
```

## Mod States

```cpp
enum class ModState {
    Available, // Found on disk, not loaded
    Loading,   // Currently being loaded
    Active,    // Loaded and active
    Error,     // Failed to load
    Disabled   // Explicitly disabled by user
};
```

## Quick Start

```cpp
ModSystem mods;

// Scan for available mods
size_t found = mods.ScanForMods("Data/Mods/");

// Enable and load mods
mods.EnableMod("weapon_pack_01");
mods.EnableMod("custom_maps");
mods.LoadEnabledMods();  // Loads in dependency order

// Check mod status
if (mods.IsModActive("weapon_pack_01")) {
    // Mod content is available
}
```

## Load Order and Dependencies

```cpp
// Set explicit load order
mods.SetLoadOrder({"base_mod", "weapon_pack_01", "custom_maps"});

// Check if dependencies are satisfied
if (mods.AreDependenciesMet("custom_maps")) {
    mods.LoadMod("custom_maps");
}
```

## Mod Callbacks

```cpp
mods.OnModLoaded([](const std::string& modId) {
    LOG("Mod loaded: " + modId);
});

mods.OnModUnloaded([](const std::string& modId) {
    LOG("Mod unloaded: " + modId);
});
```

## Persistence

Save and load the user's mod configuration (enabled/disabled state, load order):

```cpp
mods.SaveConfig("Data/Config/mods.json");
mods.LoadConfig("Data/Config/mods.json");
```

## mod.json Schema

The `mod.json` file is the manifest for every mod. It and the mod config are
capped at 64 KB and parsed strictly (`Json::ParseBounded`, depth 16, 4096 nodes):
a manifest with trailing junk or a truncated document is refused with a logged
reason instead of silently yielding a partial object. All fields are described below:

```json
{
    "id": "weapon_pack_01",
    "name": "Weapon Pack: Volume 1",
    "version": "1.2.0",
    "engineVersion": ">=0.9.0 <2.0.0",
    "author": "StudioName",
    "authorUrl": "https://example.com",
    "description": "Adds 12 new weapons with custom animations and sounds.",
    "category": "gameplay",
    "tags": ["weapons", "combat", "fps"],
    "previewImage": "Preview.png",
    "dependencies": [
        { "id": "base_content", "version": ">=1.0.0" }
    ],
    "conflicts": ["incompatible_mod_id"],
    "loadPriority": 100,
    "scripts": [
        "Scripts/WeaponInit.as",
        "Scripts/WeaponLogic.as"
    ],
    "assetOverrides": true,
    "dataOverrides": [
        "Data/weapons.json",
        "Data/balance.json"
    ],
    "permissions": [
        "filesystem:read",
        "network:none",
        "engine:entities",
        "engine:audio"
    ],
    "maxSizeMB": 500,
    "license": "MIT"
}
```

### Required Fields

| Field | Type | Description |
|-------|------|-------------|
| `id` | string | Unique identifier (lowercase, alphanumeric + underscores) |
| `name` | string | Display name shown in the mod manager UI |
| `version` | string | Semantic version (major.minor.patch) |
| `author` | string | Author or organization name |

### Optional Fields

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `engineVersion` | string | `"*"` | Semver range of compatible engine versions |
| `description` | string | `""` | Long description for the mod browser |
| `category` | string | `"misc"` | One of: `gameplay`, `cosmetic`, `map`, `total_conversion`, `tool`, `misc` |
| `tags` | string[] | `[]` | Searchable tags for the mod browser |
| `dependencies` | object[] | `[]` | Other mods that must be loaded first |
| `conflicts` | string[] | `[]` | Mod IDs that cannot be active simultaneously |
| `loadPriority` | int | `100` | Lower values load first; ties broken alphabetically |
| `permissions` | string[] | all | Sandbox permission whitelist (see below) |
| `maxSizeMB` | int | `1024` | Advisory size limit for distribution |
| `license` | string | `""` | License identifier (SPDX format recommended) |

## Mod Sandboxing and Security

Mods execute in a restricted sandbox to protect the player's system and the engine's integrity.

### Filesystem Sandbox

- Mods can only read files within their own directory and the shared `Data/` directory.
- Mods **cannot** write to any location outside `Data/Mods/<mod_id>/UserData/`.
- Attempts to access paths outside the sandbox throw a `ModSecurityException` and are logged.

### Virtual-path policy (`Spark::IsVirtualPathSafe`)

Every path a mod hands to the `VirtualFileSystem` must be mount-relative. A path is
rejected when it contains `:` (drive-relative or NTFS alternate data stream), names
a reserved Windows device (`CON`, `NUL`, `COM1`, ...), or escapes the mount after
normalization; containment is decided on the normalized path and then re-checked
against the canonical path so a symlink or junction planted inside the mount cannot
redirect a read outside it. Two behavior changes for mod authors: a name that merely
*contains* `..` is legal (`weapon..old.mesh` loads), and a zero-byte override file
now wins the mount-priority contest instead of falling through to the
lower-priority original.

### Script Sandbox

AngelScript mod scripts run with a restricted API surface:

- **Allowed:** Entity creation/destruction, component access, audio playback, UI widget creation, event subscription, math/utility functions.
- **Blocked:** Direct filesystem I/O, network sockets, OS process spawning, raw memory access, engine internals (renderer, physics solver internals).
- **Execution limits:** Scripts are subject to an instruction count limit per frame (default 1,000,000 instructions). Exceeding the limit pauses the script and logs a warning.

```cpp
ModSandbox sandbox;
sandbox.SetInstructionLimit(1'000'000);
sandbox.SetMemoryLimit(64 * 1024 * 1024);  // 64 MB per mod
sandbox.AllowPermission(ModPermission::Entities);
sandbox.AllowPermission(ModPermission::Audio);
sandbox.DenyPermission(ModPermission::Network);
```

### Permission Strings

| Permission | Description |
|-----------|-------------|
| `filesystem:read` | Read files in the mod directory and shared data |
| `filesystem:write` | Write to the mod's `UserData/` directory |
| `network:none` | No network access (default) |
| `network:http` | HTTP GET/POST to whitelisted domains |
| `engine:entities` | Create, destroy, and modify entities and components |
| `engine:audio` | Play sounds and music |
| `engine:ui` | Create custom UI panels and widgets |
| `engine:input` | Register custom input bindings |

## Mod Compatibility Versioning

Mods use semantic versioning (semver) for both their own version and engine compatibility ranges:

- **Major version** bump: Breaking changes to the mod's public API or data formats.
- **Minor version** bump: New features, backward-compatible additions.
- **Patch version** bump: Bug fixes only.

The `engineVersion` field in `mod.json` specifies which engine versions the mod supports using npm-style range syntax:

| Range | Meaning |
|-------|---------|
| `">=1.0.0 <2.0.0"` | Any 1.x release |
| `"~1.2.0"` | >= 1.2.0 and < 1.3.0 |
| `"^1.2.0"` | >= 1.2.0 and < 2.0.0 |
| `"*"` | Any engine version (not recommended) |

If the running engine version is outside the specified range, the mod is flagged with a compatibility warning but can still be force-loaded by the user.

## Mod Conflicts and Resolution

When two mods declare a conflict (via the `conflicts` array) or both override the same asset/data file, the mod system applies conflict resolution:

1. **Explicit conflicts:** If mod A lists mod B in its `conflicts` array, enabling both simultaneously shows a warning in the mod manager. The user must choose which to keep active.
2. **Asset conflicts:** When two mods override the same base asset, the mod with the **higher `loadPriority`** (lower numerical value) wins. The losing override is silently discarded.
3. **Data merge conflicts:** For JSON data overrides, the engine attempts a **deep merge** by default. Array fields are concatenated; object fields are merged recursively. If a key appears in both, the higher-priority mod's value wins.
4. **Script conflicts:** If two mods register the same script hook or entity type name, the second mod's registration fails and logs an error.

## Steam Workshop Integration Pattern

SparkEngine provides a Workshop integration layer for publishing and subscribing to mods:

```cpp
// Upload a mod to Steam Workshop
WorkshopUploader uploader;
uploader.SetModPath("Data/Mods/weapon_pack_01/");
uploader.SetTitle("Weapon Pack: Volume 1");
uploader.SetDescription("12 new weapons...");
uploader.SetPreviewImage("Data/Mods/weapon_pack_01/Preview.png");
uploader.SetTags({"weapons", "combat"});
uploader.SetVisibility(WorkshopVisibility::Public);
uploader.Upload([](WorkshopResult result) {
    if (result.success) LOG("Published: " + result.workshopId);
});

// Download subscribed mods at startup
WorkshopManager workshop;
workshop.SyncSubscribedMods("Data/Mods/");  // Downloads/updates subscribed mods
```

The Workshop manager synchronizes subscribed items on startup and can optionally check for updates periodically during gameplay.

## Mod Development Workflow

### 1. Create a New Mod

```bash
# Use the mod scaffolding tool
SparkModTool create --id my_mod --name "My First Mod" --author "PlayerName"
# Creates Data/Mods/my_mod/ with mod.json, empty Scripts/, Assets/, Data/ directories
```

### 2. Develop and Iterate

- Place AngelScript files in `Scripts/`, assets in `Assets/`, data overrides in `Data/`.
- Run the game with `--mod-dev my_mod` to enable hot-reload for the mod under development.
- Use the in-editor **Mod Inspector** panel to view loaded state, errors, and performance stats.

### 3. Test

```bash
# Validate mod structure and metadata
SparkModTool validate --path Data/Mods/my_mod/

# Run the automated mod test suite
SparkModTool test --path Data/Mods/my_mod/ --sandbox strict
```

### 4. Package and Publish

```bash
# Package for distribution (creates .sparkmod archive)
SparkModTool package --path Data/Mods/my_mod/ --output Releases/my_mod_1.0.0.sparkmod

# Publish to Steam Workshop
SparkModTool publish --path Data/Mods/my_mod/
```

## Asset Override Priority

When a mod provides an asset with the same relative path as a base game asset, the mod's version takes precedence. Priority is determined by load order:

```
Base Game Assets (lowest priority)
  ↑  Mod A assets (loadPriority 100)
  ↑  Mod B assets (loadPriority 50)    ← wins over Mod A if both override same asset
  ↑  Mod C assets (loadPriority 10)    ← highest priority
```

Lower `loadPriority` values indicate higher priority. The asset pipeline resolves overrides at load time by searching mod directories in priority order before falling back to the base game.

### Override Example

If the base game has `Assets/Weapons/pistol.fbx` and a mod at `Data/Mods/weapon_pack_01/Assets/Weapons/pistol.fbx`, the mod's pistol mesh is used whenever `Weapons/pistol.fbx` is requested through the asset system.

## Hot-Reload of Mod Content

When running with `--mod-dev <mod_id>`, the engine watches the mod directory for file changes:

| File Type | Hot-Reload Behavior |
|-----------|-------------------|
| `.as` (AngelScript) | Script is recompiled and re-executed; existing entity instances are patched |
| `.json` (Data) | Data is re-parsed and merged; affected systems are notified via events |
| `.fbx`, `.obj` (Mesh) | Mesh is reimported; existing instances update on next frame |
| `.png`, `.jpg`, `.dds` (Texture) | Texture is reloaded into GPU memory; material references update automatically |
| `.wav`, `.ogg` (Audio) | Audio buffer is replaced; currently playing sounds finish before switching |
| `mod.json` | Metadata is refreshed; dependency graph is re-evaluated |

Hot-reload latency is typically under 500ms for scripts and textures, and 1--3 seconds for mesh assets depending on complexity.

## Mod Performance Profiling

The mod system includes built-in profiling to identify poorly performing mods:

```cpp
const ModProfile& profile = mods.GetProfile("weapon_pack_01");
float scriptMs = profile.GetAverageScriptTime();    // ms per frame
float memoryMB = profile.GetMemoryUsageMB();        // Current allocation
size_t entityCount = profile.GetEntityCount();       // Entities owned by mod
size_t assetCount = profile.GetLoadedAssetCount();   // Assets currently loaded
```

### Console Profiling Commands

```
mod_profile <mod_id>     # Show per-frame timing for a specific mod
mod_profile_all          # Show aggregate timing for all active mods
mod_memory               # Show memory usage per mod
mod_budget               # Show resource budgets and current utilization
```

### Performance Budget

Each mod is allocated a default budget to prevent any single mod from degrading the game:

| Resource | Default Budget |
|----------|---------------|
| Script CPU time | 2 ms per frame |
| Memory | 64 MB |
| Entities | 500 |
| Draw calls (additional) | 50 |
| Audio sources | 8 |

Exceeding a budget triggers a warning in the console and mod inspector. Persistent overages (5+ consecutive seconds) can optionally auto-disable the mod if `mod_enforce_budgets 1` is set.

## Mod Size Limits and Best Practices

### Size Guidelines

| Distribution Channel | Recommended Max | Hard Limit |
|---------------------|----------------|-----------|
| Steam Workshop | 500 MB | 2 GB |
| Manual download | 200 MB | No limit |
| `.sparkmod` archive | 500 MB | 4 GB |

### Best Practices

- **Compress textures** using BCn/DXT formats rather than shipping uncompressed PNG/TGA.
- **Share base assets** by declaring dependencies on content packs rather than duplicating assets.
- **Use LODs** for custom meshes; provide at least 2 LOD levels for models over 5,000 triangles.
- **Minimize script complexity** -- avoid per-entity per-frame scripts when system-level logic suffices.
- **Test with other popular mods** enabled to catch conflicts early.
- **Version your mod** with meaningful semver bumps so dependent mods can specify compatible ranges.
- **Provide a clear README** inside the mod directory explaining installation, features, and known issues.
- **Use the sandbox validator** (`SparkModTool validate`) before every release to catch permission violations and structural errors.

## Console Commands

```
mod_status    # Show mod system status
mod_list      # List all discovered mods with state
mod_enable <mod_id>    # Enable a mod
mod_disable <mod_id>   # Disable a mod
mod_reload <mod_id>    # Unload and reload a specific mod
mod_reload_all         # Reload all active mods
mod_info <mod_id>      # Show detailed mod metadata
mod_conflicts          # List all detected mod conflicts
mod_validate <mod_id>  # Run validation checks on a mod
```

---

## Virtual Filesystem & SparkPak Archives

The mod system integrates with the VirtualFileSystem (`VirtualFileSystem.h`) which provides priority-based mount layering. Assets from mods override game assets, which override engine assets.

SparkPak (.spk) archives can be mounted as VFS providers via `ArchiveResourceProvider`, allowing mods and DLC to be distributed as single archive files instead of loose directories. See [Asset Format Specifications](../specifications/Asset-Format-Specifications.md#sparkpak-archive-format-spk) for the archive format details.

**Source:** `SparkEngine/Source/Engine/Modding/VirtualFileSystem.h`, `SparkEngine/Source/Engine/Modding/ArchiveResourceProvider.h`

## See Also

- [Scripting with AngelScript](Scripting-with-AngelScript.md) — Mod scripts
- [Asset Pipeline](../gameplay-tools/Asset-Pipeline.md) — Loading mod assets
- [Asset Format Specifications](../specifications/Asset-Format-Specifications.md) — SparkPak archive format
- [Save System](../gameplay-tools/Save-System.md) — Persisting mod configuration
