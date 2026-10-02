/**
 * @file ARPGHud.h
 * @brief Player-facing ARPG HUD on the engine runtime UISystem, projected from the authoritative run state
 *
 * BuildHudModel() is a pure projection of the demo encounter (floor, kills, hero, carried loot, primary skill
 * cooldown, current target and run completion); ARPGHud::Apply() writes that model into an `ARPGHud` panel of
 * UILabel / UIProgressBar widgets on the engine's runtime UI canvas. Because the HUD holds no gameplay state of its
 * own, a loaded save shows the restored run on the next frame without any special case.
 *
 * Contract: game thread only. Owned by SparkGameARPGModule (created in OnLoad when the engine exposes a UISystem,
 * applied every OnUpdate, removed in OnUnload). Widgets are allocated once in Initialize(); Apply() rewrites label
 * text only when the projected model changed since the previous frame.
 */

#pragma once

#include "Enums/ARPGEnums.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace Spark::UI
{
    class UILabel;
    class UIProgressBar;
    class UISystem;
} // namespace Spark::UI

namespace ARPG
{
    class ARPGDemoEncounter;
    class ARPGDungeonSystem;
    class ARPGSkillSystem;

    /// @brief Everything the HUD shows, projected from the authoritative encounter and skill state.
    struct ARPGHudModel
    {
        int floor = 0;
        uint32_t totalKills = 0;
        uint32_t killsOnFloor = 0;
        std::string heroName;
        int heroLevel = 0;
        float heroHealth = 0.0f;
        float heroMaxHealth = 0.0f;
        float heroMana = 0.0f;
        float heroMaxMana = 0.0f;
        size_t lootCarried = 0;
        size_t lootCapacity = 0;
        std::string primarySkillName;
        float primarySkillCooldown = 0.0f; ///< Seconds until the primary skill is ready (0 = ready)
        bool hasTarget = false;
        std::string targetName;
        ARPGMonsterRank targetRank = ARPGMonsterRank::Normal;
        int targetLevel = 0;
        float targetHealth = 0.0f;
        float targetMaxHealth = 0.0f;
        bool runComplete = false;

        bool operator==(const ARPGHudModel&) const = default;
    };

    /// @brief Project the HUD model from the live run; reads state only.
    ARPGHudModel BuildHudModel(const ARPGDemoEncounter& encounter, const ARPGDungeonSystem& dungeon,
                               const ARPGSkillSystem& skills);

    /// @brief The `ARPGHud` panel of runtime UI widgets that displays an ARPGHudModel.
    class ARPGHud
    {
      public:
        static constexpr const char* PanelName = "ARPGHud";
        /// Widget names inside the panel (labels, then progress bars), for tools and tests that read the HUD.
        static constexpr size_t LabelCount = 5;
        static constexpr size_t BarCount = 3;
        static constexpr std::array<const char*, LabelCount> LabelNames = {
            "arpg_hud_floor", "arpg_hud_hero", "arpg_hud_loot", "arpg_hud_skill", "arpg_hud_target"};
        static constexpr std::array<const char*, BarCount> BarNames = {"arpg_hud_health", "arpg_hud_mana",
                                                                       "arpg_hud_target_health"};

        /// @brief Create the panel and its widgets on @p ui's canvas. @return false when @p ui is null.
        bool Initialize(Spark::UI::UISystem* ui);

        /// @brief Show @p model; a model equal to the last one applied changes nothing.
        void Apply(const ARPGHudModel& model);

        /// @brief Remove the panel from the canvas.
        void Shutdown();

      private:
        Spark::UI::UISystem* m_ui = nullptr;
        std::array<Spark::UI::UILabel*, LabelCount> m_labels{};
        std::array<Spark::UI::UIProgressBar*, BarCount> m_bars{};
        ARPGHudModel m_applied;
        bool m_hasApplied = false;
    };
} // namespace ARPG
