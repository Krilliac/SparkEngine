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

## Console Commands

```
mod_status    # Show mod system status
mod_list      # List all discovered mods with state
```

---

## See Also

- [Scripting with AngelScript](Scripting-with-AngelScript) — Mod scripts
- [Asset Pipeline](Asset-Pipeline) — Loading mod assets
- [Save System](Save-System) — Persisting mod configuration
