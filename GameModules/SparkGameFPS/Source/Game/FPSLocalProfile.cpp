/**
 * @file FPSLocalProfile.cpp
 * @brief Serialisation of the SparkGameFPS local profile into save custom state.
 */

#include "FPSLocalProfile.h"

#include <charconv>
#include <string>

namespace Spark
{
    namespace
    {
        std::string Key(const char* field)
        {
            return std::string(FPSLocalProfile::kKeyPrefix) + field;
        }

        /// @brief Parse one required field; reports the key that failed.
        template <typename T>
        bool ReadField(const std::unordered_map<std::string, std::string>& customState, const char* field, T& outValue,
                       std::string& outError)
        {
            const std::string key = Key(field);
            const auto entry = customState.find(key);
            if (entry == customState.end())
            {
                outError = "missing key '" + key + "'";
                return false;
            }

            const std::string& text = entry->second;
            T parsed{};
            const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
            if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
            {
                outError = "unparseable value for '" + key + "': '" + text + "'";
                return false;
            }
            outValue = parsed;
            return true;
        }
    } // namespace

    void FPSLocalProfile::WriteTo(std::unordered_map<std::string, std::string>& customState) const
    {
        customState[Key("version")] = std::to_string(kVersion);
        customState[Key("level")] = std::to_string(progressionLevel);
        customState[Key("xp")] = std::to_string(progressionXP);
        customState[Key("class")] = std::to_string(playerClass);
        customState[Key("weapon")] = std::to_string(weapon);
        customState[Key("kills")] = std::to_string(kills);
        customState[Key("deaths")] = std::to_string(deaths);
        customState[Key("score")] = std::to_string(score);
        customState[Key("playTime")] = std::to_string(playTimeSeconds);
        customState[Key("health")] = std::to_string(health);
        customState[Key("armor")] = std::to_string(armor);
    }

    bool FPSLocalProfile::ReadFrom(const std::unordered_map<std::string, std::string>& customState,
                                   std::string& outError)
    {
        FPSLocalProfile parsed;
        if (!ReadField(customState, "version", parsed.version, outError))
            return false;
        if (parsed.version > kVersion)
        {
            outError = "profile was written by a newer module (version " + std::to_string(parsed.version) +
                       ", this build reads up to " + std::to_string(kVersion) + ")";
            return false;
        }

        if (!ReadField(customState, "level", parsed.progressionLevel, outError) ||
            !ReadField(customState, "xp", parsed.progressionXP, outError) ||
            !ReadField(customState, "class", parsed.playerClass, outError) ||
            !ReadField(customState, "weapon", parsed.weapon, outError) ||
            !ReadField(customState, "kills", parsed.kills, outError) ||
            !ReadField(customState, "deaths", parsed.deaths, outError) ||
            !ReadField(customState, "score", parsed.score, outError) ||
            !ReadField(customState, "playTime", parsed.playTimeSeconds, outError) ||
            !ReadField(customState, "health", parsed.health, outError) ||
            !ReadField(customState, "armor", parsed.armor, outError))
        {
            return false;
        }

        *this = parsed;
        outError.clear();
        return true;
    }
} // namespace Spark
