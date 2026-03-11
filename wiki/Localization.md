# Localization

SparkEngine provides an internationalization system for managing translated text. The `LocalizationSystem` loads string tables from JSON files and supports runtime language switching without restart.

**Source:** `SparkEngine/Source/Engine/Localization/LocalizationSystem.h`

## Overview

| Class | Responsibility |
|-------|---------------|
| `LocalizationSystem` | Singleton manager: loads languages, resolves keys, fires change events |
| `StringTable` | Key-value store of localized strings for one language |

## Quick Start

```cpp
auto& loc = LocalizationSystem::Get();
loc.LoadLanguage("en", "Data/Localization/en.json");
loc.LoadLanguage("fr", "Data/Localization/fr.json");
loc.SetCurrentLanguage("en");

std::string text = loc.GetString("menu.play");         // "Play"
std::string fmt  = loc.Format("hud.ammo", 30, 120);    // "Ammo: 30/120"
```

## JSON File Format

Translation files use flat key-value JSON with `{0}`, `{1}` placeholders for formatting:

```json
{
    "menu.play": "Play",
    "menu.settings": "Settings",
    "hud.ammo": "Ammo: {0}/{1}",
    "hud.health": "Health: {0}"
}
```

## Language Switching

```cpp
loc.SetCurrentLanguage("fr");

// Register a callback to update UI when language changes
loc.OnLanguageChanged([](const std::string& langCode) {
    RefreshAllUIText();
});
```

## Fallback Language

When a key is missing in the current language, the system falls back to a configurable fallback (default: `"en"`):

```cpp
loc.SetFallbackLanguage("en");
```

## API Reference

| Method | Description |
|--------|-------------|
| `LoadLanguage(code, path)` | Load a language from a JSON file |
| `SetCurrentLanguage(code)` | Switch the active language |
| `GetCurrentLanguage()` | Get the current language code |
| `GetAvailableLanguages()` | List all loaded language codes |
| `GetString(key)` | Look up a localized string |
| `Format(key, args...)` | Format a localized template with positional arguments |
| `SetFallbackLanguage(code)` | Set the fallback language for missing keys |
| `OnLanguageChanged(callback)` | Register a language change callback |

## Thread Safety

`LocalizationSystem` is thread-safe for concurrent reads after initial loading (protected by mutex).

## Console Commands

```
loc_status       # Show localization system status
loc_keys         # List all keys for the current language
```

---

## See Also

- [Dialogue System](Dialogue-System) — Dialogue nodes reference localization keys
- [UI System](UI-System) — Displaying localized text in game UI
- [Scripting with AngelScript](Scripting-with-AngelScript) — Accessing localized strings from scripts
