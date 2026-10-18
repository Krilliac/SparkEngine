/**
 * @file OWPlayerController.cpp
 * @brief Open world player movement, sprint, and interaction input
 */

#include "OWPlayerController.h"
#include "OWPlayerSystem.h"
#include "Events/OWDynamicEventSystem.h"
#include "Gathering/OWGatheringSystem.h"
#include "Settlement/OWSettlementSystem.h"
#include "World/OWWorldSetup.h"
#include "Input/InputManager.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace OpenWorld
{

    void OWPlayerController::Initialize(Spark::IEngineContext* context, OWPlayerSystem& player,
                                        const OWWorldSetup& world, OWGatheringSystem& gathering,
                                        OWDynamicEventSystem& events, OWSettlementSystem& settlements)
    {
        m_context = context;
        m_player = &player;
        m_world = &world;
        m_gathering = &gathering;
        m_events = &events;
        m_settlements = &settlements;
    }

    void OWPlayerController::SetMoveInput(float forward, float strafe, bool sprint)
    {
        m_forward = std::clamp(forward, -1.0f, 1.0f);
        m_strafe = std::clamp(strafe, -1.0f, 1.0f);
        m_sprintRequested = sprint;
    }

    void OWPlayerController::SetTurnInput(float yawDegreesPerSecond)
    {
        m_turnRate = yawDegreesPerSecond;
    }

    void OWPlayerController::Update([[maybe_unused]] float deltaTime)
    {
        InputManager* input = m_context ? m_context->GetInput() : nullptr;
        if (!input)
        {
            return;
        }

        constexpr int kShift = 0x10;
        auto axis = [input](int positive, int negative)
        { return (input->IsKeyDown(positive) ? 1.0f : 0.0f) - (input->IsKeyDown(negative) ? 1.0f : 0.0f); };
        SetMoveInput(axis('W', 'S'), axis('D', 'A'), input->IsKeyDown(kShift));
        SetTurnInput(axis('E', 'Q') * kKeyboardTurnRate);

        const bool interactHeld = input->IsKeyDown('F');
        if (interactHeld && !m_interactHeld)
        {
            TryInteract();
        }
        m_interactHeld = interactHeld;
    }

    void OWPlayerController::FixedUpdate(float fixedDeltaTime)
    {
        if (!m_player || !m_world)
        {
            return;
        }
        if (!m_player->IsAlive())
        {
            m_player->SetLocomotion(0.0f, false);
            return;
        }

        const PlayerWorldState& state = m_player->GetWorldState();
        m_player->SetFacing(state.yaw + m_turnRate * fixedDeltaTime);

        // Sprinting to zero stamina locks sprint until it recovers or the key is released,
        // so an exhausted player walks instead of flickering between speeds every step.
        const float stamina = m_player->GetSurvivalState().stamina;
        if (!m_sprintRequested || stamina >= kSprintRecoverStamina)
        {
            m_sprintExhausted = false;
        }
        else if (stamina <= 0.0f)
        {
            m_sprintExhausted = true;
        }

        const float inputLength = std::sqrt(m_forward * m_forward + m_strafe * m_strafe);
        if (inputLength <= 0.0f)
        {
            m_player->SetLocomotion(0.0f, false);
            return;
        }
        const float forward = m_forward / std::max(1.0f, inputLength);
        const float strafe = m_strafe / std::max(1.0f, inputLength);
        const bool sprinting = m_sprintRequested && !m_sprintExhausted;
        const float baseSpeed = sprinting ? kSprintSpeed : kWalkSpeed;

        // Yaw 0 faces north (+Z) and 90 faces east (+X), matching the compass.
        constexpr float kDegToRad = std::numbers::pi_v<float> / 180.0f;
        const float yawRad = state.yaw * kDegToRad;
        const float sinYaw = std::sin(yawRad);
        const float cosYaw = std::cos(yawRad);
        const float step = baseSpeed * fixedDeltaTime;
        const float nextX = state.posX + (forward * sinYaw + strafe * cosYaw) * step;
        const float nextZ = state.posZ + (forward * cosYaw - strafe * sinYaw) * step;

        // The authored regions are the whole walkable world: a step that leaves them is refused.
        const BiomeRegion* region = m_world->GetRegionAtPosition(nextX, nextZ);
        if (!region)
        {
            m_player->SetLocomotion(0.0f, false);
            return;
        }
        m_player->SetPosition(nextX, region->elevationMin, nextZ);
        m_player->SetCurrentRegion(region->regionId);
        m_player->SetLocomotion(baseSpeed * std::min(1.0f, inputLength), sprinting);
    }

    InteractResult OWPlayerController::TryInteract()
    {
        InteractResult result;
        if (m_player && m_gathering && m_events && m_settlements && m_player->IsAlive())
        {
            const PlayerWorldState& state = m_player->GetWorldState();
            if (const uint32_t nodeId = m_gathering->FindNearestHarvestableNode(state.posX, state.posZ, kHarvestRange))
            {
                result = {InteractResult::Kind::Harvest, nodeId, m_gathering->HarvestNode(nodeId)};
            }
            else if (const uint32_t eventId = m_events->FindJoinableEventNear(state.posX, state.posZ, kEventJoinRange))
            {
                if (m_events->JoinEvent(eventId))
                {
                    result = {InteractResult::Kind::JoinEvent, eventId, 0};
                }
            }
            else if (const Settlement* settlement = m_settlements->FindSettlementAt(state.posX, state.posZ))
            {
                if (m_settlements->VisitSettlement(settlement->settlementId))
                {
                    result = {InteractResult::Kind::VisitSettlement, settlement->settlementId, 0};
                }
            }
        }
        return result;
    }

} // namespace OpenWorld
