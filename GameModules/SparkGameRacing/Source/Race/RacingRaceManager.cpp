/**
 * @file RacingRaceManager.cpp
 * @brief Race lifecycle, timing, position tracking, and championship points
 */

#include "RacingRaceManager.h"
#include "Utils/SparkConsole.h"
#include "Utils/LogMacros.h"

#include <algorithm>

#ifdef ENABLE_EDITOR
#include <imgui.h>
#endif

namespace Racing
{

    bool RacingRaceManager::Initialize(Spark::IEngineContext* context)
    {
        m_context = context;
        m_initialized = true;

        auto& console = Spark::SimpleConsole::GetInstance();
        SPARK_LOG_INFO(Spark::LogCategory::Game, "Racing race manager initialized");
        console.LogInfo("[Racing Race] Race manager initialized");
        return true;
    }

    void RacingRaceManager::Update(float deltaTime)
    {
        if (!m_initialized)
            return;

        switch (m_state)
        {
        case RaceState::Countdown:
            UpdateCountdown(deltaTime);
            break;
        case RaceState::Racing:
            UpdateRacing(deltaTime);
            break;
        case RaceState::Paused:
        case RaceState::Finished:
        case RaceState::DNF:
        case RaceState::Count:
            break;
        }
    }

    void RacingRaceManager::Shutdown()
    {
        m_racers.clear();
        m_championship.clear();
        m_initialized = false;
    }

    void RacingRaceManager::RegisterRacer(uint32_t vehicleId, const std::string& name, bool isPlayer)
    {
        RacerState racer{};
        racer.vehicleId = vehicleId;
        racer.name = name;
        racer.isPlayer = isPlayer;
        racer.position = static_cast<uint32_t>(m_racers.size()) + 1;
        m_racers.push_back(racer);
    }

    void RacingRaceManager::StartRace(RaceMode mode, uint32_t totalLaps)
    {
        m_mode = mode;
        m_totalLaps = totalLaps;
        m_state = RaceState::Countdown;
        m_countdownTimer = 3.0f;
        m_raceTime = 0.0f;

        // Reset all racers
        for (auto& racer : m_racers)
        {
            racer.currentLap = 0;
            racer.lastCheckpoint = 0;
            racer.distanceAlongTrack = 0.0f;
            racer.totalTime = 0.0f;
            racer.currentLapTime = 0.0f;
            racer.bestLapTime = -1.0f;
            racer.splitTimes.clear();
            racer.lapTimes.clear();
            racer.finished = false;
            racer.dnf = false;
            racer.finishTime = 0.0f;
        }

        auto& console = Spark::SimpleConsole::GetInstance();
        SPARK_LOG_INFO(Spark::LogCategory::Game, "Race starting: %zu racers, %u laps", m_racers.size(), totalLaps);
        console.LogInfo("[Racing Race] Race starting: " + std::to_string(m_racers.size()) + " racers, " +
                        std::to_string(totalLaps) + " laps");
    }

    void RacingRaceManager::OnCheckpointCrossed(uint32_t vehicleId, uint32_t checkpointIndex)
    {
        for (auto& racer : m_racers)
        {
            if (racer.vehicleId != vehicleId || racer.finished)
                continue;

            // Checkpoints must be crossed in order
            uint32_t expectedNext = racer.lastCheckpoint + 1;
            if (checkpointIndex == expectedNext || (checkpointIndex == 0 && racer.lastCheckpoint > 0))
            {
                racer.lastCheckpoint = checkpointIndex;
                racer.splitTimes.push_back(racer.currentLapTime);
            }
            break;
        }
    }

    void RacingRaceManager::OnLapCompleted(uint32_t vehicleId)
    {
        for (auto& racer : m_racers)
        {
            if (racer.vehicleId != vehicleId || racer.finished)
                continue;

            racer.currentLap++;
            racer.lapTimes.push_back(racer.currentLapTime);

            // Update best lap
            if (racer.bestLapTime < 0.0f || racer.currentLapTime < racer.bestLapTime)
                racer.bestLapTime = racer.currentLapTime;

            racer.currentLapTime = 0.0f;
            racer.lastCheckpoint = 0;
            racer.splitTimes.clear();

            // Check if finished
            if (racer.currentLap >= m_totalLaps)
            {
                racer.finished = true;
                racer.finishTime = m_raceTime;
            }
            break;
        }

        CheckFinishConditions();
    }

