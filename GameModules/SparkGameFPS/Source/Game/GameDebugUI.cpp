/**
 * @file GameDebugUI.cpp
 * @brief Live status and editor controls for the playable Spark Arena example
 */

#include "Game.h"

#include "Player.h"
#include "ProgressionSystem.h"
#include "WaveSpawner.h"
#include "Projectiles/WeaponStats.h"

#ifdef ENABLE_EDITOR
#include <imgui.h>
#endif

#include <iomanip>
#include <sstream>

namespace
{
    const char* RoundStateToString(Spark::RoundState state)
    {
        switch (state)
        {
        case Spark::RoundState::WaitingForPlayers:
            return "Waiting";
        case Spark::RoundState::Countdown:
            return "Countdown";
        case Spark::RoundState::InProgress:
            return "In progress";
        case Spark::RoundState::RoundEnd:
            return "Round complete";
        case Spark::RoundState::MatchEnd:
            return "Match complete";
        default:
            return "Unknown";
        }
    }

    const char* WaveStateToString(Spark::WaveState state)
    {
        switch (state)
        {
        case Spark::WaveState::Idle:
            return "Idle";
        case Spark::WaveState::Countdown:
            return "Countdown";
        case Spark::WaveState::Spawning:
            return "Spawning";
        case Spark::WaveState::InProgress:
            return "In progress";
        case Spark::WaveState::Completed:
            return "Completed";
        case Spark::WaveState::Failed:
            return "Failed";
        default:
            return "Unknown";
        }
    }
} // namespace

std::string Game::GetStatusString() const
{
    std::ostringstream status;
    status << "=== Spark Arena ===\n";

    if (m_gameMode)
    {
        const auto& rules = m_gameMode->GetRules();
        status << "Mode: " << Spark::GameMode::GameModeTypeToString(rules.type) << " | Round "
               << m_gameMode->GetCurrentRound() << '/' << rules.roundLimit << " | "
               << RoundStateToString(m_gameMode->GetRoundState()) << '\n';
    }

    if (m_waveSpawner)
    {
        status << "Wave: " << m_waveSpawner->GetCurrentWave() << '/' << m_waveSpawner->GetTotalWaves() << " | "
               << WaveStateToString(m_waveSpawner->GetState()) << " | Enemies alive: " << GetAliveEnemyCount() << '\n';
    }

    if (m_player)
    {
        status << std::fixed << std::setprecision(0);
        status << "Player: " << m_player->GetClassName() << " | HP " << m_player->GetHealth() << '/'
               << m_player->GetMaxHealth() << " | Armor " << m_player->GetArmor() << " | Shield "
               << m_player->GetShield() << '\n';
        status << "Weapon: " << WeaponTypeToString(m_player->GetCurrentWeaponType()) << " | Ammo "
               << m_player->GetCurrentAmmo() << '\n';
    }

    if (m_progression)
    {
        status << "Level " << m_progression->GetLevel() << " | XP " << m_progression->GetCurrentXP() << '/'
               << m_progression->GetXPToNextLevel() << '\n';
    }

    status << "SDK services: " << (m_engineSystemsInitialized ? "wired" : "legacy/minimal")
           << " | Quicksave: " << (m_saveSystemReady ? "available" : "unavailable")
           << " | Rendering: " << (m_renderingEnabled ? "on" : "off (no D3D11 device)")
           << " | Time scale: " << std::fixed << std::setprecision(2) << m_timeScale << "x | "
           << (m_isPaused ? "Paused" : "Running");
    return status.str();
}

void Game::RenderDebugUI()
{
#ifdef ENABLE_EDITOR
    if (!ImGui::CollapsingHeader("Spark Arena", ImGuiTreeNodeFlags_DefaultOpen))
    {
        return;
    }

    const std::string status = GetStatusString();
    ImGui::TextUnformatted(status.c_str());
    ImGui::Separator();

    if (ImGui::Button("Start / Restart Survival"))
    {
        StartWaves();
    }
    ImGui::SameLine();
    if (ImGui::Button(m_isPaused ? "Resume" : "Pause"))
    {
        if (m_isPaused)
        {
            Resume();
        }
        else
        {
            Pause();
        }
    }

    if (ImGui::Button("Previous Class"))
    {
        CyclePrevClass();
    }
    ImGui::SameLine();
    if (ImGui::Button("Next Class"))
    {
        CycleNextClass();
    }

    ImGui::TextDisabled("F11: start survival | F5-F10: choose class | [ / ]: cycle class | V: enter vehicle");
#endif
}
