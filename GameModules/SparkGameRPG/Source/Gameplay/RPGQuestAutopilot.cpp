/**
 * @file RPGQuestAutopilot.cpp
 * @brief Thornwood hunting autopilot over RPGDemoSession (see RPGQuestAutopilot.h)
 */

#include "RPGQuestAutopilot.h"

#include "Engine/Gameplay/QuestSystem.h"
#include "RPGDemoSession.h"

#include <utility>

namespace RPG
{
    namespace
    {
        constexpr uint32_t kOakhollowArea = 1; ///< Safe zone with the inn (RPGWorldSetup)
        constexpr uint32_t kThornwoodArea = 2; ///< Hunting ground connected to Oakhollow

        bool Contains(const std::string& text, const char* needle)
        {
            return text.find(needle) != std::string::npos;
        }
    } // namespace

    RPGQuestAutopilot::RPGQuestAutopilot(uint32_t questId, std::string quarry)
        : m_questId(questId), m_quarry(std::move(quarry))
    {
    }

    RPGQuestAutopilot::Outcome RPGQuestAutopilot::Step(RPGDemoSession& session)
    {
        if (m_outcome != Outcome::Running)
        {
            return m_outcome;
        }

        const auto state =
            Spark::Gameplay::QuestSystem::GetInstance().GetQuestState(session.GetPlayerCharacterId(), m_questId);
        if (state == Spark::Gameplay::QuestState::Completed)
        {
            m_outcome = Outcome::Completed;
            return m_outcome;
        }

        if (session.IsInCombat())
        {
            if (!m_fightingQuarry)
            {
                m_lastResult = session.Flee();
                m_needsRest = true;
                return m_outcome;
            }

            // A refused attack ("still on cooldown") changes nothing; the next frame tries again.
            m_lastResult = session.Attack();
            if (Contains(m_lastResult, "Rowan was defeated"))
            {
                m_outcome = Outcome::Defeated;
            }
            else if (Contains(m_lastResult, "enemy defeated"))
            {
                ++m_kills;
                m_needsRest = true;
            }
            return m_outcome;
        }

        if (m_needsRest)
        {
            if (session.GetCurrentAreaId() != kOakhollowArea)
            {
                m_lastResult = session.Travel(kOakhollowArea);
                if (session.GetCurrentAreaId() != kOakhollowArea)
                {
                    m_outcome = Outcome::Stalled;
                }
                return m_outcome;
            }
            m_lastResult = session.Rest();
            if (!Contains(m_lastResult, "restored"))
            {
                m_outcome = Outcome::Stalled;
            }
            m_needsRest = false;
            return m_outcome;
        }

        if (m_trips >= MaxTrips)
        {
            m_outcome = Outcome::TripLimit;
            return m_outcome;
        }
        ++m_trips;
        m_lastResult = session.Travel(kThornwoodArea);
        if (!session.IsInCombat())
        {
            // Thornwood always holds an encounter; arriving without one means the loop cannot progress.
            m_outcome = Outcome::Stalled;
            return m_outcome;
        }
        m_fightingQuarry = m_quarry.empty() || m_lastResult.find(m_quarry) != std::string::npos;
        return m_outcome;
    }

    std::string RPGQuestAutopilot::GetStatusString() const
    {
        const char* outcome = "running";
        switch (m_outcome)
        {
        case Outcome::Running:
            break;
        case Outcome::Completed:
            outcome = "completed";
            break;
        case Outcome::Defeated:
            outcome = "stopped: Rowan was defeated";
            break;
        case Outcome::TripLimit:
            outcome = "stopped: trip limit reached";
            break;
        case Outcome::Stalled:
            outcome = "stopped: stalled";
            break;
        }
        return "Autoplay quest " + std::to_string(m_questId) + ": " + outcome + " (trips " + std::to_string(m_trips) +
               ", kills " + std::to_string(m_kills) + "); last: " + (m_lastResult.empty() ? "-" : m_lastResult);
    }
} // namespace RPG
