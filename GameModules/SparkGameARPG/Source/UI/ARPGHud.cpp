/**
 * @file ARPGHud.cpp
 * @brief Player-facing ARPG HUD on the engine runtime UISystem (see ARPGHud.h)
 */

#include "ARPGHud.h"

#include "Demo/ARPGDemoEncounter.h"
#include "Dungeon/ARPGDungeonSystem.h"
#include "Engine/UI/UISystem.h"
#include "Hero/ARPGHeroSystem.h"
#include "Monster/ARPGMonsterSystem.h"
#include "Skill/ARPGSkillSystem.h"

#include <format>
#include <vector>

namespace ARPG
{
    namespace
    {
        float Fraction(float value, float maximum)
        {
            return maximum > 0.0f ? value / maximum : 0.0f;
        }
    } // namespace

    ARPGHudModel BuildHudModel(const ARPGDemoEncounter& encounter, const ARPGDungeonSystem& dungeon,
                               const ARPGSkillSystem& skills)
    {
        const ARPGDemoEncounterState& state = encounter.GetState();
        ARPGHudModel model;
        model.floor = dungeon.GetCurrentFloorNumber();
        model.totalKills = state.totalKills;
        model.killsOnFloor = state.killsOnFloor;
        model.lootCarried = state.collectedLoot.size();
        model.lootCapacity = ARPGDemoEncounter::MaxCarriedLoot;
        model.runComplete = state.runComplete;

        if (const HeroData* hero = encounter.GetHero())
        {
            model.heroName = hero->name;
            model.heroLevel = hero->level;
            model.heroHealth = hero->health;
            model.heroMaxHealth = hero->maxHealth;
            model.heroMana = hero->mana;
            model.heroMaxMana = hero->maxMana;
            if (const SkillData* skill = skills.GetSkill(state.primarySkillId))
            {
                model.primarySkillName = skill->name;
                for (const SkillCooldownState& cooldown : skills.GetCooldowns(hero->heroId))
                {
                    if (cooldown.skillId == skill->skillId)
                        model.primarySkillCooldown = cooldown.remainingCooldown;
                }
            }
        }

        if (const MonsterData* target = encounter.GetTarget())
        {
            model.hasTarget = true;
            model.targetName = target->name;
            model.targetRank = target->rank;
            model.targetLevel = target->level;
            model.targetHealth = target->health;
            model.targetMaxHealth = target->maxHealth;
        }
        return model;
    }

    bool ARPGHud::Initialize(Spark::UI::UISystem* ui)
    {
        if (!ui)
            return false;
        m_ui = ui;
        m_hasApplied = false;

        Spark::UI::UIPanel* panel = ui->GetCanvas().CreatePanel(PanelName);
        panel->SetAnchor(Spark::UI::Anchor::TopLeft);
        panel->SetLayout(Spark::UI::LayoutDirection::Vertical);
        panel->SetPosition(20.0f, 20.0f);
        panel->SetSize(360.0f, 260.0f);
        for (size_t i = 0; i < LabelNames.size(); ++i)
            m_labels[i] = panel->CreateLabel(LabelNames[i], "");
        for (size_t i = 0; i < BarNames.size(); ++i)
        {
            m_bars[i] = panel->CreateProgressBar(BarNames[i]);
            m_bars[i]->SetSize(320.0f, 16.0f);
        }
        return true;
    }

    void ARPGHud::Apply(const ARPGHudModel& model)
    {
        if (!m_ui || (m_hasApplied && model == m_applied))
            return;

        m_labels[0]->SetText(std::format("Floor {} | Kills {} ({}/{})", model.floor, model.totalKills,
                                         model.killsOnFloor, ARPGDemoEncounter::KillsPerFloor));
        m_labels[1]->SetText(std::format("{} Lv{} | HP {:.0f}/{:.0f} | MP {:.0f}/{:.0f}", model.heroName,
                                         model.heroLevel, model.heroHealth, model.heroMaxHealth, model.heroMana,
                                         model.heroMaxMana));
        m_labels[2]->SetText(std::format("Loot {}/{}", model.lootCarried, model.lootCapacity));
        m_labels[3]->SetText(model.primarySkillCooldown > 0.0f
                                 ? std::format("{}: {:.1f}s", model.primarySkillName, model.primarySkillCooldown)
                                 : std::format("{}: ready", model.primarySkillName));
        if (model.runComplete)
            m_labels[4]->SetText(std::format("Dungeon cleared: boss defeated on floor {}", model.floor));
        else if (model.hasTarget)
            m_labels[4]->SetText(std::format("{} [{}] Lv{} | HP {:.0f}/{:.0f}", model.targetName,
                                             GetMonsterRankName(model.targetRank), model.targetLevel,
                                             model.targetHealth, model.targetMaxHealth));
        else
            m_labels[4]->SetText("No target");

        m_bars[0]->SetValue(Fraction(model.heroHealth, model.heroMaxHealth));
        m_bars[1]->SetValue(Fraction(model.heroMana, model.heroMaxMana));
        m_bars[2]->SetValue(model.hasTarget ? Fraction(model.targetHealth, model.targetMaxHealth) : 0.0f);

        m_applied = model;
        m_hasApplied = true;
    }

    void ARPGHud::Shutdown()
    {
        if (m_ui)
            m_ui->GetCanvas().RemovePanel(PanelName);
        m_ui = nullptr;
        m_labels.fill(nullptr);
        m_bars.fill(nullptr);
        m_hasApplied = false;
    }
} // namespace ARPG