    void RacingRaceManager::UpdateRacerDistance(uint32_t vehicleId, float distance)
    {
        for (auto& racer : m_racers)
        {
            if (racer.vehicleId == vehicleId)
            {
                racer.distanceAlongTrack = distance;
                break;
            }
        }
    }

    const RacerState* RacingRaceManager::GetRacer(uint32_t vehicleId) const
    {
        for (const auto& racer : m_racers)
        {
            if (racer.vehicleId == vehicleId)
                return &racer;
        }
        return nullptr;
    }

    uint32_t RacingRaceManager::GetPlayerPosition() const
    {
        for (const auto& racer : m_racers)
        {
            if (racer.isPlayer)
                return racer.position;
        }
        return 0;
    }

    float RacingRaceManager::GetPlayerBestLap() const
    {
        for (const auto& racer : m_racers)
        {
            if (racer.isPlayer)
                return racer.bestLapTime;
        }
        return -1.0f;
    }

    std::string RacingRaceManager::GetStandingsString() const
    {
        std::string result = "Race Standings:\n";
        for (const auto& racer : m_racers)
        {
            result += "  P" + std::to_string(racer.position) + " - " + racer.name;
            result += " | Lap " + std::to_string(racer.currentLap) + "/" + std::to_string(m_totalLaps);
            if (racer.bestLapTime >= 0.0f)
                result += " | Best: " + std::to_string(racer.bestLapTime) + "s";
            if (racer.finished)
                result += " [FINISHED]";
            if (racer.dnf)
                result += " [DNF]";
            result += "\n";
        }
        return result;
    }

    std::string RacingRaceManager::GetResultsString() const
    {
        if (m_state != RaceState::Finished)
            return "Race not finished yet.\n";

        std::string result = "=== Race Results ===\n";
        for (const auto& racer : m_racers)
        {
            result += "  P" + std::to_string(racer.position) + " - " + racer.name;
            result += " | Time: " + std::to_string(racer.finishTime) + "s";
            if (racer.bestLapTime >= 0.0f)
                result += " | Best Lap: " + std::to_string(racer.bestLapTime) + "s";
            if (m_mode == RaceMode::Championship)
                result += " | Points: " + std::to_string(GetPointsForPosition(racer.position));
            result += "\n";
        }
        return result;
    }

    void RacingRaceManager::UpdateCountdown(float deltaTime)
    {
        m_countdownTimer -= deltaTime;
        if (m_countdownTimer <= 0.0f)
        {
            m_state = RaceState::Racing;
            auto& console = Spark::SimpleConsole::GetInstance();
            SPARK_LOG_INFO(Spark::LogCategory::Game, "Race GO!");
            console.LogInfo("[Racing Race] GO!");
        }
    }

    void RacingRaceManager::UpdateRacing(float deltaTime)
    {
        m_raceTime += deltaTime;

        for (auto& racer : m_racers)
        {
            if (!racer.finished && !racer.dnf)
            {
                racer.totalTime += deltaTime;
                racer.currentLapTime += deltaTime;
            }
        }

        SortPositions();

        // DNF timeout
        if (m_raceTime > m_dnfTimeout)
        {
            for (auto& racer : m_racers)
            {
                if (!racer.finished)
                    racer.dnf = true;
            }
            m_state = RaceState::Finished;
            AwardChampionshipPoints();
        }
    }

