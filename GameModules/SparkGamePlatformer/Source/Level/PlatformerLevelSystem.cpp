/**
 * @file PlatformerLevelSystem.cpp
 * @brief Level layout, progression, star ratings, and speedrun timer
 */

#include "PlatformerLevelSystem.h"
#include "Utils/LogMacros.h"
#ifdef ENABLE_EDITOR
#include <imgui.h>
#endif

namespace Platformer
{

    bool PlatformerLevelSystem::Initialize(Spark::IEngineContext* context)
    {
        if (!context)
            return false;

        m_context = context;

        BuildLevelDefinitions();

        // Initialize progress tracking for each level
        m_progress.resize(m_levels.size());
        if (!m_progress.empty())
            m_progress[0].unlocked = true; // First level always unlocked

        m_initialized = true;

        SPARK_LOG_INFO(Spark::LogCategory::Game, "[Platformer Level] Level system initialized with %s levels",
                       std::to_string(m_levels.size()).c_str());
        return true;
    }

    void PlatformerLevelSystem::BuildLevelDefinitions()
    {
        BuildGrasslandsLevel();
        BuildDesertLevel();
        BuildSkyLevel();
    }

    void PlatformerLevelSystem::BuildGrasslandsLevel()
    {
        LevelDef level{};
        level.id = 0;
        level.name = "Green Hills";
        level.theme = LevelTheme::Grasslands;
        level.spawnPoint = {0.0f, 1.0f, 0.0f};
        level.goalPoint = {100.0f, 5.0f, 0.0f};
        level.requiredStarsToUnlock = 0;
        level.hasSecretArea = true;
        level.starThresholds = {120.0f, 90.0f, 60.0f, 0};

        // Starting platform
        level.platforms.push_back({PlatformType::Static, 0.0f, 0.0f, 0.0f, 8.0f, 0.5f, 4.0f});

        // Stepping stones
        level.platforms.push_back({PlatformType::Static, 12.0f, 2.0f, 0.0f, 3.0f, 0.5f, 3.0f});
        level.platforms.push_back({PlatformType::Static, 20.0f, 4.0f, 0.0f, 3.0f, 0.5f, 3.0f});

        // Moving platform section
        PlatformDef moving{};
        moving.type = PlatformType::Moving;
        moving.posX = 28.0f;
        moving.posY = 4.0f;
        moving.posZ = 0.0f;
        moving.endX = 40.0f;
        moving.endY = 8.0f;
        moving.endZ = 0.0f;
        moving.moveSpeed = 2.0f;
        moving.width = 4.0f;
        moving.height = 0.5f;
        moving.depth = 3.0f;
        level.platforms.push_back(moving);

        // Bouncy platform
        PlatformDef bouncy{};
        bouncy.type = PlatformType::Bouncy;
        bouncy.posX = 48.0f;
        bouncy.posY = 2.0f;
        bouncy.posZ = 0.0f;
        bouncy.bounceForce = 18.0f;
        bouncy.width = 3.0f;
        bouncy.height = 0.5f;
        bouncy.depth = 3.0f;
        level.platforms.push_back(bouncy);

        // High platform after bounce
        level.platforms.push_back({PlatformType::Static, 55.0f, 12.0f, 0.0f, 6.0f, 0.5f, 4.0f});

        // Falling platforms
        PlatformDef falling{};
        falling.type = PlatformType::Falling;
        falling.posX = 65.0f;
        falling.posY = 12.0f;
        falling.posZ = 0.0f;
        falling.fallDelay = 0.5f;
        falling.width = 3.0f;
        falling.height = 0.5f;
        falling.depth = 3.0f;
        level.platforms.push_back(falling);

        falling.posX = 72.0f;
        level.platforms.push_back(falling);

        // Goal platform
        level.platforms.push_back({PlatformType::Static, 100.0f, 5.0f, 0.0f, 6.0f, 0.5f, 6.0f});

        m_levels.push_back(std::move(level));
    }

