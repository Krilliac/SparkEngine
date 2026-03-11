/**
 * @file LocalizationSystem.h
 * @brief Internationalization and localization system for Spark Engine
 * @author Spark Engine Team
 * @date 2025
 *
 * @details
 * Provides string table management, locale switching, and formatted text
 * substitution for all in-game text. Supports loading translations from
 * JSON files and runtime language switching without restart.
 *
 * ## Usage
 * @code
 *   auto& loc = LocalizationSystem::Get();
 *   loc.LoadLanguage("en", "Data/Localization/en.json");
 *   loc.LoadLanguage("fr", "Data/Localization/fr.json");
 *   loc.SetCurrentLanguage("en");
 *
 *   std::string text = loc.GetString("menu.play");         // "Play"
 *   std::string fmt  = loc.Format("hud.ammo", 30, 120);    // "Ammo: 30/120"
 * @endcode
 *
 * ## File format (JSON)
 * @code{.json}
 *   {
 *     "menu.play": "Play",
 *     "menu.settings": "Settings",
 *     "hud.ammo": "Ammo: {0}/{1}",
 *     "hud.health": "Health: {0}"
 *   }
 * @endcode
 */

#pragma once

#include <string>
#include <unordered_map>
#include <vector>
#include <functional>
#include <mutex>
#include <sstream>

namespace Spark
{

    /**
 * @brief String table mapping key → localized text for one language.
 */
    class StringTable
    {
      public:
        /**
     * @brief Load string entries from a JSON file.
     * @param filePath Path to the JSON localization file.
     * @return true if loading succeeded.
     */
        bool LoadFromFile(const std::string& filePath);

        /**
     * @brief Add or overwrite a single string entry.
     * @param key   String key (e.g. "menu.play").
     * @param value Localized text.
     */
        void SetEntry(const std::string& key, const std::string& value);

        /**
     * @brief Look up a localized string by key.
     * @param key String key to look up.
     * @return Localized text, or the key itself if not found.
     */
        const std::string& GetEntry(const std::string& key) const;

        /**
     * @brief Check whether a key exists in this table.
     * @param key String key to check.
     * @return true if the key has a mapping.
     */
        bool HasEntry(const std::string& key) const;

        /** @brief Return the number of entries in this table. */
        size_t GetEntryCount() const { return m_entries.size(); }

        /** @brief Return all keys in this table. */
        std::vector<std::string> GetAllKeys() const;

      private:
        std::unordered_map<std::string, std::string> m_entries;
        std::string m_missingPlaceholder;
    };

    // =============================================================================
    // LocalizationSystem
    // =============================================================================

    /**
 * @class LocalizationSystem
 * @brief Central localization manager — singleton accessible via Get().
 *
 * Manages multiple language string tables and provides formatted text
 * substitution. Thread-safe for concurrent reads after initial loading.
 */
    class LocalizationSystem
    {
      public:
        /** @brief Get the singleton instance. */
        static LocalizationSystem& Get();

        /**
     * @brief Load a language from a JSON file.
     * @param languageCode ISO 639-1 code (e.g. "en", "fr", "de", "ja").
     * @param filePath     Path to the JSON file.
     * @return true if loading succeeded.
     */
        bool LoadLanguage(const std::string& languageCode, const std::string& filePath);

        /**
     * @brief Set the active language.
     * @param languageCode ISO 639-1 code of a previously loaded language.
     * @return true if the language was found and set.
     */
        bool SetCurrentLanguage(const std::string& languageCode);

        /** @brief Get the current language code. */
        const std::string& GetCurrentLanguage() const { return m_currentLanguage; }

        /** @brief Get a list of all loaded language codes. */
        std::vector<std::string> GetAvailableLanguages() const;

        /**
     * @brief Look up a localized string by key in the current language.
     * @param key String key (e.g. "menu.play").
     * @return Localized text, or the key if not found.
     *
     * Falls back to the fallback language if the key is missing in the
     * current language.
     */
        const std::string& GetString(const std::string& key) const;

        /**
     * @brief Format a localized string with positional arguments.
     *
     * Replaces `{0}`, `{1}`, etc. in the localized template with the
     * provided arguments converted to strings.
     *
     * @tparam Args Argument types (must be streamable to std::ostringstream).
     * @param key  String key for the template.
     * @param args Values to substitute.
     * @return Formatted string.
     */
        template <typename... Args> std::string Format(const std::string& key, Args&&... args) const
        {
            const std::string& tmpl = GetString(key);
            std::vector<std::string> argStrings;
            (argStrings.push_back(ToString(std::forward<Args>(args))), ...);
            return FormatImpl(tmpl, argStrings);
        }

        /**
     * @brief Set the fallback language used when a key is missing.
     * @param languageCode ISO 639-1 code (default: "en").
     */
        void SetFallbackLanguage(const std::string& languageCode) { m_fallbackLanguage = languageCode; }

        /**
     * @brief Register a callback invoked when the language changes.
     * @param callback Function called with the new language code.
     */
        void OnLanguageChanged(std::function<void(const std::string&)> callback);

        // --- Console integration ---

        /** @brief Get localization system statistics (console integration). */
        std::string Console_GetStatus() const;

        /** @brief List all keys for the current language (console integration). */
        std::string Console_ListKeys() const;

      private:
        LocalizationSystem() = default;

        template <typename T> static std::string ToString(const T& value)
        {
            std::ostringstream oss;
            oss << value;
            return oss.str();
        }

        std::string FormatImpl(const std::string& tmpl, const std::vector<std::string>& args) const;

        std::unordered_map<std::string, StringTable> m_languages;
        std::string m_currentLanguage = "en";
        std::string m_fallbackLanguage = "en";
        std::vector<std::function<void(const std::string&)>> m_languageChangedCallbacks;
        mutable std::mutex m_mutex;
    };

} // namespace Spark
