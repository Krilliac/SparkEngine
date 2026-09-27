/**
 * @file FPSConsolePolicy.h
 * @brief Console input rules for SparkGameFPS: which commands a build registers
 *        and how numeric arguments are accepted.
 *
 * Contract:
 * - Thread affinity: none (constexpr / pure functions).
 * - Allocation: none.
 * - Used by: SparkGameModule::RegisterGameConsoleCommands (Core/Main.cpp) and
 *   the advanced console commands (Console/AdvancedConsoleCommands.cpp).
 */

#pragma once

#include <array>
#include <charconv>
#include <cmath>
#include <optional>
#include <string_view>
#include <system_error>

namespace SparkFPS::ConsolePolicy
{

    /**
     * @brief Commands that bypass normal play: teleports, spawns, time control,
     *        free items/XP/progression, match and wave control, scene and
     *        cinematic authoring, world destruction.
     *
     * ENABLE_DEVCOMMANDS_IN_SHIPPING=OFF promises that a Shipping build carries
     * none of them. The gate used to wrap only god/noclip, so every other entry
     * here shipped. Registration is where the gate lives: a command that is never
     * registered cannot be typed, scripted with -exec, or sent over the external
     * console pipe.
     */
    inline constexpr std::array<std::string_view, 21> kDeveloperCommands = {
        "god",      "noclip",   "player_tp",   "spawn",     "game_timescale",  "scene_load", "scene_save",
        "gamemode", "give",     "quest_start", "quest_all", "destroy",         "weather",    "dialogue_start",
        "seq_play", "seq_stop", "seq_time",    "wave_skip", "wave_difficulty", "xp",         "powerup",
    };

    /// Whether this build keeps developer commands (every non-Shipping build, or
    /// Shipping with ENABLE_DEVCOMMANDS_IN_SHIPPING=ON).
#if defined(SPARK_DEVCOMMANDS_IN_SHIPPING) || !defined(SPARK_BUILD_SHIPPING)
    inline constexpr bool kDeveloperCommandsEnabled = true;
#else
    inline constexpr bool kDeveloperCommandsEnabled = false;
#endif

    [[nodiscard]] constexpr bool IsDeveloperCommand(std::string_view name) noexcept
    {
        for (std::string_view candidate : kDeveloperCommands)
        {
            if (candidate == name)
                return true;
        }
        return false;
    }

    /// Whether @p name may be registered when developer commands are
    /// @p developerCommandsEnabled. Pass kDeveloperCommandsEnabled in production.
    [[nodiscard]] constexpr bool ShouldRegister(std::string_view name, bool developerCommandsEnabled) noexcept
    {
        return developerCommandsEnabled || !IsDeveloperCommand(name);
    }

    /**
     * @brief Parse a whole console token as a finite float.
     *
     * std::stof accepts "nan", "inf", leading whitespace and trailing garbage
     * ("1.5abc" -> 1.5). NaN then slips past every range check (all comparisons
     * are false), so `game_timescale nan` stored NaN as the time scale and every
     * later gameplay update ran with dt = NaN. This rejects all of those.
     *
     * @return The value, or std::nullopt when the token is not exactly one
     *         finite decimal number within float range.
     */
    [[nodiscard]] inline std::optional<float> ParseFiniteFloat(std::string_view token) noexcept
    {
        if (token.empty())
            return std::nullopt;
        // from_chars rejects a leading '+'; accept it for console convenience.
        if (token.front() == '+')
            token.remove_prefix(1);

        float value = 0.0f;
        const char* const first = token.data();
        const char* const last = token.data() + token.size();
        const auto [end, error] = std::from_chars(first, last, value, std::chars_format::general);
        if (error != std::errc{} || end != last || !std::isfinite(value))
            return std::nullopt;
        return value;
    }

} // namespace SparkFPS::ConsolePolicy
