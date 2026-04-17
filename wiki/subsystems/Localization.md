# Localization

SparkEngine provides an internationalization (i18n) system for managing translated text across all in-game UI, HUD, menus, dialogue, and scripting. The `LocalizationSystem` loads string tables from JSON files and supports runtime language switching without requiring a game restart.

**Source:** `SparkEngine/Source/Engine/Localization/LocalizationSystem.h`

**Namespace:** `Spark`

---

## Architecture Overview

```
+-------------------------------------------------------+
|                  LocalizationSystem                    |
|  (Singleton -- thread-safe reads after loading)        |
|                                                        |
|  m_languages: map<string, StringTable>                 |
|  m_currentLanguage: string   (default: "en")           |
|  m_fallbackLanguage: string  (default: "en")           |
|  m_languageChangedCallbacks: vector<function>          |
|  m_mutex: std::mutex                                   |
+---------------------------+---------------------------+
                            |
                 +----------+----------+
                 |      StringTable     |
                 |  (per-language store) |
                 |                      |
                 | m_entries: map<k,v>  |
                 | m_missingPlaceholder |
                 +----------------------+
```

| Class | Responsibility |
|-------|---------------|
| `LocalizationSystem` | Singleton manager: loads languages, resolves keys, fires change events, provides formatted text substitution |
| `StringTable` | Key-value store of localized strings for a single language; supports loading from JSON, adding entries at runtime, and querying all keys |

---

## Quick Start

```cpp
#include "Engine/Localization/LocalizationSystem.h"

// 1. Get the singleton
auto& loc = Spark::LocalizationSystem::Get();

// 2. Load language files
loc.LoadLanguage("en", "Data/Localization/en.json");
loc.LoadLanguage("fr", "Data/Localization/fr.json");
loc.LoadLanguage("de", "Data/Localization/de.json");
loc.LoadLanguage("ja", "Data/Localization/ja.json");

// 3. Set the active language
loc.SetCurrentLanguage("en");

// 4. Look up strings
std::string text = loc.GetString("menu.play");         // "Play"
std::string fmt  = loc.Format("hud.ammo", 30, 120);    // "Ammo: 30/120"
```

---

## JSON File Format

Translation files use flat key-value JSON. Keys follow a dot-separated namespace convention (e.g. `category.subcategory.item`). Placeholders use `{0}`, `{1}`, etc. for positional argument substitution.

### Example: `Data/Localization/en.json`

```json
{
    "menu.play": "Play",
    "menu.settings": "Settings",
    "menu.quit": "Quit",
    "menu.resume": "Resume",
    "menu.new_game": "New Game",
    "menu.load_game": "Load Game",

    "hud.ammo": "Ammo: {0}/{1}",
    "hud.health": "Health: {0}",
    "hud.score": "Score: {0}",
    "hud.lives": "Lives: {0}",

    "dialog.npc_greeting": "Hello, traveler!",
    "dialog.npc_farewell": "Safe travels!",

    "system.loading": "Loading...",
    "system.saving": "Saving...",
    "system.error_generic": "An error occurred: {0}"
}
```

### Example: `Data/Localization/fr.json`

```json
{
    "menu.play": "Jouer",
    "menu.settings": "Parametres",
    "menu.quit": "Quitter",

    "hud.ammo": "Munitions: {0}/{1}",
    "hud.health": "Sante: {0}",

    "dialog.npc_greeting": "Bonjour, voyageur !",
    "dialog.npc_farewell": "Bon voyage !"
}
```

### Key Naming Conventions

| Pattern | Purpose | Example |
|---------|---------|---------|
| `menu.*` | Main menu and pause menu strings | `menu.play`, `menu.settings` |
| `hud.*` | In-game HUD elements | `hud.ammo`, `hud.health` |
| `dialog.*` | Dialogue system text | `dialog.npc_greeting` |
| `system.*` | System messages (loading, errors) | `system.loading` |
| `item.*` | Item names and descriptions | `item.sword_name` |
| `ui.*` | General UI labels | `ui.confirm`, `ui.cancel` |
| `tutorial.*` | Tutorial and help strings | `tutorial.move` |

---

## StringTable API Reference

`StringTable` is the per-language storage class. Each loaded language gets its own `StringTable` instance managed internally by `LocalizationSystem`.

