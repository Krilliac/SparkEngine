/**
 * @file WaveComposition.h
 * @brief Pure, bounded enemy-wave composition for the FPS survival mode
 *
 * Split out of WaveSpawner so the composition rules link without the D3D-backed
 * Game type. Every function here is total over its inputs: a wave number or
 * difficulty scale from the console, a save or a script is clamped first, so
 * no input can reach a signed overflow or produce more than
 * MAX_ENEMIES_PER_WAVE enemies.
 *
 * Thread affinity: none (pure functions). Allocation: the announcement string only.
 */

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>

namespace Spark
{

    /**
     * @brief Defines the composition of a single enemy wave
     */
    struct WaveDefinition
    {
        int waveNumber = 1;
        int gruntCount = 0;
        int scoutCount = 0;
        int guardCount = 0;
        int heavyCount = 0;
        int sniperCount = 0;
        int medicCount = 0;
        float healthMultiplier = 1.0f; ///< Scales enemy health
        float damageMultiplier = 1.0f; ///< Scales enemy damage
        float speedMultiplier = 1.0f;  ///< Scales enemy movement speed
        bool isBossWave = false;       ///< Boss waves have special announcements
        std::string announcement;      ///< Text shown at wave start

        /// Total enemies the wave spawns.
        [[nodiscard]] int TotalEnemies() const noexcept
        {
            return gruntCount + scoutCount + guardCount + heavyCount + sniperCount + medicCount;
        }
    };

    namespace WaveComposition
    {
        /// Hard ceiling on enemies spawned by one wave.
        inline constexpr int MAX_ENEMIES_PER_WAVE = 30;
        /// Hard ceiling on any wave number (and on a configured total wave count).
        inline constexpr int MAX_WAVE_NUMBER = 1000;
        /// Documented range of the wave_difficulty console command.
        inline constexpr float MIN_DIFFICULTY_SCALE = 1.0f;
        inline constexpr float MAX_DIFFICULTY_SCALE = 3.0f;

        /// Clamp a wave number into [1, MAX_WAVE_NUMBER].
        [[nodiscard]] inline int ClampWaveNumber(int waveNumber) noexcept
        {
            return std::clamp(waveNumber, 1, MAX_WAVE_NUMBER);
        }

        /// True when @p scale is finite and inside the documented difficulty range.
        [[nodiscard]] inline bool IsValidDifficultyScale(float scale) noexcept
        {
            return std::isfinite(scale) && scale >= MIN_DIFFICULTY_SCALE && scale <= MAX_DIFFICULTY_SCALE;
        }

        /// Non-finite scales fall back to 1.0; finite ones are clamped into the documented range.
        [[nodiscard]] inline float SanitizeDifficultyScale(float scale) noexcept
        {
            if (!std::isfinite(scale))
                return MIN_DIFFICULTY_SCALE;
            return std::clamp(scale, MIN_DIFFICULTY_SCALE, MAX_DIFFICULTY_SCALE);
        }

        /**
         * @brief Build the composition for @p waveNumber.
         *
         * Postcondition: 0 < TotalEnemies() <= MAX_ENEMIES_PER_WAVE, and a boss wave keeps at least one heavy.
         * Every category, heavies included, is scaled down proportionally when the raw composition exceeds the
         * cap; any rounding excess left by the boss-heavy floor is then trimmed, grunts first.
         */
        [[nodiscard]] inline WaveDefinition Compose(int requestedWave, float difficultyScale)
        {
            const int waveNumber = ClampWaveNumber(requestedWave);
            const float scale = SanitizeDifficultyScale(difficultyScale);

            WaveDefinition wave;
            wave.waveNumber = waveNumber;
            const float step = static_cast<float>(waveNumber - 1);
            wave.healthMultiplier = 1.0f + step * 0.1f * scale;
            wave.damageMultiplier = 1.0f + step * 0.08f * scale;
            wave.speedMultiplier = 1.0f + step * 0.03f * scale;

            const bool isBoss = (waveNumber % 5 == 0);
            wave.isBossWave = isBoss;
            if (isBoss)
            {
                // Boss waves: fewer but tougher enemies + a heavy
                wave.gruntCount = waveNumber / 2;
                wave.guardCount = waveNumber / 3;
                wave.heavyCount = 1 + waveNumber / 5;
                wave.healthMultiplier *= 1.5f;
                wave.announcement = "BOSS WAVE " + std::to_string(waveNumber) + "!";
            }
            else
            {
                // Normal waves: scaling composition
                wave.gruntCount = 2 + waveNumber;
                wave.scoutCount = (waveNumber >= 3) ? (waveNumber / 2) : 0;
                wave.guardCount = (waveNumber >= 5) ? (waveNumber / 3) : 0;
                wave.sniperCount = (waveNumber >= 7) ? (waveNumber / 4) : 0;
                wave.medicCount = (waveNumber >= 8) ? (waveNumber / 5) : 0;
                wave.heavyCount = (waveNumber >= 10) ? (waveNumber / 6) : 0;
                wave.announcement = "Wave " + std::to_string(waveNumber);
            }

            const int total = wave.TotalEnemies();
            if (total > MAX_ENEMIES_PER_WAVE)
            {
                const double ratio = static_cast<double>(MAX_ENEMIES_PER_WAVE) / static_cast<double>(total);
                const auto scaleCount = [ratio](int count) { return static_cast<int>(count * ratio); };
                wave.gruntCount = scaleCount(wave.gruntCount);
                wave.scoutCount = scaleCount(wave.scoutCount);
                wave.guardCount = scaleCount(wave.guardCount);
                wave.heavyCount = scaleCount(wave.heavyCount);
                wave.sniperCount = scaleCount(wave.sniperCount);
                wave.medicCount = scaleCount(wave.medicCount);
                if (isBoss)
                    wave.heavyCount = std::max(wave.heavyCount, 1); // Always keep at least 1 heavy on boss

                // Enforce the cap as a postcondition, not an approximation: trim grunts first, heavies last.
                const std::array<int*, 6> trimOrder = {&wave.gruntCount,  &wave.scoutCount, &wave.guardCount,
                                                       &wave.sniperCount, &wave.medicCount, &wave.heavyCount};
                for (int* count : trimOrder)
                {
                    const int minimum = (count == &wave.heavyCount && isBoss) ? 1 : 0;
                    const int excess = wave.TotalEnemies() - MAX_ENEMIES_PER_WAVE;
                    if (excess <= 0)
                        break;
                    *count -= std::min(excess, *count - minimum);
                }
            }
            return wave;
        }
    } // namespace WaveComposition

} // namespace Spark
