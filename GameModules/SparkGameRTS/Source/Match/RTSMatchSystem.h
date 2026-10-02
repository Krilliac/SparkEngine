/**
 * @file RTSMatchSystem.h
 * @brief Match management: setup, factions, win conditions, state tracking
 * @author Spark Engine Team
 * @date 2026
 *
 * Manages match lifecycle from setup (faction selection, starting positions)
 * through gameplay to victory/defeat detection.
 */

#pragma once

#include "Spark/IEngineContext.h"
#include "Enums/RTSEnums.h"

#include <cstdint>
#include <string>
#include <vector>

namespace RTS
{

    /// @brief Starting configuration for a player in a match
    struct PlayerSetup
    {
        RTSFaction faction = RTSFaction::Human;
        float startX = 0.0f;
        float startY = 0.0f;
        bool isAI = false;
        bool hasSurrendered = false;
        bool isEliminated = false;
    };

    /// @brief Complete match lifecycle state, as persisted in a save
    struct RTSMatchSnapshot
    {
        RTSMatchState state = RTSMatchState::Setup;
        float matchTime = 0.0f;
        RTSFaction winner = RTSFaction::Human;
        bool hasWinner = false;
        std::vector<PlayerSetup> players;
    };

    /**
     * @brief Manages match lifecycle, players, and win conditions
     */
    class RTSMatchSystem
    {
      public:
        RTSMatchSystem() = default;
        ~RTSMatchSystem() = default;

        bool Initialize(Spark::IEngineContext* context);
        void Update(float deltaTime);
        void Shutdown();
        void RenderDebugUI();

        // === Match flow ===
        void SetupMatch(int playerCount);
        void SetPlayerFaction(int playerIndex, RTSFaction faction);
        void SetPlayerStartPosition(int playerIndex, float x, float y);
        void SetPlayerIsAI(int playerIndex, bool isAI);
        bool StartMatch();
        void EndMatch(RTSFaction winner);
        void Surrender(int playerIndex);

        // === Queries ===
        RTSMatchState GetMatchState() const;
        float GetMatchTime() const;
        int GetPlayerCount() const;
        const PlayerSetup* GetPlayer(int index) const;
        RTSFaction GetWinner() const;
        bool HasWinner() const;
        std::string GetMatchStatusString() const;

        // === Persistence ===
        RTSMatchSnapshot CaptureState() const;
        /**
         * @brief Replace the match lifecycle state from a persistence snapshot.
         * @return false (leaving state untouched) on an out-of-range enum, a non-finite or negative match time,
         *         a non-finite start position, or more than MAX_PLAYERS players.
         */
        bool RestoreState(const RTSMatchSnapshot& snapshot);

        static constexpr int MAX_PLAYERS = 8;

        // === Win condition checks ===
        void MarkPlayerEliminated(int playerIndex);
        bool IsPlayerEliminated(int playerIndex) const;
        int GetRemainingPlayerCount() const;

      private:
        void CheckWinConditions();

        Spark::IEngineContext* m_context{nullptr};

        RTSMatchState m_state = RTSMatchState::Setup;
        std::vector<PlayerSetup> m_players;
        float m_matchTime = 0.0f;
        RTSFaction m_winner = RTSFaction::Human;
        bool m_hasWinner = false;
    };

} // namespace RTS