| Method | Signature | Description |
|--------|-----------|-------------|
| `LoadFromFile` | `bool LoadFromFile(const std::string& filePath)` | Parse a JSON file and populate the string table. Returns `true` on success. |
| `SetEntry` | `void SetEntry(const std::string& key, const std::string& value)` | Add or overwrite a single key-value pair at runtime. |
| `GetEntry` | `const std::string& GetEntry(const std::string& key) const` | Look up a string by key. Returns the key itself if not found. |
| `HasEntry` | `bool HasEntry(const std::string& key) const` | Check whether a key exists in this table. |
| `GetEntryCount` | `size_t GetEntryCount() const` | Return the total number of entries in the table. |
| `GetAllKeys` | `std::vector<std::string> GetAllKeys() const` | Return a vector of all keys in this table. Useful for coverage audits. |

### Internal Storage

```cpp
// StringTable members
std::unordered_map<std::string, std::string> m_entries;
std::string m_missingPlaceholder;   // returned when key not found
```

---

## LocalizationSystem API Reference

`LocalizationSystem` is a singleton accessed via `LocalizationSystem::Get()`. It manages all loaded languages and provides the primary string lookup and formatting API.

### Core Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `Get` | `static LocalizationSystem& Get()` | Return the singleton instance (created on first call). |
| `LoadLanguage` | `bool LoadLanguage(const std::string& languageCode, const std::string& filePath)` | Load a language from a JSON file. The `languageCode` should be an ISO 639-1 code (e.g. `"en"`, `"fr"`, `"de"`, `"ja"`). Returns `true` on success. |
| `SetCurrentLanguage` | `bool SetCurrentLanguage(const std::string& languageCode)` | Switch the active language. Fires all registered `OnLanguageChanged` callbacks. Returns `true` if the language was found (previously loaded). |
| `GetCurrentLanguage` | `const std::string& GetCurrentLanguage() const` | Return the active language code (e.g. `"en"`). |
| `GetAvailableLanguages` | `std::vector<std::string> GetAvailableLanguages() const` | Return a list of all loaded language codes. |
| `GetString` | `const std::string& GetString(const std::string& key) const` | Look up a localized string by key. Falls back to the fallback language if the key is missing in the current language. Returns the raw key string if not found in either. |
| `Format` | `template<typename... Args> std::string Format(const std::string& key, Args&&... args) const` | Format a localized template with positional arguments. Replaces `{0}`, `{1}`, etc. in the template. Arguments are converted to strings via `std::ostringstream`. |
| `SetFallbackLanguage` | `void SetFallbackLanguage(const std::string& languageCode)` | Set the fallback language used when a key is missing in the current language. Default is `"en"`. |
| `OnLanguageChanged` | `void OnLanguageChanged(std::function<void(const std::string&)> callback)` | Register a callback that fires whenever `SetCurrentLanguage()` is called. The callback receives the new language code. |

### Console Integration Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `Console_GetStatus` | `std::string Console_GetStatus() const` | Return a status string showing current language, fallback language, number of loaded languages, and entry counts. |
| `Console_ListKeys` | `std::string Console_ListKeys() const` | Return a string listing all keys for the current language. |

---

## Format String Substitution

The `Format()` method uses positional placeholders `{0}`, `{1}`, `{2}`, etc. Each argument is converted to a string using `std::ostringstream` and then substituted into the template via the internal `FormatImpl()` method.

### How It Works Internally

```cpp
template <typename... Args>
std::string Format(const std::string& key, Args&&... args) const
{
    const std::string& tmpl = GetString(key);
    std::vector<std::string> argStrings;
    (argStrings.push_back(ToString(std::forward<Args>(args))), ...);
    return FormatImpl(tmpl, argStrings);
}
```

The `ToString()` helper uses `std::ostringstream` to convert each argument:

```cpp
template <typename T>
static std::string ToString(const T& value)
{
    std::ostringstream oss;
    oss << value;
    return oss.str();
}
```

### Substitution Rules

1. Placeholders are zero-indexed: `{0}` is the first argument, `{1}` is the second, etc.
2. If a placeholder index exceeds the number of provided arguments, the placeholder remains in the output unchanged.
3. Arguments can be any type that supports `operator<<` to `std::ostringstream` (integers, floats, strings, etc.).
4. Placeholders can appear in any order and can repeat within a template.

### Examples

