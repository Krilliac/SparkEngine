/**
 * @file FPSConsolePolicy.h
 * @brief Console input rules for SparkGameFPS: which commands a build registers
 *        and how numeric arguments are accepted.
 *
 * Contract:
 * - Thread affinity: none (constexpr / pure functions).
 * - Allocation: none, except ParseFiniteFloat, which builds a short-lived
 *   string stream per token (console input only, never per frame).
 * - Used by: SparkGameModule::RegisterGameConsoleCommands (Core/Main.cpp) and
 *   the advanced console commands (Console/AdvancedConsoleCommands.cpp).
 */

#pragma once

#include <array>
#include <cmath>
#include <locale>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

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
        // libc++ 18 (build-linux-clang, clang-tidy, MSan, macOS) deletes the
        // floating-point std::from_chars overload, so this parses with a
        // classic-locale, no-skip stream instead (same approach as
        // Game::ParseAuthoredFiniteFloat and Core/ExecScript.cpp).
        //
        // Screen first: stream extraction on some standard libraries hands the
        // accumulated characters to strtod, which would accept hex floats,
        // "inf" and "nan". Only decimal digits, '.', exponent and signs pass,
        // and the token must start with a digit, '.', or a sign.
        if (token.empty() || token.find_first_not_of("0123456789.eE+-") != std::string_view::npos)
            return std::nullopt;
        const char lead = token.front();
        if (!((lead >= '0' && lead <= '9') || lead == '.' || lead == '+' || lead == '-'))
            return std::nullopt;

        try
        {
            float value = 0.0f;
            std::istringstream stream{std::string(token)};
            stream.imbue(std::locale::classic());
            stream >> std::noskipws >> value;
            // Out-of-range input sets failbit; the whole token must be consumed.
            if (stream.fail() || stream.peek() != std::char_traits<char>::eof() || !std::isfinite(value))
                return std::nullopt;
            return value;
        }
        catch (...)
        {
            // Allocation failure while building the stream: reject the token.
            return std::nullopt;
        }
    }

} // namespace SparkFPS::ConsolePolicy
