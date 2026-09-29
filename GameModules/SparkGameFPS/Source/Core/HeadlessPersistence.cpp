/**
 * @file HeadlessPersistence.cpp
 * @brief SparkGameFPS local-profile persistence for the no-render headless lifecycle.
 *
 * The headless host builds no renderable Game, so the windowed progression and
 * quicksave commands (which reach Game::QuickSaveProfile / QuickLoadProfile)
 * are never registered there. This file gives the headless arena the same four
 * console commands -- level, xp, quicksave and quickload -- backed by a
 * CPU-only ProgressionSystem and the arena's Deathmatch scoreboard. Each
 * command prints text byte-identical to its windowed counterpart, so the
 * installed-package save/reload audit validator accepts both hosts.
 *
 * Thread affinity: game thread (console dispatch runs from the host loop).
 * Ownership: the commands capture `this`; Shutdown() unregisters them before
 * ShutdownHeadlessArena() releases the state they read.
 */

#include "SparkGameFPS.h"
#include "Engine/SaveSystem/SaveSystem.h"
#include "Game/FPSLocalProfile.h"
#include "Game/FPSQuickLoad.h"
#include "Game/GameMode.h"
#include "Game/ProgressionSystem.h"
#include "Utils/SparkConsole.h"

#include <exception>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
    /// Same slot the windowed Game::kQuickSaveSlot writes, so both hosts share one save.
    constexpr const char* kQuickSaveSlot = "fps_quicksave";
    constexpr const char* kLocalPlayer = "Player1";
} // namespace

void SparkGameModule::RegisterHeadlessPersistenceCommands()
{
    auto& console = Spark::SimpleConsole::GetInstance();
    const auto registerTracked = [&console, this](const std::string& name, Spark::SimpleConsole::CommandHandler handler,
                                                  const std::string& description)
    {
        console.RegisterCommand(name, std::move(handler), description);
        m_registeredConsoleCommands.push_back(name);
    };

    registerTracked(
        "level", [this](const std::vector<std::string>&) -> std::string
        { return m_headlessProgression ? m_headlessProgression->Console_GetStatus() : "Progression not initialized"; },
        "Show player level and XP status");

    registerTracked(
        "xp",
        [this](const std::vector<std::string>& args) -> std::string
        {
            if (args.empty())
            {
                return "Usage: xp <amount>";
            }
            if (!m_headlessProgression)
            {
                return "Progression not initialized";
            }
            int amount;
            try
            {
                amount = std::stoi(args[0]);
            }
            catch (const std::exception&)
            {
                return "Invalid XP amount: " + args[0];
            }
            if (amount < 1 || amount > Spark::ProgressionSystem::MAX_SINGLE_AWARD)
            {
                return "XP amount must be between 1 and " + std::to_string(Spark::ProgressionSystem::MAX_SINGLE_AWARD);
            }
            m_headlessProgression->AwardXP(amount, "console");
            return "Awarded " + std::to_string(amount) + " XP (level " +
                   std::to_string(m_headlessProgression->GetLevel()) + ")";
        },
        "Award XP to player (xp <amount>)");

    registerTracked(
        "quicksave", [this](const std::vector<std::string>&) -> std::string { return HeadlessQuickSave(); },
        "Quick save current game state");

    registerTracked(
        "quickload", [this](const std::vector<std::string>&) -> std::string { return HeadlessQuickLoad(); },
        "Quick load last saved state");
}

std::string SparkGameModule::HeadlessQuickSave() const
{
    Spark::SaveSystem* saveSystem = m_context ? m_context->GetSaveSystem() : nullptr;
    World* world = m_context ? m_context->GetWorld() : nullptr;
    if (!saveSystem || !world)
    {
        return "Save system unavailable: engine exposes no save system or world";
    }

    Spark::FPSLocalProfile profile;
    profile.playTimeSeconds = m_headlessPlayTime;
    if (m_headlessProgression)
    {
        profile.progressionLevel = m_headlessProgression->GetLevel();
        profile.progressionXP = m_headlessProgression->GetCurrentXP();
    }
    if (m_headlessMode)
    {
        if (const auto* score = m_headlessMode->GetPlayerScore(kLocalPlayer))
        {
            profile.kills = score->kills;
            profile.deaths = score->deaths;
            profile.score = score->totalScore;
        }
    }

    Spark::SaveMetadata metadata;
    metadata.saveName = "Quick Save";
    metadata.sceneName = "combat_arena";
    metadata.playTime = m_headlessPlayTime;
    metadata.playerKills = profile.kills;
    metadata.playerDeaths = profile.deaths;

    std::unordered_map<std::string, std::string> customState;
    profile.WriteTo(customState);

    if (!saveSystem->Save(kQuickSaveSlot, *world, metadata, customState))
    {
        return std::string("Quick save FAILED to write slot '") + kQuickSaveSlot + "'";
    }
    return std::string("Quick save written to slot '") + kQuickSaveSlot + "'";
}

std::string SparkGameModule::HeadlessQuickLoad()
{
    Spark::SaveSystem* saveSystem = m_context ? m_context->GetSaveSystem() : nullptr;
    World* world = m_context ? m_context->GetWorld() : nullptr;
    if (!saveSystem || !world)
    {
        return "Save system unavailable: engine exposes no save system or world";
    }
    if (!saveSystem->SaveExists(kQuickSaveSlot))
    {
        return std::string("No quicksave found in slot '") + kQuickSaveSlot + "'";
    }

    // The profile is validated before the world is replaced, so a rejected profile block
    // leaves both the world and this host's progression and scoreboard as they were.
    Spark::FPSLocalProfile profile;
    std::string profileError;
    switch (Spark::LoadSlotWithProfile(*saveSystem, kQuickSaveSlot, *world, profile, profileError))
    {
    case Spark::FPSQuickLoadStatus::Loaded:
        break;
    case Spark::FPSQuickLoadStatus::ProfileRejected:
        return "Quick load rejected the local profile; the world is unchanged: " + profileError;
    case Spark::FPSQuickLoadStatus::LoadFailed:
        return std::string("Quick load FAILED for slot '") + kQuickSaveSlot + "'";
    }

    // The headless arena has no Player, so class, weapon, health and armor stay
    // in the file for the windowed host; progression, play time and the
    // scoreboard are the state this host owns.
    m_headlessPlayTime = profile.playTimeSeconds;
    if (m_headlessProgression)
    {
        m_headlessProgression->RestoreProgress(profile.progressionXP);
    }
    if (m_headlessMode)
    {
        m_headlessMode->RestorePlayerScore(kLocalPlayer, profile.kills, profile.deaths, profile.score);
    }

    // Report the level re-derived from XP, as the windowed host does.
    const int restoredLevel = m_headlessProgression ? m_headlessProgression->GetLevel() : profile.progressionLevel;
    return "Quick load restored level " + std::to_string(restoredLevel) + " (" + std::to_string(profile.progressionXP) +
           " XP)";
}
