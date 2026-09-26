/**
 * @file PlatformerCheckpointSystem.cpp
 * @brief Checkpoint activation, respawn tracking, and flag animation
 */

#include "PlatformerCheckpointSystem.h"
#include "Utils/SparkConsole.h"
#include "Utils/LogMacros.h"

#ifdef ENABLE_EDITOR
#include <imgui.h>
#endif

#include <algorithm>
#include <cmath>

namespace Platformer
{

    bool PlatformerCheckpointSystem::Initialize(Spark::IEngineContext* context)
    {
        m_context = context;

        BuildDemoCheckpoints();

        m_initialized = true;

        auto& console = Spark::SimpleConsole::GetInstance();
        SPARK_LOG_INFO(Spark::LogCategory::Game, "Platformer checkpoint system initialized with %zu checkpoints",
                       m_checkpoints.size());
        console.LogInfo("[Platformer Checkpoint] System initialized with " + std::to_string(m_checkpoints.size()) +
                        " checkpoints");
        return true;
    }

    void PlatformerCheckpointSystem::BuildDemoCheckpoints()
    {
        // Level 0 (Green Hills) checkpoints
        CheckpointData cp{};

        // Checkpoint after the stepping stones
        cp.id = m_nextId++;
        cp.levelIndex = 0;
        cp.posX = 25.0f;
        cp.posY = 5.0f;
        cp.posZ = 0.0f;
        m_checkpoints.push_back(cp);

        // Checkpoint after the moving platform section
        cp.id = m_nextId++;
        cp.levelIndex = 0;
        cp.posX = 55.0f;
        cp.posY = 13.0f;
        cp.posZ = 0.0f;
        m_checkpoints.push_back(cp);

        // Checkpoint near the goal
        cp.id = m_nextId++;
        cp.levelIndex = 0;
        cp.posX = 90.0f;
        cp.posY = 6.0f;
        cp.posZ = 0.0f;
        m_checkpoints.push_back(cp);

        // Level 1 (Scorching Sands) checkpoints. Every checkpoint must sit above a platform: a respawn
        // over empty space falls straight through the kill plane again.
        // On the rest ledge after the conveyor (x 25..31, top 1)
        cp.id = m_nextId++;
        cp.levelIndex = 1;
        cp.posX = 29.0f;
        cp.posY = 2.0f;
        cp.posZ = 0.0f;
        m_checkpoints.push_back(cp);

        // Over the rotating platform's pivot, which its collider covers at every angle
        cp.id = m_nextId++;
        cp.levelIndex = 1;
        cp.posX = 65.0f;
        cp.posY = 7.0f;
        cp.posZ = 0.0f;
        m_checkpoints.push_back(cp);

        // Level 2 (Cloud Kingdom) checkpoints
        cp.id = m_nextId++;
        cp.levelIndex = 2;
        cp.posX = 36.0f;
        cp.posY = 64.0f;
        cp.posZ = 0.0f;
        m_checkpoints.push_back(cp);
    }

    void PlatformerCheckpointSystem::CheckActivation(float playerX, float playerY, float playerZ)
    {
        for (auto& cp : m_checkpoints)
        {
            if (cp.levelIndex != m_activeLevel || cp.activated)
                continue;

            float dx = cp.posX - playerX;
            float dy = cp.posY - playerY;
            float dz = cp.posZ - playerZ;
            float distSq = dx * dx + dy * dy + dz * dz;

            if (distSq <= cp.activationRadius * cp.activationRadius)
            {
                cp.activated = true;
                cp.animationTimer = 0.0f;
                m_lastActivatedId = cp.id;

                auto& console = Spark::SimpleConsole::GetInstance();
                SPARK_LOG_INFO(Spark::LogCategory::Game, "Platformer checkpoint %u activated at (%.0f, %.0f)", cp.id,
                               cp.posX, cp.posY);
                console.LogInfo("[Platformer Checkpoint] Checkpoint " + std::to_string(cp.id) + " activated at (" +
                                std::to_string(static_cast<int>(cp.posX)) + ", " +
                                std::to_string(static_cast<int>(cp.posY)) + ")");
            }
        }
    }

