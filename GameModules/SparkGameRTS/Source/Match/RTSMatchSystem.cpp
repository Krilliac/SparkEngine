/**
 * @file RTSMatchSystem.cpp
 * @brief Match lifecycle: setup, faction selection, win conditions, state tracking
 */

#include "RTSMatchSystem.h"
#include "Utils/SparkConsole.h"
#include "Utils/LogMacros.h"

#ifdef ENABLE_EDITOR
#include <imgui.h>
#endif

namespace RTS
{

    bool RTSMatchSystem::Initialize(Spark::IEngineContext* context)
    {
        m_context = context;
        m_state = RTSMatchState::Setup;
        m_matchTime = 0.0f;
        m_hasWinner = false;

        SPARK_LOG_INFO(Spark::LogCategory::Game, "RTS match system initialized");
        Spark::SimpleConsole::GetInstance().LogInfo("[RTS] Match system initialized");
        return true;
    }

    void RTSMatchSystem::Update(float deltaTime)
    {
        if (m_state != RTSMatchState::Playing)
            return;

        m_matchTime += deltaTime;
        CheckWinConditions();
    }

    void RTSMatchSystem::Shutdown()
    {
        m_players.clear();
        m_state = RTSMatchState::Setup;
    }

    // === Match flow ===

    void RTSMatchSystem::SetupMatch(int playerCount)
    {
        m_players.clear();
        m_players.resize(static_cast<size_t>(playerCount));

        // Default faction assignments
        for (int i = 0; i < playerCount; ++i)
        {
            m_players[static_cast<size_t>(i)].faction =
                static_cast<RTSFaction>(i % static_cast<int>(RTSFaction::Count));
        }

        m_state = RTSMatchState::Setup;
        m_matchTime = 0.0f;
        m_hasWinner = false;

        Spark::SimpleConsole::GetInstance().LogInfo("[RTS] Match setup for " + std::to_string(playerCount) +
                                                    " players");
    }

    void RTSMatchSystem::SetPlayerFaction(int playerIndex, RTSFaction faction)
    {
        if (playerIndex < 0 || playerIndex >= static_cast<int>(m_players.size()))
            return;

        m_players[static_cast<size_t>(playerIndex)].faction = faction;
    }

    void RTSMatchSystem::SetPlayerStartPosition(int playerIndex, float x, float y)
    {
        if (playerIndex < 0 || playerIndex >= static_cast<int>(m_players.size()))
            return;

        auto& player = m_players[static_cast<size_t>(playerIndex)];
        player.startX = x;
        player.startY = y;
    }

    void RTSMatchSystem::SetPlayerIsAI(int playerIndex, bool isAI)
    {
        if (playerIndex < 0 || playerIndex >= static_cast<int>(m_players.size()))
            return;

        m_players[static_cast<size_t>(playerIndex)].isAI = isAI;
    }

    bool RTSMatchSystem::StartMatch()
    {
        if (m_players.size() < 2)
        {
            Spark::SimpleConsole::GetInstance().LogError("[RTS] Need at least 2 players to start a match");
            return false;
        }

        m_state = RTSMatchState::Playing;
        m_matchTime = 0.0f;
        m_hasWinner = false;

        // Reset elimination flags
        for (auto& player : m_players)
        {
            player.hasSurrendered = false;
            player.isEliminated = false;
        }

        SPARK_LOG_INFO(Spark::LogCategory::Game, "RTS match started with %zu players", m_players.size());
        Spark::SimpleConsole::GetInstance().LogInfo("[RTS] Match started!");
        return true;
    }

    void RTSMatchSystem::EndMatch(RTSFaction winner)
    {
        m_state = RTSMatchState::Victory;
        m_winner = winner;
        m_hasWinner = true;

        const char* factionNames[] = {"Human", "Sentinel", "Swarm"};
        int idx = static_cast<int>(winner);
        std::string name = (idx >= 0 && idx < 3) ? factionNames[idx] : "Unknown";

        SPARK_LOG_INFO(Spark::LogCategory::Game, "RTS match ended — winner: %s (time: %ds)", name.c_str(),
                       static_cast<int>(m_matchTime));
        Spark::SimpleConsole::GetInstance().LogInfo("[RTS] Match ended! Winner: " + name +
                                                    " (Time: " + std::to_string(static_cast<int>(m_matchTime)) + "s)");
    }

    void RTSMatchSystem::Surrender(int playerIndex)
    {
        if (playerIndex < 0 || playerIndex >= static_cast<int>(m_players.size()))
            return;

        m_players[static_cast<size_t>(playerIndex)].hasSurrendered = true;
        m_players[static_cast<size_t>(playerIndex)].isEliminated = true;

        Spark::SimpleConsole::GetInstance().LogInfo("[RTS] Player " + std::to_string(playerIndex) + " surrendered");
    }

