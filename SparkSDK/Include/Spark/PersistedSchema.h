/// @file PersistedSchema.h
/// @brief Per-game-module persisted-schema version declarations for save custom state.
///
/// SaveSystem versions the save envelope; each game module owns the meaning of the
/// custom-state entries it writes and versions them itself. A module declares one
/// ModulePersistedSchema (its name, the custom-state key that carries its version,
/// and the version it writes). Readers apply the same window as engine saves and
/// scenes (owner decision OD-03): accept exactly N and N-1, write N only, and fail
/// closed with a versioned diagnostic for anything missing, older, or newer.

#pragma once

#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

namespace Spark
{
    /// @brief One game module's declaration of the schema version it persists.
    struct ModulePersistedSchema
    {
        std::string_view moduleName; ///< Module name used in diagnostics (e.g. "SparkGameFPS").
        std::string_view versionKey; ///< Custom-state key that stores this module's schema version.
        uint32_t currentVersion = 1; ///< Version this build writes (N); must be at least 1.

        /// @brief Oldest version this build reads: N-1, or N when no earlier version exists.
        constexpr uint32_t OldestReadableVersion() const noexcept
        {
            return currentVersion > 1 ? currentVersion - 1 : currentVersion;
        }
    };

    /// @brief Record @p schema's current version in a save's custom-state map.
    inline void WriteModuleSchemaVersion(const ModulePersistedSchema& schema,
                                         std::unordered_map<std::string, std::string>& customState)
    {
        customState[std::string(schema.versionKey)] = std::to_string(schema.currentVersion);
    }

    /**
     * @brief Read and check a module's persisted schema version from save custom state.
     *
     * @param schema      The module's declaration.
     * @param customState Custom-state map returned by SaveSystem::Load.
     * @param outVersion  Set to the stored version when it is readable (N or N-1).
     * @param outError    Versioned, actionable reason when the check fails.
     * @return true when the stored version is N or N-1. The caller migrates N-1 data
     *         to N in memory. false when the key is missing or malformed, or the
     *         version is outside the window; @p outVersion is left unchanged then.
     */
    inline bool CheckModuleSchemaVersion(const ModulePersistedSchema& schema,
                                         const std::unordered_map<std::string, std::string>& customState,
                                         uint32_t& outVersion, std::string& outError)
    {
        const std::string owner = std::string(schema.moduleName) + " persisted schema";
        const std::string window = "this build reads versions " + std::to_string(schema.OldestReadableVersion()) + "-" +
                                   std::to_string(schema.currentVersion) + " and writes version " +
                                   std::to_string(schema.currentVersion);

        const auto entry = customState.find(std::string(schema.versionKey));
        if (entry == customState.end())
        {
            outError = owner + ": missing version key '" + std::string(schema.versionKey) + "'";
            return false;
        }

        const std::string& text = entry->second;
        uint32_t stored = 0;
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), stored);
        if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
        {
            outError = owner + ": version key '" + std::string(schema.versionKey) + "' holds '" + text +
                       "', which is not a version number";
            return false;
        }

        if (stored > schema.currentVersion)
        {
            outError = owner + ": data is version " + std::to_string(stored) + ", but " + window +
                       "; load it with the newer build that wrote it";
            return false;
        }
        if (stored < schema.OldestReadableVersion())
        {
            outError = owner + ": data is version " + std::to_string(stored) + ", but " + window +
                       "; convert it with an older build that reads version " + std::to_string(stored);
            return false;
        }

        outVersion = stored;
        outError.clear();
        return true;
    }
} // namespace Spark
