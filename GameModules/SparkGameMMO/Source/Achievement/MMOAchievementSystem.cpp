/**
 * @file MMOAchievementSystem.cpp
 * @brief Achievement definitions, stat tracking, and completion logic
 */

#include "MMOAchievementSystem.h"
#ifdef ENABLE_EDITOR
#include <imgui.h>
#endif

#include <sstream>
#include "Utils/LogMacros.h"

namespace MMO
{

    bool MMOAchievementSystem::Initialize(Spark::IEngineContext* context)
    {
        m_context = context;
        RegisterDefaultAchievements();
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[MMO] Achievement system initialized (%s achievements)",
                       std::to_string(m_achievements.size()).c_str());
        return true;
    }

    void MMOAchievementSystem::Shutdown()
    {
        m_achievements.clear();
        m_statToAch.clear();
    }

    void MMOAchievementSystem::RegisterDefaultAchievements()
    {
        // Combat achievements
        RegisterAchievement({1,
                             "First Blood",
                             "Defeat your first enemy",
                             AchievementCategory::Combat,
                             false,
                             {{"Defeat 1 enemy", "kills_total", 1}},
                             {50, 10, "", 5},
                             2});
        RegisterAchievement({2,
                             "Warrior",
                             "Defeat 50 enemies",
                             AchievementCategory::Combat,
                             false,
                             {{"Defeat 50 enemies", "kills_total", 50}},
                             {200, 50, "", 10},
                             3});
        RegisterAchievement({3,
                             "Slayer",
                             "Defeat 500 enemies",
                             AchievementCategory::Combat,
                             false,
                             {{"Defeat 500 enemies", "kills_total", 500}},
                             {1000, 200, "Slayer", 25},
                             0});

        // Exploration
        RegisterAchievement({10,
                             "Explorer",
                             "Visit all 4 world areas",
                             AchievementCategory::Exploration,
                             false,
                             {{"Visit all areas", "areas_visited", 4}},
                             {300, 100, "Explorer", 15},
                             0});
        RegisterAchievement({11,
                             "Traveler",
                             "Travel 10km total",
                             AchievementCategory::Exploration,
                             false,
                             {{"Travel 10,000 meters", "distance_traveled", 10000}},
                             {200, 50, "", 10},
                             0});

        // Social
        RegisterAchievement({20,
                             "Social Butterfly",
                             "Join a guild",
                             AchievementCategory::Social,
                             false,
                             {{"Join a guild", "guilds_joined", 1}},
                             {100, 25, "", 5},
                             0});
        RegisterAchievement({21,
                             "Team Player",
                             "Complete 10 party dungeons",
                             AchievementCategory::Social,
                             false,
                             {{"Complete 10 dungeons", "dungeons_completed", 10}},
                             {500, 150, "Team Player", 20},
                             0});

        // Crafting
        RegisterAchievement({30,
                             "Apprentice Crafter",
                             "Craft 10 items",
                             AchievementCategory::Crafting,
                             false,
                             {{"Craft 10 items", "items_crafted", 10}},
                             {150, 30, "", 5},
                             31});
        RegisterAchievement({31,
                             "Master Crafter",
                             "Craft 100 items",
                             AchievementCategory::Crafting,
                             false,
                             {{"Craft 100 items", "items_crafted", 100}},
                             {500, 100, "Master Crafter", 20},
                             0});

        // Collection
        RegisterAchievement({40,
                             "Hoarder",
                             "Accumulate 1000 currency",
                             AchievementCategory::Collection,
                             false,
                             {{"Earn 1000 gold", "currency_earned", 1000}},
                             {0, 0, "Moneybags", 10},
                             0});

        // Dungeon
        RegisterAchievement({50,
                             "Dungeon Delver",
                             "Complete your first dungeon",
                             AchievementCategory::Dungeon,
                             false,
                             {{"Complete 1 dungeon", "dungeons_completed", 1}},
                             {200, 50, "", 10},
                             0});
        RegisterAchievement({51,
                             "Boss Slayer",
                             "Defeat 10 dungeon bosses",
                             AchievementCategory::Dungeon,
                             false,
                             {{"Defeat 10 bosses", "bosses_killed", 10}},
                             {400, 100, "Boss Slayer", 15},
                             0});

        // PvP
        RegisterAchievement({60,
                             "Gladiator",
                             "Win 10 PvP matches",
                             AchievementCategory::PvP,
                             false,
                             {{"Win 10 PvP matches", "pvp_wins", 10}},
                             {300, 75, "Gladiator", 15},
                             0});
    }

    void MMOAchievementSystem::RegisterAchievement(const AchievementDef& def)
    {
        m_achievements[def.id] = def;
        for (const auto& crit : def.criteria)
        {
            m_statToAch[crit.trackingKey].push_back(def.id);
        }
    }

    const AchievementDef* MMOAchievementSystem::GetAchievement(uint32_t id) const
    {
        auto it = m_achievements.find(id);
        return it != m_achievements.end() ? &it->second : nullptr;
    }

    void MMOAchievementSystem::InitializePlayerState(AchievementState& state) const
    {
        state.progress.clear();
        for (const auto& [id, def] : m_achievements)
        {
            AchievementProgress prog;
            prog.achievementId = id;
            prog.criteriaProgress.resize(def.criteria.size());
            state.progress.push_back(prog);
        }
    }

    void MMOAchievementSystem::IncrementStat(AchievementState& state, const std::string& statKey, int increment)
    {
        state.stats[statKey] += increment;
        int value = state.stats[statKey];

        auto it = m_statToAch.find(statKey);
        if (it == m_statToAch.end())
            return;

        for (uint32_t achId : it->second)
        {
            if (state.completedIds.count(achId))
                continue;
            CheckAchievement(state, achId);
        }
    }

    void MMOAchievementSystem::SetStat(AchievementState& state, const std::string& statKey, int value)
    {
        state.stats[statKey] = value;
        auto it = m_statToAch.find(statKey);
        if (it == m_statToAch.end())
            return;

        for (uint32_t achId : it->second)
        {
            if (!state.completedIds.count(achId))
                CheckAchievement(state, achId);
        }
    }

    void MMOAchievementSystem::CheckAchievement(AchievementState& state, uint32_t achId)
    {
        const auto* def = GetAchievement(achId);
        if (!def)
            return;

        for (auto& prog : state.progress)
        {
            if (prog.achievementId != achId || prog.completed)
                continue;

            bool allDone = true;
            for (size_t i = 0; i < def->criteria.size() && i < prog.criteriaProgress.size(); ++i)
            {
                auto& cp = prog.criteriaProgress[i];
                auto statIt = state.stats.find(def->criteria[i].trackingKey);
                if (statIt != state.stats.end())
                    cp.current = statIt->second;

                if (cp.current >= def->criteria[i].requiredCount)
                    cp.completed = true;
                else
                    allDone = false;
            }

            if (allDone)
                CompleteAchievement(state, achId);
            break;
        }
    }

    void MMOAchievementSystem::CompleteAchievement(AchievementState& state, uint32_t achId)
    {
        const auto* def = GetAchievement(achId);
        if (!def)
            return;

        state.completedIds.insert(achId);
        state.totalPoints += def->reward.points;

        for (auto& prog : state.progress)
        {
            if (prog.achievementId == achId)
            {
                prog.completed = true;
                break;
            }
        }

        if (!def->reward.title.empty())
        {
            state.earnedTitles.push_back(def->reward.title);
            if (state.activeTitle.empty())
                state.activeTitle = def->reward.title;
        }

        SPARK_LOG_INFO(Spark::LogCategory::Game, "[MMO] Achievement unlocked: %s (+%s pts)", def->name.c_str(),
                       std::to_string(def->reward.points).c_str());
    }

    void MMOAchievementSystem::ForceComplete(AchievementState& state, uint32_t achId)
    {
        if (!state.completedIds.count(achId))
            CompleteAchievement(state, achId);
    }

    float MMOAchievementSystem::GetCompletionPercent(const AchievementState& state) const
    {
        if (m_achievements.empty())
            return 0.0f;
        return static_cast<float>(state.completedIds.size()) / m_achievements.size() * 100.0f;
    }

    std::string MMOAchievementSystem::GetStatusString(const AchievementState& state) const
    {
        std::ostringstream ss;
        ss << "Achievements: " << state.completedIds.size() << "/" << m_achievements.size();
        ss << " (" << static_cast<int>(GetCompletionPercent(state)) << "%)";
        ss << " | Points: " << state.totalPoints;
        if (!state.activeTitle.empty())
            ss << " | Title: " << state.activeTitle;
        return ss.str();
    }

    void MMOAchievementSystem::RenderDebugUI()
    {
#ifdef ENABLE_EDITOR
        if (ImGui::TreeNode("MMO Achievement System"))
        {
            ImGui::Text("Total Achievements: %zu", m_achievements.size());
            for (const auto& [id, def] : m_achievements)
            {
                ImGui::Text("[%u] %s - %s (%d pts)", id, def.name.c_str(), def.description.c_str(), def.reward.points);
            }
            ImGui::TreePop();
        }
#endif
    }

} // namespace MMO