    void PlatformerLevelSystem::BuildDesertLevel()
    {
        LevelDef level{};
        level.id = 1;
        level.name = "Scorching Sands";
        level.theme = LevelTheme::Desert;
        level.spawnPoint = {0.0f, 1.0f, 0.0f};
        level.goalPoint = {120.0f, 10.0f, 0.0f};
        level.requiredStarsToUnlock = 1;
        level.hasSecretArea = true;
        level.starThresholds = {150.0f, 110.0f, 75.0f, 0};

        // Start
        level.platforms.push_back({PlatformType::Static, 0.0f, 0.0f, 0.0f, 8.0f, 0.5f, 4.0f});

        // Conveyor belt section
        PlatformDef conveyor{};
        conveyor.type = PlatformType::Conveyor;
        conveyor.posX = 15.0f;
        conveyor.posY = 0.0f;
        conveyor.posZ = 0.0f;
        conveyor.conveyorDirX = -1.0f;
        conveyor.conveyorSpeed = 4.0f;
        conveyor.width = 12.0f;
        conveyor.height = 0.5f;
        conveyor.depth = 4.0f;
        level.platforms.push_back(conveyor);

        // Disappearing platforms over lava gap
        PlatformDef disappearing{};
        disappearing.type = PlatformType::Disappearing;
        disappearing.posY = 3.0f;
        disappearing.posZ = 0.0f;
        disappearing.visibleTime = 2.0f;
        disappearing.hiddenTime = 1.5f;
        disappearing.width = 3.0f;
        disappearing.height = 0.5f;
        disappearing.depth = 3.0f;

        disappearing.posX = 35.0f;
        level.platforms.push_back(disappearing);
        disappearing.posX = 42.0f;
        level.platforms.push_back(disappearing);
        disappearing.posX = 49.0f;
        level.platforms.push_back(disappearing);

        // Rotating platform
        PlatformDef rotating{};
        rotating.type = PlatformType::Rotating;
        rotating.posX = 65.0f;
        rotating.posY = 6.0f;
        rotating.posZ = 0.0f;
        rotating.width = 8.0f;
        rotating.height = 0.5f;
        rotating.depth = 2.0f;
        level.platforms.push_back(rotating);

        // Vertical moving platform to goal
        PlatformDef elevator{};
        elevator.type = PlatformType::Moving;
        elevator.posX = 100.0f;
        elevator.posY = 0.0f;
        elevator.posZ = 0.0f;
        elevator.endX = 100.0f;
        elevator.endY = 10.0f;
        elevator.endZ = 0.0f;
        elevator.moveSpeed = 1.5f;
        elevator.width = 4.0f;
        elevator.height = 0.5f;
        elevator.depth = 4.0f;
        level.platforms.push_back(elevator);

        // Goal
        level.platforms.push_back({PlatformType::Static, 120.0f, 10.0f, 0.0f, 6.0f, 0.5f, 6.0f});

        m_levels.push_back(std::move(level));
    }

    void PlatformerLevelSystem::BuildSkyLevel()
    {
        LevelDef level{};
        level.id = 2;
        level.name = "Cloud Kingdom";
        level.theme = LevelTheme::Sky;
        level.spawnPoint = {0.0f, 50.0f, 0.0f};
        level.goalPoint = {80.0f, 80.0f, 0.0f};
        level.requiredStarsToUnlock = 3;
        level.hasSecretArea = false;
        level.starThresholds = {180.0f, 130.0f, 90.0f, 0};

        // Starting cloud
        level.platforms.push_back({PlatformType::Static, 0.0f, 50.0f, 0.0f, 6.0f, 0.5f, 6.0f});

        // Ascending bouncy chain
        PlatformDef bouncy{};
        bouncy.type = PlatformType::Bouncy;
        bouncy.width = 3.0f;
        bouncy.height = 0.5f;
        bouncy.depth = 3.0f;
        bouncy.bounceForce = 20.0f;

        bouncy.posX = 12.0f;
        bouncy.posY = 48.0f;
        bouncy.posZ = 0.0f;
        level.platforms.push_back(bouncy);

        bouncy.posX = 24.0f;
        bouncy.posY = 55.0f;
        level.platforms.push_back(bouncy);

        bouncy.posX = 36.0f;
        bouncy.posY = 62.0f;
        level.platforms.push_back(bouncy);

        // Moving cloud platform
        PlatformDef cloud{};
        cloud.type = PlatformType::Moving;
        cloud.posX = 45.0f;
        cloud.posY = 70.0f;
        cloud.posZ = 0.0f;
        cloud.endX = 65.0f;
        cloud.endY = 70.0f;
        cloud.endZ = 0.0f;
        cloud.moveSpeed = 3.0f;
        cloud.width = 5.0f;
        cloud.height = 0.5f;
        cloud.depth = 4.0f;
        level.platforms.push_back(cloud);

        // Goal cloud
        level.platforms.push_back({PlatformType::Static, 80.0f, 80.0f, 0.0f, 8.0f, 0.5f, 8.0f});

        m_levels.push_back(std::move(level));
    }