    PlayerPosition PlatformerCheckpointSystem::GetLastCheckpointPosition() const
    {
        for (const auto& cp : m_checkpoints)
        {
            if (cp.levelIndex == m_activeLevel && cp.id == m_lastActivatedId)
                return {cp.posX, cp.posY, cp.posZ};
        }

        // No checkpoint activated yet — use level spawn
        return m_levelSpawn;
    }

    size_t PlatformerCheckpointSystem::GetActivatedCount() const
    {
        size_t count = 0;
        for (const auto& cp : m_checkpoints)
        {
            if (cp.activated)
                ++count;
        }
        return count;
    }

    CheckpointProgress PlatformerCheckpointSystem::CaptureProgress() const
    {
        CheckpointProgress progress;
        for (const auto& cp : m_checkpoints)
        {
            if (cp.activated)
                progress.activatedIds.push_back(cp.id);
        }
        std::ranges::sort(progress.activatedIds);
        progress.lastActivatedId = m_lastActivatedId;
        return progress;
    }

    bool PlatformerCheckpointSystem::RestoreProgress(const CheckpointProgress& progress)
    {
        const auto isPlaced = [this](uint32_t id)
        { return std::ranges::any_of(m_checkpoints, [id](const CheckpointData& cp) { return cp.id == id; }); };
        const auto isActivated = [&progress](uint32_t id)
        { return std::ranges::find(progress.activatedIds, id) != progress.activatedIds.end(); };

        if (!std::ranges::all_of(progress.activatedIds, isPlaced))
            return false;
        if (progress.lastActivatedId != 0 && !isActivated(progress.lastActivatedId))
            return false;

        for (auto& cp : m_checkpoints)
        {
            cp.activated = isActivated(cp.id);
            cp.animationTimer = cp.activated ? 1.0f : 0.0f; // Restored flags are already fully raised
        }
        m_lastActivatedId = progress.lastActivatedId;
        return true;
    }

    void PlatformerCheckpointSystem::ResetLevel(uint32_t levelIndex)
    {
        for (auto& cp : m_checkpoints)
        {
            if (cp.levelIndex == levelIndex)
            {
                cp.activated = false;
                cp.animationTimer = 0.0f;
            }
        }
        m_lastActivatedId = 0;
        m_activeLevel = levelIndex;
    }

    void PlatformerCheckpointSystem::SetLevelSpawn(float x, float y, float z)
    {
        m_levelSpawn = {x, y, z};
    }

    void PlatformerCheckpointSystem::Update(float deltaTime)
    {
        if (!m_initialized)
            return;

        // Update flag raise animation for recently activated checkpoints
        for (auto& cp : m_checkpoints)
        {
            if (cp.activated && cp.animationTimer < 1.0f)
            {
                cp.animationTimer += deltaTime * 2.0f; // 0.5s flag raise
                if (cp.animationTimer > 1.0f)
                    cp.animationTimer = 1.0f;
            }
        }
    }

    void PlatformerCheckpointSystem::Shutdown()
    {
        m_checkpoints.clear();
        m_initialized = false;
    }

    void PlatformerCheckpointSystem::RenderDebugUI()
    {
#ifdef ENABLE_EDITOR
        if (!ImGui::CollapsingHeader("Platformer Checkpoints"))
            return;

        ImGui::Text("Checkpoints: %zu", m_checkpoints.size());
        ImGui::Text("Activated: %zu", GetActivatedCount());
        ImGui::Text("Last Active ID: %u", m_lastActivatedId);
        ImGui::Separator();

        for (const auto& cp : m_checkpoints)
        {
            ImGui::PushID(static_cast<int>(cp.id));
            std::string label = "CP " + std::to_string(cp.id) + " (Level " + std::to_string(cp.levelIndex) + ")";
            if (cp.activated)
                label += " [ACTIVE]";

            if (ImGui::TreeNode(label.c_str()))
            {
                ImGui::Text("Position: (%.1f, %.1f, %.1f)", cp.posX, cp.posY, cp.posZ);
                ImGui::Text("Activated: %s", cp.activated ? "Yes" : "No");
                if (cp.activated)
                    ImGui::Text("Flag Animation: %.0f%%", cp.animationTimer * 100.0f);
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
#endif
    }

} // namespace Platformer