```cpp
auto& loc = LocalizationSystem::Get();

// Simple substitution
// Template: "Health: {0}"
loc.Format("hud.health", 85);                    // "Health: 85"

// Multiple arguments
// Template: "Ammo: {0}/{1}"
loc.Format("hud.ammo", 30, 120);                 // "Ammo: 30/120"

// Float arguments
// Template: "Distance: {0}m"
loc.Format("hud.distance", 42.5f);               // "Distance: 42.5m"

// String arguments
// Template: "Player {0} scored {1} points"
loc.Format("game.scored", std::string("Alice"), 1500);

// Repeated placeholders
// Template: "{0} vs {0}: mirror match!"
loc.Format("game.mirror", std::string("Warrior"));
// Result: "Warrior vs Warrior: mirror match!"
```

---

## Language Switching

Language can be changed at any time during gameplay. The system fires all registered callbacks so that UI elements can refresh their displayed text.

```cpp
auto& loc = LocalizationSystem::Get();

// Register callbacks before switching
loc.OnLanguageChanged([](const std::string& langCode) {
    RefreshAllUIText();
});

loc.OnLanguageChanged([](const std::string& langCode) {
    LOG_INFO("Language changed to: {}", langCode);
});

// Switch language -- all callbacks fire immediately
loc.SetCurrentLanguage("fr");

// Query available languages for a settings menu dropdown
auto languages = loc.GetAvailableLanguages();
for (const auto& lang : languages)
{
    AddDropdownOption(lang);
}
```

### Language Change Flow

```
SetCurrentLanguage("fr")
    |
    +--> Acquire m_mutex
    |
    +--> Validate language code exists in m_languages
    |        |
    |        +--> [Not found] return false
    |        +--> [Found] update m_currentLanguage = "fr"
    |
    +--> Fire all OnLanguageChanged callbacks (synchronous)
    |        |
    |        +--> callback_1("fr")
    |        +--> callback_2("fr")
    |        +--> ...
    |
    +--> Release m_mutex
    |
    +--> return true
```

---

## Fallback Language

When a key is missing from the current language's string table, the system automatically looks it up in the fallback language before giving up. This is essential for incremental translation workflows where not all strings have been translated yet.

```cpp
auto& loc = LocalizationSystem::Get();

// Default fallback is "en"
loc.SetFallbackLanguage("en");

// If "fr" is missing "menu.credits" but "en" has it:
loc.SetCurrentLanguage("fr");
loc.GetString("menu.credits");   // Returns English fallback: "Credits"

// If key is missing in both current AND fallback:
loc.GetString("nonexistent.key"); // Returns the raw key: "nonexistent.key"
```

### Lookup Order Diagram

```
GetString("menu.credits")
    |
    +--> Check current language ("fr") StringTable
    |        |
    |        +--> [Found] return localized string
    |        +--> [Not found] continue
    |
    +--> Check fallback language ("en") StringTable
    |        |
    |        +--> [Found] return fallback string
    |        +--> [Not found] continue
    |
    +--> Return the raw key string ("menu.credits")
```

---

## Thread Safety

`LocalizationSystem` is protected by a `std::mutex` (`m_mutex`). The following guarantees apply:

| Operation | Thread Safety |
|-----------|--------------|
| `LoadLanguage()` | Must be called from the main thread or during initialization. Acquires the mutex. |
| `SetCurrentLanguage()` | Must be called from the main thread. Acquires the mutex and fires callbacks synchronously. |
| `GetString()` | Thread-safe for concurrent reads after all loading is complete. Acquires mutex (`mutable`). |
| `Format()` | Thread-safe for concurrent reads (internally calls `GetString()`). |
| `OnLanguageChanged()` | Must be called from the main thread (modifies the callback vector). |
| `GetAvailableLanguages()` | Thread-safe for concurrent reads. |

**Best practice:** Load all languages during initialization (e.g. during a loading screen), then perform reads freely from any thread. Avoid calling `LoadLanguage()` or `SetCurrentLanguage()` from background threads.

---

## Internal Members

| Member | Type | Default | Description |
|--------|------|---------|-------------|
| `m_languages` | `std::unordered_map<std::string, StringTable>` | (empty) | Map of language code to string table |
| `m_currentLanguage` | `std::string` | `"en"` | Currently active language code |
| `m_fallbackLanguage` | `std::string` | `"en"` | Fallback language for missing keys |
| `m_languageChangedCallbacks` | `std::vector<std::function<void(const std::string&)>>` | (empty) | Registered language change callbacks |
| `m_mutex` | `mutable std::mutex` | N/A | Protects concurrent access to the language maps |

