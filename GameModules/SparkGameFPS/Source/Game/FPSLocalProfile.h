/**
 * @file FPSLocalProfile.h
 * @brief The local single-player profile SparkGameFPS persists through SaveSystem.
 *
 * SaveSystem serialises the ECS world; the FPS module keeps its player,
 * progression and score state outside the ECS, so that state travels in the
 * save file's module-owned custom-state map instead.
 */

#pragma once

#include <string>
#include <unordered_map>

namespace Spark
{
    /**
     * @brief Declared local profile state for the single-player slice.
     *
     * Every field here is restored by Game::ApplyLocalProfile after a quickload,
     * so a save/reload round trip preserves progression, class, loadout, and score.
     */
    struct FPSLocalProfile
    {
        /// Profile block format version. Bump when a field's meaning changes.
        static constexpr int kVersion = 1;

        /// Key prefix used for every profile entry in the save's custom-state map.
        static constexpr const char* kKeyPrefix = "fps.profile.";

        int version = kVersion;       ///< Version of the block this profile was read from.
        int progressionLevel = 1;     ///< Progression level (derived from XP on restore).
        int progressionXP = 0;        ///< Total accumulated XP.
        int playerClass = 0;          ///< SparkEditor::PlayerClass cast to int.
        int weapon = 0;               ///< SparkEditor::WeaponType currently equipped, cast to int.
        int kills = 0;                ///< Kills recorded by GameMode for the local player.
        int deaths = 0;               ///< Deaths recorded by GameMode for the local player.
        int score = 0;                ///< Combined GameMode score for the local player.
        float playTimeSeconds = 0.0f; ///< Accumulated play time.
        float health = 100.0f;        ///< Player health at save time.
        float armor = 0.0f;           ///< Player armor at save time.

        /**
         * @brief Write this profile into a save file's custom-state map.
         *
         * Existing unrelated entries are left untouched, so other writers can
         * share the same map.
         */
        void WriteTo(std::unordered_map<std::string, std::string>& customState) const;

        /**
         * @brief Read a profile back out of a save file's custom-state map.
         *
         * @param customState Map returned by SaveSystem::Load.
         * @param outError    Human-readable reason when the read fails.
         * @return false when no profile block is present, when it was written by
         *         a newer module version, or when a field is unparseable. The
         *         profile is left unchanged in that case.
         */
        bool ReadFrom(const std::unordered_map<std::string, std::string>& customState, std::string& outError);
    };
} // namespace Spark