    void RacingRaceManager::SortPositions()
    {
        // Sort by: finished first (by finish time), then by lap count (desc), then by distance along track (desc)
        std::vector<size_t> indices(m_racers.size());
        for (size_t i = 0; i < indices.size(); ++i)
            indices[i] = i;

        std::sort(indices.begin(), indices.end(),
                  [this](size_t a, size_t b)
                  {
                      const auto& ra = m_racers[a];
                      const auto& rb = m_racers[b];

                      // Finished racers first, sorted by finish time
                      if (ra.finished != rb.finished)
                          return ra.finished;
                      if (ra.finished && rb.finished)
                          return ra.finishTime < rb.finishTime;

                      // DNF last
                      if (ra.dnf != rb.dnf)
                          return rb.dnf;

                      // Higher lap count first
                      if (ra.currentLap != rb.currentLap)
                          return ra.currentLap > rb.currentLap;

                      // Greater distance along track first
                      return ra.distanceAlongTrack > rb.distanceAlongTrack;
                  });

        for (size_t i = 0; i < indices.size(); ++i)
            m_racers[indices[i]].position = static_cast<uint32_t>(i) + 1;
    }

    void RacingRaceManager::CheckFinishConditions()
    {
        bool allDone = true;
        for (const auto& racer : m_racers)
        {
            if (!racer.finished && !racer.dnf)
            {
                allDone = false;
                break;
            }
        }

        if (allDone)
        {
            m_state = RaceState::Finished;
            AwardChampionshipPoints();

            auto& console = Spark::SimpleConsole::GetInstance();
            SPARK_LOG_INFO(Spark::LogCategory::Game, "Race finished!");
            console.LogInfo("[Racing Race] Race finished!");
        }
    }

    void RacingRaceManager::AwardChampionshipPoints()
    {
        if (m_mode != RaceMode::Championship)
            return;

        for (auto& racer : m_racers)
        {
            uint32_t points = GetPointsForPosition(racer.position);
            racer.championshipPoints += points;

            // Update championship standings
            bool found = false;
            for (auto& entry : m_championship)
            {
                if (entry.vehicleId == racer.vehicleId)
                {
                    entry.totalPoints += points;
                    if (racer.position == 1)
                        entry.wins++;
                    found = true;
                    break;
                }
            }

            if (!found)
            {
                ChampionshipEntry entry{};
                entry.vehicleId = racer.vehicleId;
                entry.name = racer.name;
                entry.totalPoints = points;
                entry.wins = (racer.position == 1) ? 1 : 0;
                m_championship.push_back(entry);
            }
        }
    }

    uint32_t RacingRaceManager::GetPointsForPosition(uint32_t position)
    {
        constexpr uint32_t pointsTable[] = {25, 18, 15, 12, 10, 8, 6, 4, 2, 1};
        if (position == 0 || position > 10)
            return 0;
        return pointsTable[position - 1];
    }

    void RacingRaceManager::RenderDebugUI()
    {
#ifdef ENABLE_EDITOR
        if (!ImGui::CollapsingHeader("Racing Race Manager"))
            return;

        const char* stateNames[] = {"Countdown", "Racing", "Paused", "Finished", "DNF"};
        int stateIdx = static_cast<int>(m_state);
        const char* stateName =
            (stateIdx >= 0 && stateIdx < static_cast<int>(IM_ARRAYSIZE(stateNames))) ? stateNames[stateIdx] : "Unknown";
        ImGui::Text("State: %s", stateName);

        if (m_state == RaceState::Countdown)
            ImGui::Text("Countdown: %.1f", m_countdownTimer);

        ImGui::Text("Race Time: %.2f s", m_raceTime);
        ImGui::Text("Laps: %u", m_totalLaps);
        ImGui::Text("Racers: %zu", m_racers.size());
        ImGui::Separator();

        // Standings table
        for (const auto& racer : m_racers)
        {
            ImVec4 color = racer.isPlayer ? ImVec4(0.4f, 1.0f, 0.4f, 1.0f) : ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
            ImGui::TextColored(color, "P%u %s - Lap %u/%u - %.2fs", racer.position, racer.name.c_str(),
                               racer.currentLap, m_totalLaps, racer.totalTime);
        }
#endif
    }

} // namespace Racing
