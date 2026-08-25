/**
 * @file OWPlayerSystem.cpp
 * @brief Player survival mechanics, fast travel, and compass
 */

#include "OWPlayerSystem.h"
#include "Utils/SparkConsole.h"
#include "Utils/LogMacros.h"

#ifdef ENABLE_EDITOR
#include <imgui.h>
#endif

#include <algorithm>
#include <cmath>
#include <sstream>
#include <unordered_set>

namespace OpenWorld
{

    bool OWPlayerSystem::Initialize(Spark::IEngineContext* context)
    {
        m_context = context;

        // Start in the Emerald Meadows center
        m_worldState.posX = 0.0f;
        m_worldState.posY = 5.0f;
        m_worldState.posZ = 0.0f;
        m_worldState.yaw = 0.0f;
        m_worldState.currentRegionId = 1;

        // Full survival stats
        m_survival = SurvivalState{};

        // Register starting fast travel point
        UnlockFastTravel({1, "Meadow Campfire", 0.0f, 5.0f, 0.0f, 1});

        m_initialized = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "Open world player system initialized");
        Spark::SimpleConsole::GetInstance().LogInfo("[OpenWorld] Player system initialized");
        return true;
    }

    void OWPlayerSystem::Update(float deltaTime)
    {
        if (!m_initialized)
            return;

        m_survivalTickTimer += deltaTime;
        if (m_survivalTickTimer >= 1.0f)
        {
            UpdateSurvival(m_survivalTickTimer);
            m_survivalTickTimer = 0.0f;
        }
    }

    void OWPlayerSystem::FixedUpdate(float fixedDeltaTime)
    {
        if (!m_initialized)
            return;

        // Stamina recovery/drain
        if (m_worldState.isSprinting)
        {
            m_survival.stamina = std::max(0.0f, m_survival.stamina - 15.0f * fixedDeltaTime);
            if (m_survival.stamina <= 0.0f)
                m_worldState.isSprinting = false;
        }
        else
        {
            m_survival.stamina = std::min(m_survival.maxStamina, m_survival.stamina + 8.0f * fixedDeltaTime);
        }
    }

    void OWPlayerSystem::Shutdown()
    {
        if (!m_initialized)
            return;

        m_fastTravelPoints.clear();
        m_unlockedPointIds.clear();
        m_initialized = false;
    }

    void OWPlayerSystem::UpdateSurvival(float deltaTime)
    {
        // Hunger and thirst decrease over time
        constexpr float hungerRate = 0.5f; // per second
        constexpr float thirstRate = 0.7f;

        m_survival.hunger = std::max(0.0f, m_survival.hunger - hungerRate * deltaTime);
        m_survival.thirst = std::max(0.0f, m_survival.thirst - thirstRate * deltaTime);

        UpdateTemperature(deltaTime);
        ApplySurvivalEffects(deltaTime);
    }

    // Intentional: deltaTime reserved for future time-scaled temperature changes
    void OWPlayerSystem::UpdateTemperature([[maybe_unused]] float deltaTime)
    {
        // Temperature is influenced by biome base temp and clothing warmth
        // This is a simplified model — real implementation would query the
        // world setup for biome temperature at current position
        float targetTemp = 20.0f; // Default temperate
        float adjustment = (m_survival.warmth - 0.5f) * 10.0f;
        targetTemp += adjustment;

        // Lerp toward target
        float diff = targetTemp - m_survival.temperature;
        m_survival.temperature += diff * 0.1f;
    }

    void OWPlayerSystem::ApplySurvivalEffects(float deltaTime)
    {
        // Starvation damage
        if (m_survival.hunger <= 0.0f)
            m_survival.health = std::max(0.0f, m_survival.health - 2.0f * deltaTime);

        // Dehydration damage
        if (m_survival.thirst <= 0.0f)
            m_survival.health = std::max(0.0f, m_survival.health - 3.0f * deltaTime);

        // Temperature extremes
        if (m_survival.temperature < 0.0f)
            m_survival.health = std::max(0.0f, m_survival.health - 1.5f * deltaTime);
        else if (m_survival.temperature > 45.0f)
            m_survival.health = std::max(0.0f, m_survival.health - 1.5f * deltaTime);

        // Passive health regen when well-fed and hydrated
        if (m_survival.hunger > 50.0f && m_survival.thirst > 50.0f)
            m_survival.health = std::min(m_survival.maxHealth, m_survival.health + 1.0f * deltaTime);
    }

    void OWPlayerSystem::Eat(float nutritionValue)
    {
        m_survival.hunger = std::min(100.0f, m_survival.hunger + nutritionValue);
    }

    void OWPlayerSystem::Drink(float hydrationValue)
    {
        m_survival.thirst = std::min(100.0f, m_survival.thirst + hydrationValue);
    }

    void OWPlayerSystem::TakeDamage(float amount)
    {
        m_survival.health = std::max(0.0f, m_survival.health - amount);
    }

    void OWPlayerSystem::Heal(float amount)
    {
        m_survival.health = std::min(m_survival.maxHealth, m_survival.health + amount);
    }

    void OWPlayerSystem::SetPosition(float x, float y, float z)
    {
        m_worldState.posX = x;
        m_worldState.posY = y;
        m_worldState.posZ = z;
    }

    void OWPlayerSystem::SetFacing(float yaw)
    {
        m_worldState.yaw = std::fmod(yaw, 360.0f);
        if (m_worldState.yaw < 0.0f)
            m_worldState.yaw += 360.0f;
    }

    std::string OWPlayerSystem::GetCompassDirection() const
    {
        float y = m_worldState.yaw;
        if (y >= 337.5f || y < 22.5f)
            return "N";
        if (y < 67.5f)
            return "NE";
        if (y < 112.5f)
            return "E";
        if (y < 157.5f)
            return "SE";
        if (y < 202.5f)
            return "S";
        if (y < 247.5f)
            return "SW";
        if (y < 292.5f)
            return "W";
        return "NW";
    }

    void OWPlayerSystem::UnlockFastTravel(const FastTravelPoint& point)
    {
        if (m_unlockedPointIds.count(point.pointId))
            return;

        m_fastTravelPoints.push_back(point);
        m_unlockedPointIds.insert(point.pointId);

        Spark::SimpleConsole::GetInstance().LogInfo("[OpenWorld] Fast travel unlocked: " + point.name);
    }

    bool OWPlayerSystem::FastTravelTo(uint32_t pointId)
    {
        for (const auto& pt : m_fastTravelPoints)
        {
            if (pt.pointId == pointId)
            {
                SetPosition(pt.x, pt.y, pt.z);
                m_worldState.currentRegionId = pt.regionId;
                Spark::SimpleConsole::GetInstance().LogInfo("[OpenWorld] Fast traveled to " + pt.name);
                return true;
            }
        }
        return false;
    }

    PlayerSaveState OWPlayerSystem::CaptureSaveState() const
    {
        return {m_survival, m_worldState, m_fastTravelPoints, m_survivalTickTimer};
    }

    bool OWPlayerSystem::RestoreSaveState(const PlayerSaveState& state, std::string* error)
    {
        auto fail = [&](const char* message)
        {
            if (error)
                *error = message;
            return false;
        };
        auto finite = [](float value) { return std::isfinite(value); };

        const auto& s = state.survival;
        if (!finite(s.health) || !finite(s.maxHealth) || !finite(s.stamina) || !finite(s.maxStamina) ||
            !finite(s.hunger) || !finite(s.thirst) || !finite(s.temperature) || !finite(s.warmth) ||
            s.maxHealth <= 0.0f || s.maxHealth > 100000.0f || s.health < 0.0f || s.health > s.maxHealth ||
            s.maxStamina <= 0.0f || s.maxStamina > 100000.0f || s.stamina < 0.0f || s.stamina > s.maxStamina ||
            s.hunger < 0.0f || s.hunger > 100.0f || s.thirst < 0.0f || s.thirst > 100.0f || s.temperature < -100.0f ||
            s.temperature > 100.0f || s.warmth < 0.0f || s.warmth > 1.0f)
            return fail("invalid player survival state");

        const auto& world = state.world;
        if (!finite(world.posX) || !finite(world.posY) || !finite(world.posZ) || !finite(world.yaw) ||
            !finite(world.speed) || std::abs(world.posX) > 100000000.0f || std::abs(world.posY) > 100000000.0f ||
            std::abs(world.posZ) > 100000000.0f || world.yaw < 0.0f || world.yaw >= 360.0f || world.speed < 0.0f ||
            world.speed > 10000.0f || world.currentRegionId > 8)
            return fail("invalid player world state");
        if (!finite(state.survivalTickTimer) || state.survivalTickTimer < 0.0f || state.survivalTickTimer > 60.0f)
            return fail("invalid survival tick timer");
        if (state.fastTravelPoints.size() > 256)
            return fail("too many fast travel points");

        std::unordered_set<uint32_t> ids;
        bool hasStartingPoint = false;
        for (const auto& point : state.fastTravelPoints)
        {
            if (point.pointId == 0 || !ids.insert(point.pointId).second || point.name.empty() ||
                point.name.size() > 128 || !finite(point.x) || !finite(point.y) || !finite(point.z) ||
                point.regionId == 0 || point.regionId > 8)
                return fail("invalid fast travel point");
            hasStartingPoint = hasStartingPoint || point.pointId == 1;
        }
        if (!hasStartingPoint)
            return fail("starting fast travel point is missing");

        m_survival = state.survival;
        m_worldState = state.world;
        SetFacing(state.world.yaw);
        m_fastTravelPoints = state.fastTravelPoints;
        m_unlockedPointIds = std::move(ids);
        m_survivalTickTimer = state.survivalTickTimer;
        return true;
    }

    std::string OWPlayerSystem::GetStatusString() const
    {
        std::ostringstream ss;
        ss << "=== Player Status ===\n";
        ss << "Position: (" << m_worldState.posX << ", " << m_worldState.posY << ", " << m_worldState.posZ << ")\n";
        ss << "Facing: " << GetCompassDirection() << " (" << m_worldState.yaw << " deg)\n";
        ss << "Region: " << m_worldState.currentRegionId << "\n";
        ss << "Health: " << m_survival.health << "/" << m_survival.maxHealth << "\n";
        ss << "Stamina: " << m_survival.stamina << "/" << m_survival.maxStamina << "\n";
        ss << "Hunger: " << m_survival.hunger << "/100\n";
        ss << "Thirst: " << m_survival.thirst << "/100\n";
        ss << "Temperature: " << m_survival.temperature << "C\n";
        ss << "Fast Travel Points: " << m_fastTravelPoints.size() << "\n";
        return ss.str();
    }

    void OWPlayerSystem::RenderDebugUI()
    {
#ifdef ENABLE_EDITOR
        if (!ImGui::CollapsingHeader("Open World Player"))
            return;

        ImGui::Text("Position: (%.1f, %.1f, %.1f)", m_worldState.posX, m_worldState.posY, m_worldState.posZ);
        ImGui::Text("Facing: %s (%.0f deg)", GetCompassDirection().c_str(), m_worldState.yaw);
        ImGui::Text("Region: %u", m_worldState.currentRegionId);
        ImGui::Separator();

        ImGui::ProgressBar(m_survival.health / m_survival.maxHealth, ImVec2(-1, 0), "Health");
        ImGui::ProgressBar(m_survival.stamina / m_survival.maxStamina, ImVec2(-1, 0), "Stamina");
        ImGui::ProgressBar(m_survival.hunger / 100.0f, ImVec2(-1, 0), "Hunger");
        ImGui::ProgressBar(m_survival.thirst / 100.0f, ImVec2(-1, 0), "Thirst");
        ImGui::Text("Temperature: %.1fC | Warmth: %.1f", m_survival.temperature, m_survival.warmth);

        if (!m_fastTravelPoints.empty() && ImGui::TreeNode("Fast Travel"))
        {
            for (const auto& pt : m_fastTravelPoints)
            {
                ImGui::Text("[%u] %s (%.0f, %.0f, %.0f)", pt.pointId, pt.name.c_str(), pt.x, pt.y, pt.z);
            }
            ImGui::TreePop();
        }
#endif
    }

} // namespace OpenWorld