    bool PlatformerLevelSystem::LoadLevel(uint32_t index)
    {
        if (index >= m_levels.size())
            return false;

        if (index < m_progress.size() && !m_progress[index].unlocked)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Game, "[Platformer Level] Level %s is locked (need %s stars)",
                           std::to_string(index).c_str(),
                           std::to_string(m_levels[index].requiredStarsToUnlock).c_str());
            return false;
        }

        m_currentLevel = index;
        m_levelTimer = 0.0f;
        m_levelDeaths = 0;
        m_levelActive = true;

        SPARK_LOG_INFO(Spark::LogCategory::Game, "[Platformer Level] Loaded level %s: %s",
                       std::to_string(index).c_str(), m_levels[index].name.c_str());
        return true;
    }

    SpawnPoint PlatformerLevelSystem::GetCurrentSpawnPoint() const
    {
        if (m_currentLevel < m_levels.size())
            return m_levels[m_currentLevel].spawnPoint;
        return {0.0f, 1.0f, 0.0f};
    }

    int PlatformerLevelSystem::GetTotalStarsEarned() const
    {
        int total = 0;
        for (const auto& prog : m_progress)
            total += prog.starsEarned;
        return total;
    }

    void PlatformerLevelSystem::CompleteLevel(float completionTime, int deaths)
    {
        if (m_currentLevel >= m_levels.size())
            return;

        auto& prog = m_progress[m_currentLevel];
        prog.completed = true;

        int stars = CalculateStars(m_currentLevel, completionTime, deaths);

        // Only update if better than previous attempt
        if (stars > prog.starsEarned)
            prog.starsEarned = stars;
        if (prog.bestTime <= 0.0f || completionTime < prog.bestTime)
            prog.bestTime = completionTime;
        if (prog.bestDeaths < 0 || deaths < prog.bestDeaths)
            prog.bestDeaths = deaths;

        // Unlock next level if star requirements met
        int totalStars = GetTotalStarsEarned();
        for (size_t i = 0; i < m_levels.size(); ++i)
        {
            if (static_cast<int>(m_levels[i].requiredStarsToUnlock) <= totalStars)
                m_progress[i].unlocked = true;
        }

        m_levelActive = false;

        SPARK_LOG_INFO(Spark::LogCategory::Game, "[Platformer Level] Level completed! Stars: %s Time: %ss Deaths: %s",
                       std::to_string(stars).c_str(), std::to_string(completionTime).c_str(),
                       std::to_string(deaths).c_str());
    }

    int PlatformerLevelSystem::CalculateStars(uint32_t levelIndex, float time, int deaths) const
    {
        if (levelIndex >= m_levels.size())
            return 0;

        const auto& thresholds = m_levels[levelIndex].starThresholds;
        int stars = 0;

        if (time <= thresholds.timeStar3 && deaths <= thresholds.maxDeathsStar)
            stars = 3;
        else if (time <= thresholds.timeStar2)
            stars = 2;
        else if (time <= thresholds.timeStar1)
            stars = 1;

        return stars;
    }

    void PlatformerLevelSystem::Update(float deltaTime)
    {
        if (!m_initialized || !m_levelActive)
            return;

        m_levelTimer += deltaTime;
    }

    void PlatformerLevelSystem::Render()
    {
        if (!m_initialized)
            return;

        // In a full implementation, this would render the level geometry:
        // - Platform meshes with theme-appropriate materials
        // - Background/skybox matching the level theme
        // - Animated elements (moving platforms, conveyors, rotating platforms)
        // - Goal marker VFX
    }

    void PlatformerLevelSystem::Shutdown()
    {
        m_levels.clear();
        m_progress.clear();
        m_levelActive = false;
        m_initialized = false;
    }

    std::string PlatformerLevelSystem::GetLevelListString() const
    {
        std::string list = "=== Platformer Levels ===\n";
        for (size_t i = 0; i < m_levels.size(); ++i)
        {
            const auto& level = m_levels[i];
            const auto& prog = m_progress[i];

            list += "[" + std::to_string(i) + "] " + level.name;
            list += " (" + std::to_string(level.platforms.size()) + " platforms)";

            if (!prog.unlocked)
            {
                list += " [LOCKED - need " + std::to_string(level.requiredStarsToUnlock) + " stars]";
            }
            else if (prog.completed)
            {
                list += " [DONE] Stars: " + std::to_string(prog.starsEarned) + "/3";
                list += " Best: " + std::to_string(static_cast<int>(prog.bestTime)) + "s";
            }
            else
            {
                list += " [Available]";
            }

            if (i == m_currentLevel && m_levelActive)
                list += " *ACTIVE*";
            list += "\n";
        }
        list += "Total stars: " + std::to_string(GetTotalStarsEarned()) + "\n";
        return list;
    }

    void PlatformerLevelSystem::RenderDebugUI()
    {
#ifdef ENABLE_EDITOR
        if (!ImGui::CollapsingHeader("Platformer Levels"))
            return;

        ImGui::Text("Levels: %zu", m_levels.size());
        ImGui::Text("Total Stars: %d", GetTotalStarsEarned());
        ImGui::Text("Current Level: %u", m_currentLevel);
        if (m_levelActive)
            ImGui::Text("Timer: %.1fs", m_levelTimer);
        ImGui::Separator();

        for (size_t i = 0; i < m_levels.size(); ++i)
        {
            const auto& level = m_levels[i];
            const auto& prog = m_progress[i];

            ImGui::PushID(static_cast<int>(i));
            std::string label = level.name;
            if (!prog.unlocked)
                label += " [LOCKED]";

            if (ImGui::TreeNode(label.c_str()))
            {
                ImGui::Text("Platforms: %zu", level.platforms.size());
                ImGui::Text("Stars: %d / 3", prog.starsEarned);
                ImGui::Text("Best Time: %.1fs", prog.bestTime);
                ImGui::Text("Secret: %s", level.hasSecretArea ? (prog.secretFound ? "Found" : "Hidden") : "None");

                if (prog.unlocked && ImGui::Button("Load"))
                    LoadLevel(static_cast<uint32_t>(i));

                ImGui::TreePop();
            }
            ImGui::PopID();
        }
#endif
    }

} // namespace Platformer