The constructor is private (`LocalizationSystem() = default;`), enforcing the singleton pattern. Access is only through `Get()`.

---

## Console Commands

The localization system registers two console commands for debugging:

| Command | Description |
|---------|-------------|
| `loc_status` | Display current language, fallback language, number of loaded languages, and entry count per language. Calls `Console_GetStatus()` internally. |
| `loc_keys` | List all keys for the currently active language. Calls `Console_ListKeys()` internally. Useful for verifying translation coverage. |

### Example Console Output

```
> loc_status
Localization System Status:
  Current language: en
  Fallback language: en
  Loaded languages: 4 (en, fr, de, ja)
  en: 156 entries
  fr: 142 entries
  de: 138 entries
  ja: 151 entries

> loc_keys
Keys for language 'en' (156):
  dialog.npc_farewell
  dialog.npc_greeting
  hud.ammo
  hud.health
  hud.score
  menu.play
  menu.quit
  menu.settings
  ...
```

---

## File Organization

Recommended directory structure for localization assets:

```
Data/
  Localization/
    en.json          # English (primary / fallback)
    fr.json          # French
    de.json          # German
    es.json          # Spanish
    it.json          # Italian
    ja.json          # Japanese
    ko.json          # Korean
    zh.json          # Chinese (Simplified)
    pt-br.json       # Portuguese (Brazil)
    ru.json          # Russian
    ar.json          # Arabic
    pl.json          # Polish
```

---

## Integration with Other Systems

### Dialogue System

The [Dialogue System](Dialogue-System.md) references localization keys in dialogue nodes rather than embedding raw text. This ensures all dialogue text is translated when the language changes.

```cpp
// Dialogue node stores a localization key
DialogueNode node;
node.textKey = "dialog.npc_greeting";

// At display time, resolve through localization
std::string displayText = LocalizationSystem::Get().GetString(node.textKey);
```

### UI System

The [UI System](UI-System.md) should register a language change callback to refresh all displayed text when the player changes language in settings.

```cpp
// In UI initialization
LocalizationSystem::Get().OnLanguageChanged([this](const std::string& lang) {
    for (auto& label : m_allLabels)
    {
        label.SetText(LocalizationSystem::Get().GetString(label.GetLocKey()));
    }
});
```

### AngelScript Scripting

The localization system is accessible from [AngelScript](Scripting-with-AngelScript.md) scripts for gameplay-driven text display.

```angelscript
// In AngelScript
string greeting = Localization::GetString("dialog.npc_greeting");
string ammoText = Localization::Format("hud.ammo", currentAmmo, maxAmmo);
```

### Save System

The [Save System](../gameplay-tools/Save-System.md) should persist the player's language preference so it is restored on the next session.

```cpp
// Save
saveData.Set("settings.language", loc.GetCurrentLanguage());

// Load
std::string savedLang = saveData.Get("settings.language", "en");
loc.SetCurrentLanguage(savedLang);
```

---

## Performance Considerations

1. **Startup cost:** Loading JSON files is O(n) in the number of entries. For large games (1000+ keys), this typically takes under 10ms per language file.
2. **Lookup cost:** `GetString()` uses `std::unordered_map` for O(1) average-case lookup by key. The mutex acquisition adds minimal overhead for read locks.
3. **Format cost:** `Format()` performs string scanning and replacement per call. For frequently formatted strings (e.g. HUD text updated every frame), consider caching the result and only reformatting when the underlying values change.
4. **Memory:** Each loaded language stores a full copy of all key-value pairs in its `StringTable`. For 10,000 keys averaging 50 characters each, expect approximately 1-2 MB per language.
5. **Lazy loading strategy:** Only load the current language and the fallback language at startup. Load additional languages on demand when the player opens the language selection menu.

---

## Troubleshooting

### Missing key returns the raw key string

If `GetString()` returns the key itself (e.g. `"menu.play"` instead of `"Play"`), check:

1. The JSON file was loaded successfully (`LoadLanguage()` returned `true`).
2. The key spelling matches exactly (case-sensitive).
3. The current language is set correctly (`GetCurrentLanguage()` returns the expected code).
4. The fallback language also has the key if the current language does not.
5. Use `loc_status` in the console to verify loaded language entry counts.

### Language change does not update UI

Ensure you have registered a callback via `OnLanguageChanged()` **before** calling `SetCurrentLanguage()`. Callbacks fire synchronously during `SetCurrentLanguage()`, so the UI refresh should happen immediately. If callbacks were registered after the switch, they will not retroactively fire.

