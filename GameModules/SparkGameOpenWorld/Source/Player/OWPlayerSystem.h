/**
 * @file OWPlayerSystem.h
 * @brief Open world player: survival stats, stamina, fast travel, compass
 * @author Spark Engine Team
 * @date 2026
 *
 * Manages the player character in an open world context: survival mechanics
 * (health, stamina, hunger, thirst, temperature), fast travel between
 * discovered points, compass/bearing data, and player state persistence.
 */

#pragma once

#include "Spark/IEngineContext.h"
#include "Enums/OpenWorldEnums.h"

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace OpenWorld
{

    /// @brief Player survival state
    struct SurvivalState
    {
        float health = 100.0f;
        float maxHealth = 100.0f;
        float stamina = 100.0f;
        float maxStamina = 100.0f;
        float hunger = 100.0f;     ///< 100 = full, 0 = starving
        float thirst = 100.0f;     ///< 100 = hydrated, 0 = dehydrated
        float temperature = 20.0f; ///< Current body temperature (Celsius)
        float warmth = 0.5f;       ///< Clothing/shelter warmth factor (0-1)
    };

    /// @brief Player world position and movement state
    struct PlayerWorldState
    {
        float posX = 0.0f;
        float posY = 0.0f;
        float posZ = 0.0f;
        float yaw = 0.0f; ///< Facing direction in degrees (0 = north)
        float speed = 0.0f;
        bool isSprinting = false;
        bool isSwimming = false;
        bool isClimbing = false;
        bool isInShelter = false;
        uint32_t currentRegionId = 0;
    };

    /// @brief A fast travel destination the player has unlocked
    struct FastTravelPoint
    {
        uint32_t pointId = 0;
        std::string name;
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        uint32_t regionId = 0;
    };

    /**
     * @brief Player survival, movement, fast travel, and compass system
     */
    class OWPlayerSystem
    {
      public:
        OWPlayerSystem() = default;
        ~OWPlayerSystem() = default;

        bool Initialize(Spark::IEngineContext* context);
        void Update(float deltaTime);
        void FixedUpdate(float fixedDeltaTime);
        void Shutdown();
        void RenderDebugUI();

        // --- Survival ---
        const SurvivalState& GetSurvivalState() const { return m_survival; }
        void Eat(float nutritionValue);
        void Drink(float hydrationValue);
        void TakeDamage(float amount);
        void Heal(float amount);
        bool IsAlive() const { return m_survival.health > 0.0f; }

        // --- Position / Movement ---
        const PlayerWorldState& GetWorldState() const { return m_worldState; }
        void SetPosition(float x, float y, float z);
        void SetCurrentRegion(uint32_t regionId) { m_worldState.currentRegionId = regionId; }
        void SetFacing(float yaw);
        float GetCompassBearing() const { return m_worldState.yaw; }
        std::string GetCompassDirection() const;

        // --- Fast Travel ---
        void UnlockFastTravel(const FastTravelPoint& point);
        bool FastTravelTo(uint32_t pointId);
        const std::vector<FastTravelPoint>& GetFastTravelPoints() const { return m_fastTravelPoints; }
        size_t GetFastTravelCount() const { return m_fastTravelPoints.size(); }

        std::string GetStatusString() const;

      private:
        void UpdateSurvival(float deltaTime);
        void UpdateTemperature(float deltaTime);
        void ApplySurvivalEffects(float deltaTime);

        Spark::IEngineContext* m_context{nullptr};
        SurvivalState m_survival;
        PlayerWorldState m_worldState;
        std::vector<FastTravelPoint> m_fastTravelPoints;
        std::unordered_set<uint32_t> m_unlockedPointIds;
        float m_survivalTickTimer{0.0f};
        bool m_initialized{false};
    };

} // namespace OpenWorld
