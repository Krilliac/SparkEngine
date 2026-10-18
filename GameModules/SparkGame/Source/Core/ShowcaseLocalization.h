/**
 * @file ShowcaseLocalization.h
 * @brief String tables for the SparkGame showcase status output
 * @author Spark Engine Team
 * @date 2026
 *
 * The showcase ships English and French string tables under
 * Assets/Localization/SparkGame. They are validated before they reach the
 * host LocalizationSystem, and every status label is looked up through it.
 */

#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace Spark
{
    class LocalizationSystem;
}

namespace ShowcaseLocalization
{
    /// @brief A status label key and the English text used when the host has no table for it
    struct ShowcaseString
    {
        const char* key;
        const char* english;
    };

    /// @brief Every key a showcase string table must define
    inline constexpr ShowcaseString kShowcaseStrings[] = {
        {"showcase.status.title", "=== Gameplay Showcase Status ==="},
        {"showcase.status.spawned", "Spawned entities"},
        {"showcase.status.damage_events", "Damage events"},
        {"showcase.status.kill_events", "Kill events"},
        {"showcase.status.weather_changes", "Weather changes"},
        {"showcase.status.total_damage", "Total damage dealt"},
        {"showcase.status.coroutine", "Coroutine sequence"},
        {"showcase.status.weather", "Current weather"},
        {"showcase.status.weather_timer", "Weather timer"},
        {"showcase.status.time_of_day", "Time of day"},
        {"showcase.status.day_count", "Day count"},
        {"showcase.status.language", "Language"},
    };

    /// @brief Shipped languages: {ISO 639-1 code, path relative to the content root}
    inline constexpr const char* kShowcaseLanguageFiles[][2] = {
        {"en", "Assets/Localization/SparkGame/showcase_en.json"},
        {"fr", "Assets/Localization/SparkGame/showcase_fr.json"},
    };

    /**
     * @brief Load every shipped showcase language into @p localization.
     *
     * Fails closed: each file is parsed and checked for every key in kShowcaseStrings
     * first, and @p localization is changed only when all of them pass.
     * @param localization Host localization system that receives the tables
     * @param root Content root the kShowcaseLanguageFiles paths are relative to
     * @param error Receives the first problem found (optional)
     * @return true when every language was loaded
     */
    bool LoadShowcaseStrings(Spark::LocalizationSystem& localization, const std::filesystem::path& root,
                             std::string* error = nullptr);

    /**
     * @brief Localized text for a showcase key in the host's current language.
     * @return The host's string, or the built-in English text when there is no host
     *         localization or it has no entry for @p key
     */
    std::string ShowcaseText(const Spark::LocalizationSystem* localization, std::string_view key);

} // namespace ShowcaseLocalization
