/**
 * @file AchievementSystem.h
 * @brief Achievement tracking with progressive, tiered, and cumulative support
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides a full achievement system supporting multiple unlock styles:
 * - Binary (simple locked/unlocked)
 * - Progressive (0-to-target progress bar)
 * - Tiered (bronze/silver/gold thresholds)
 * - Hidden (description revealed on unlock)
 * - Cumulative (persisted across sessions)
 *
 * Integrates with BinaryWriter/BinaryReader for save/load and exposes
 * a callback for unlock notifications (e.g. toast popups).
 *
 * ## Usage
 * @code
 *   auto& ach = Spark::Gameplay::AchievementSystem::GetInstance();
 *   ach.Initialize();
 *
 *   AchievementDefinition def;
 *   def.id = 1;
 *   def.name = "First Blood";
 *   def.description = "Defeat your first enemy";
 *   def.type = AchievementType::Binary;
 *   def.pointValue = 10;
 *   ach.RegisterAchievement(def);
 *
 *   ach.OnAchievementUnlocked([](const AchievementUnlockEvent& e) {
 *       ShowToast(e.name);
 *   });
 *
 *   ach.UnlockAchievement(1);
 * @endcode
 */

#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// BinaryWriter / BinaryReader are used directly by the inline SaveToWriter /
// LoadFromReader definitions below, so pull in the full serializer definition.
#include "../../Utils/Serializer.h"

namespace Spark::Gameplay
{

    // =========================================================================
    // Enums
    // =========================================================================

    /**
     * @brief Achievement unlock behaviour type.
     */
    enum class AchievementType : uint8_t
    {
        Binary,      ///< Simple locked/unlocked toggle
        Progressive, ///< Tracks 0 -> targetValue
        Tiered,      ///< Multiple thresholds (bronze/silver/gold)
        Hidden,      ///< Description hidden until unlocked
        Cumulative   ///< Progress persisted across sessions
    };

    // =========================================================================
    // Data structs
    // =========================================================================

    /**
     * @brief Static definition of an achievement.
     */
    struct AchievementDefinition
    {
        uint32_t id = 0;                                ///< Unique achievement ID
        std::string name;                               ///< Display name
        std::string description;                        ///< Visible description
        std::string hiddenDescription;                  ///< Shown only after unlock for Hidden type
        std::string iconPath;                           ///< Path to icon asset
        AchievementType type = AchievementType::Binary; ///< Achievement behaviour
        float targetValue = 1.0f;                       ///< Progress target (Binary: 1.0)
        uint32_t pointValue = 0;                        ///< Gamerscore / point value
        uint8_t tier = 0;                               ///< Tier level for Tiered type
    };

    /**
     * @brief Runtime progress state for a single achievement.
     */
    struct AchievementProgress
    {
        uint32_t achievementId = 0;   ///< Matching AchievementDefinition::id
        float currentValue = 0.0f;    ///< Current progress toward target
        bool unlocked = false;        ///< Whether the achievement has been earned
        uint64_t unlockTimestamp = 0; ///< Epoch seconds when unlocked (0 if locked)
        bool notified = false;        ///< Whether the unlock notification was sent
    };

    /**
     * @brief Event payload delivered when an achievement is unlocked.
     */
    struct AchievementUnlockEvent
    {
        uint32_t achievementId = 0;
        std::string name;
        std::string description;
        uint32_t pointValue = 0;
    };

    // =========================================================================
    // AchievementSystem
    // =========================================================================

    /**
     * @brief Singleton system for registering, tracking, and persisting achievements.
     */
    class AchievementSystem
    {
      public:
        static AchievementSystem& GetInstance()
        {
            static AchievementSystem instance;
            return instance;
        }

        // -----------------------------------------------------------------
        // Lifecycle
        // -----------------------------------------------------------------

        /// @brief Initialize the system, clearing all definitions and progress
        void Initialize()
        {
            m_definitions.clear();
            m_progress.clear();
            m_unlockCallbacks.clear();
            m_initialized = true;
        }

        /// @brief Shut down and release all state
        void Shutdown()
        {
            m_definitions.clear();
            m_progress.clear();
            m_unlockCallbacks.clear();
            m_initialized = false;
        }

        // -----------------------------------------------------------------
        // Registration
        // -----------------------------------------------------------------

        /// @brief Register a new achievement definition
        /// @param def Achievement definition to register (id must be unique)
        void RegisterAchievement(const AchievementDefinition& def)
        {
            m_definitions[def.id] = def;

            // Create progress entry if it does not already exist (supports cumulative re-register)
            if (m_progress.find(def.id) == m_progress.end())
            {
                AchievementProgress prog;
                prog.achievementId = def.id;
                m_progress[def.id] = prog;
            }
        }