    // === Queries ===

    RTSMatchState RTSMatchSystem::GetMatchState() const
    {
        return m_state;
    }

    float RTSMatchSystem::GetMatchTime() const
    {
        return m_matchTime;
    }

    int RTSMatchSystem::GetPlayerCount() const
    {
        return static_cast<int>(m_players.size());
    }

    const PlayerSetup* RTSMatchSystem::GetPlayer(int index) const
    {
        if (index < 0 || index >= static_cast<int>(m_players.size()))
            return nullptr;

        return &m_players[static_cast<size_t>(index)];
    }

    RTSFaction RTSMatchSystem::GetWinner() const
    {
        return m_winner;
    }

    std::string RTSMatchSystem::GetMatchStatusString() const
    {
        std::string result = "=== RTS Match ===\n";

        const char* stateNames[] = {"Setup", "Playing", "Paused", "Victory", "Defeat"};
        int stateIdx = static_cast<int>(m_state);
        result += "State: " + std::string((stateIdx >= 0 && stateIdx < 5) ? stateNames[stateIdx] : "Unknown") + "\n";
        result += "Time: " + std::to_string(static_cast<int>(m_matchTime)) + "s\n";
        result += "Players: " + std::to_string(m_players.size()) + "\n";

        const char* factionNames[] = {"Human", "Sentinel", "Swarm"};
        for (size_t i = 0; i < m_players.size(); ++i)
        {
            const auto& p = m_players[i];
            int fIdx = static_cast<int>(p.faction);
            std::string fName = (fIdx >= 0 && fIdx < 3) ? factionNames[fIdx] : "Unknown";
            result += "  P" + std::to_string(i) + ": " + fName;
            result += p.isAI ? " (AI)" : " (Human)";
            if (p.isEliminated)
                result += " [ELIMINATED]";
            result += "\n";
        }

        if (m_hasWinner)
        {
            int wIdx = static_cast<int>(m_winner);
            std::string wName = (wIdx >= 0 && wIdx < 3) ? factionNames[wIdx] : "Unknown";
            result += "Winner: " + wName + "\n";
        }
        return result;
    }

    // === Win condition checks ===

    void RTSMatchSystem::MarkPlayerEliminated(int playerIndex)
    {
        if (playerIndex < 0 || playerIndex >= static_cast<int>(m_players.size()))
            return;

        m_players[static_cast<size_t>(playerIndex)].isEliminated = true;
    }

    bool RTSMatchSystem::IsPlayerEliminated(int playerIndex) const
    {
        if (playerIndex < 0 || playerIndex >= static_cast<int>(m_players.size()))
            return true;

        return m_players[static_cast<size_t>(playerIndex)].isEliminated;
    }

    int RTSMatchSystem::GetRemainingPlayerCount() const
    {
        int count = 0;
        for (const auto& player : m_players)
        {
            if (!player.isEliminated)
                count++;
        }
        return count;
    }

    // === Internal ===

    void RTSMatchSystem::CheckWinConditions()
    {
        if (m_hasWinner || m_state != RTSMatchState::Playing)
            return;

        int remaining = GetRemainingPlayerCount();
        if (remaining <= 1)
        {
            // Find the last standing player's faction
            for (const auto& player : m_players)
            {
                if (!player.isEliminated)
                {
                    EndMatch(player.faction);
                    return;
                }
            }
        }
    }

    void RTSMatchSystem::RenderDebugUI()
    {
#ifdef ENABLE_EDITOR
        if (ImGui::TreeNode("RTS Match System"))
        {
            const char* stateNames[] = {"Setup", "Playing", "Paused", "Victory", "Defeat"};
            int idx = static_cast<int>(m_state);
            ImGui::Text("State: %s", (idx >= 0 && idx < 5) ? stateNames[idx] : "Unknown");
            ImGui::Text("Time: %.1fs", m_matchTime);
            ImGui::Text("Players: %zu", m_players.size());
            ImGui::Text("Remaining: %d", GetRemainingPlayerCount());

            if (m_hasWinner)
            {
                const char* factionNames[] = {"Human", "Sentinel", "Swarm"};
                int fIdx = static_cast<int>(m_winner);
                ImGui::Text("Winner: %s", (fIdx >= 0 && fIdx < 3) ? factionNames[fIdx] : "Unknown");
            }
            ImGui::TreePop();
        }
#endif
    }

} // namespace RTS