### JSON parse errors

The `StringTable::LoadFromFile()` method expects valid JSON with flat key-value pairs (string keys mapping to string values). Nested objects, arrays, and non-string values will cause parsing issues. Use a JSON validator to check your files before loading.

### Placeholders not substituted

Verify that:
1. Placeholders use the correct format: `{0}`, `{1}`, etc. (not `%s`, `%d`, or `{name}`).
2. The number of arguments passed to `Format()` covers all placeholder indices used in the template.
3. Arguments are of a type that supports `operator<<` to `std::ostringstream`.

### Mutex deadlock

If the application hangs during localization calls, ensure you are not calling `SetCurrentLanguage()` from within an `OnLanguageChanged` callback, which would attempt to re-acquire the already-held mutex.

---

## Edge Cases

| Scenario | Behavior |
|----------|----------|
| Empty key string `""` | Returns the empty string (no lookup performed) |
| Key exists with empty value `""` | Returns the empty string (valid entry) |
| Loading the same language code twice | Second load creates a new `StringTable`, replacing the previous one |
| Setting current language to an unloaded code | `SetCurrentLanguage()` returns `false`; current language remains unchanged |
| Calling `Format()` with zero arguments | Returns the raw template string (no substitution occurs) |
| Fallback language not loaded | If fallback language has no `StringTable`, the raw key string is returned |
| Concurrent `GetString()` calls | Safe after initial loading (mutex-protected `mutable` access) |
| `Format()` with more arguments than placeholders | Extra arguments are converted but silently unused |
| `Format()` with fewer arguments than placeholders | Unmatched `{N}` placeholders remain as literal text in the output |

---

## Supported Language Codes

While the system accepts any string as a language code, the recommended convention is ISO 639-1 two-letter codes:

| Code | Language | Code | Language |
|------|----------|------|----------|
| `en` | English | `ja` | Japanese |
| `fr` | French | `ko` | Korean |
| `de` | German | `zh` | Chinese (Simplified) |
| `es` | Spanish | `ru` | Russian |
| `it` | Italian | `ar` | Arabic |
| `pt` | Portuguese | `pl` | Polish |

For regional variants, use extended codes such as `pt-br` (Brazilian Portuguese) or `zh-tw` (Traditional Chinese).

---

## Complete Integration Example

```cpp
#include "Engine/Localization/LocalizationSystem.h"

class GameApp
{
public:
    void Initialize()
    {
        auto& loc = Spark::LocalizationSystem::Get();

        // Load all supported languages
        loc.LoadLanguage("en", "Data/Localization/en.json");
        loc.LoadLanguage("fr", "Data/Localization/fr.json");
        loc.LoadLanguage("de", "Data/Localization/de.json");

        // Set fallback and default
        loc.SetFallbackLanguage("en");
        loc.SetCurrentLanguage(LoadSavedLanguagePreference());

        // Register global language change handler
        loc.OnLanguageChanged([this](const std::string& langCode) {
            OnLanguageChanged(langCode);
        });
    }

    void OnLanguageChanged(const std::string& langCode)
    {
        // Refresh all UI text
        m_mainMenu.RefreshText();
        m_hud.RefreshText();
        m_dialogueUI.RefreshText();

        // Save preference for next session
        SaveLanguagePreference(langCode);
    }

    void RenderHUD(int ammo, int maxAmmo, int health)
    {
        auto& loc = Spark::LocalizationSystem::Get();
        DrawText(loc.Format("hud.ammo", ammo, maxAmmo));
        DrawText(loc.Format("hud.health", health));
    }
};
```

---

## See Also

- [Dialogue System](Dialogue-System.md) -- Dialogue nodes reference localization keys for all NPC text
- [UI System](UI-System.md) -- Displaying localized text in game UI widgets
- [Scripting with AngelScript](Scripting-with-AngelScript.md) -- Accessing localized strings from gameplay scripts
- [Save System](../gameplay-tools/Save-System.md) -- Persisting the player's language preference
- [SparkEditor](../gameplay-tools/SparkEditor.md) -- Editor panels including localization tools
- [Event System](Event-System.md) -- Language change events can integrate with the global event bus
- [Console](../gameplay-tools/SparkConsole.md) -- `loc_status` and `loc_keys` debug commands
- [Asset Pipeline](../gameplay-tools/Asset-Pipeline.md) -- How localization JSON files are packaged for distribution