        // -----------------------------------------------------------------
        // Progress tracking
        // -----------------------------------------------------------------

        /// @brief Set the progress value for an achievement (clamped to target)
        /// @param id Achievement ID
        /// @param value New progress value
        void UpdateProgress(uint32_t id, float value)
        {
            auto defIt = m_definitions.find(id);
            if (defIt == m_definitions.end())
            {
                return;
            }

            auto& prog = m_progress[id];
            if (prog.unlocked)
            {
                return; // already earned
            }

            prog.currentValue = std::clamp(value, 0.0f, defIt->second.targetValue);
            CheckUnlock(id);
        }

        /// @brief Add a delta to the current progress
        /// @param id Achievement ID
        /// @param delta Amount to add (default 1.0)
        void IncrementProgress(uint32_t id, float delta = 1.0f)
        {
            auto defIt = m_definitions.find(id);
            if (defIt == m_definitions.end())
            {
                return;
            }

            auto& prog = m_progress[id];
            if (prog.unlocked)
            {
                return;
            }

            prog.currentValue = std::clamp(prog.currentValue + delta, 0.0f, defIt->second.targetValue);
            CheckUnlock(id);
        }

        /// @brief Force-unlock an achievement regardless of progress
        /// @param id Achievement ID
        void UnlockAchievement(uint32_t id)
        {
            auto defIt = m_definitions.find(id);
            if (defIt == m_definitions.end())
            {
                return;
            }

            auto& prog = m_progress[id];
            if (prog.unlocked)
            {
                return;
            }

            prog.currentValue = defIt->second.targetValue;
            prog.unlocked = true;
            prog.unlockTimestamp = GetCurrentTimestamp();
            NotifyUnlock(id);
        }

        // -----------------------------------------------------------------
        // Queries
        // -----------------------------------------------------------------

        /// @brief Check whether an achievement has been unlocked
        [[nodiscard]] bool IsUnlocked(uint32_t id) const
        {
            auto it = m_progress.find(id);
            return (it != m_progress.end()) && it->second.unlocked;
        }

        /// @brief Get progress data for an achievement
        /// @return Pointer to progress, or nullptr if the achievement does not exist
        [[nodiscard]] const AchievementProgress* GetProgress(uint32_t id) const
        {
            auto it = m_progress.find(id);
            return (it != m_progress.end()) ? &it->second : nullptr;
        }

        /// @brief Get all achievements paired with their progress
        [[nodiscard]] std::vector<std::pair<AchievementDefinition, AchievementProgress>> GetAllProgress() const
        {
            std::vector<std::pair<AchievementDefinition, AchievementProgress>> result;
            result.reserve(m_definitions.size());
            for (const auto& [id, def] : m_definitions)
            {
                auto progIt = m_progress.find(id);
                AchievementProgress prog;
                if (progIt != m_progress.end())
                {
                    prog = progIt->second;
                }
                result.emplace_back(def, prog);
            }
            return result;
        }

        /// @brief Get the number of unlocked achievements
        [[nodiscard]] uint32_t GetUnlockedCount() const
        {
            uint32_t count = 0;
            for (const auto& [id, prog] : m_progress)
            {
                if (prog.unlocked)
                {
                    ++count;
                }
            }
            return count;
        }

        /// @brief Get the total point value of all registered achievements
        [[nodiscard]] uint32_t GetTotalPoints() const
        {
            uint32_t total = 0;
            for (const auto& [id, def] : m_definitions)
            {
                total += def.pointValue;
            }
            return total;
        }

        /// @brief Get the total point value of unlocked achievements
        [[nodiscard]] uint32_t GetEarnedPoints() const
        {
            uint32_t earned = 0;
            for (const auto& [id, prog] : m_progress)
            {
                if (prog.unlocked)
                {
                    auto defIt = m_definitions.find(id);
                    if (defIt != m_definitions.end())
                    {
                        earned += defIt->second.pointValue;
                    }
                }
            }
            return earned;
        }

        // -----------------------------------------------------------------
        // Reset
        // -----------------------------------------------------------------

        /// @brief Reset progress for a single achievement
        void ResetProgress(uint32_t id)
        {
            auto it = m_progress.find(id);
            if (it != m_progress.end())
            {
                it->second.currentValue = 0.0f;
                it->second.unlocked = false;
                it->second.unlockTimestamp = 0;
                it->second.notified = false;
            }
        }

        /// @brief Reset all achievement progress (definitions remain)
        void ResetAll()
        {
            for (auto& [id, prog] : m_progress)
            {
                prog.currentValue = 0.0f;
                prog.unlocked = false;
                prog.unlockTimestamp = 0;
                prog.notified = false;
            }
        }

        // -----------------------------------------------------------------
        // Callbacks
        // -----------------------------------------------------------------

        /// @brief Register a callback invoked when any achievement is unlocked
        void OnAchievementUnlocked(std::function<void(const AchievementUnlockEvent&)> callback)
        {
            if (callback)
            {
                m_unlockCallbacks.push_back(std::move(callback));
            }
        }

        // -----------------------------------------------------------------
        // Serialization
        // -----------------------------------------------------------------

        /// @brief Serialize all progress data to a binary writer
        /// @param writer Target BinaryWriter (from Utils/Serializer.h)
        void SaveToWriter(Spark::BinaryWriter& writer) const
        {
            writer.Write<uint32_t>(static_cast<uint32_t>(m_progress.size()));
            for (const auto& [id, prog] : m_progress)
            {
                writer.Write<uint32_t>(prog.achievementId);
                writer.Write<float>(prog.currentValue);
                writer.Write<uint8_t>(prog.unlocked ? static_cast<uint8_t>(1) : static_cast<uint8_t>(0));
                writer.Write<uint64_t>(prog.unlockTimestamp);
            }
        }

        /// @brief Deserialize progress data from a binary reader
        /// @param reader Source BinaryReader (from Utils/Serializer.h)
        ///
        /// Only applies records to already-registered achievements; unknown ids are
        /// skipped rather than inserted. A torn/truncated record aborts the load.
        void LoadFromReader(Spark::BinaryReader& reader)
        {
            uint32_t count = reader.Read<uint32_t>();
            for (uint32_t i = 0; i < count; ++i)
            {
                uint32_t id = reader.Read<uint32_t>();
                float currentValue = reader.Read<float>();
                uint8_t unlocked = reader.Read<uint8_t>();
                uint64_t unlockTimestamp = reader.Read<uint64_t>();

                if (reader.HasError())
                {
                    break; // Truncated buffer — stop before applying a partial record.
                }

                // Only apply to achievements that are already registered.
                auto it = m_progress.find(id);
                if (it != m_progress.end())
                {
                    it->second.currentValue = currentValue;
                    it->second.unlocked = (unlocked != 0);
                    it->second.unlockTimestamp = unlockTimestamp;
                    // Mark notified so loading a save does not re-fire unlock callbacks.
                    it->second.notified = (unlocked != 0);
                }
            }
        }

        // -----------------------------------------------------------------
        // Console
        // -----------------------------------------------------------------

        /// @brief Get a human-readable status string for console debugging
        [[nodiscard]] std::string Console_GetStatus() const
        {
            std::ostringstream oss;
            oss << "[AchievementSystem] initialized=" << (m_initialized ? "true" : "false")
                << ", definitions=" << m_definitions.size() << ", unlocked=" << GetUnlockedCount() << "/"
                << m_definitions.size() << ", points=" << GetEarnedPoints() << "/" << GetTotalPoints();
            return oss.str();
        }

      private:
        AchievementSystem() = default;
        AchievementSystem(const AchievementSystem&) = delete;
        AchievementSystem& operator=(const AchievementSystem&) = delete;

        /// @brief Check if an achievement has met its target and auto-unlock
        void CheckUnlock(uint32_t id)
        {
            auto defIt = m_definitions.find(id);
            if (defIt == m_definitions.end())
            {
                return;
            }

            auto& prog = m_progress[id];
            if (prog.unlocked)
            {
                return;
            }

            if (prog.currentValue >= defIt->second.targetValue)
            {
                prog.unlocked = true;
                prog.unlockTimestamp = GetCurrentTimestamp();
                NotifyUnlock(id);
            }
        }

        /// @brief Fire unlock callbacks for an achievement
        void NotifyUnlock(uint32_t id)
        {
            auto defIt = m_definitions.find(id);
            if (defIt == m_definitions.end())
            {
                return;
            }

            auto& prog = m_progress[id];
            if (prog.notified)
            {
                return;
            }
            prog.notified = true;

            AchievementUnlockEvent event;
            event.achievementId = id;
            event.name = defIt->second.name;
            event.description = defIt->second.description;
            event.pointValue = defIt->second.pointValue;

            for (const auto& cb : m_unlockCallbacks)
            {
                cb(event);
            }
        }

        /// @brief Get current time as epoch seconds (portable fallback)
        [[nodiscard]] static uint64_t GetCurrentTimestamp()
        {
            auto now = std::chrono::system_clock::now();
            auto epoch = now.time_since_epoch();
            return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(epoch).count());
        }

        std::unordered_map<uint32_t, AchievementDefinition> m_definitions;
        std::unordered_map<uint32_t, AchievementProgress> m_progress;
        std::vector<std::function<void(const AchievementUnlockEvent&)>> m_unlockCallbacks;
        bool m_initialized = false;
    };

} // namespace Spark::Gameplay
