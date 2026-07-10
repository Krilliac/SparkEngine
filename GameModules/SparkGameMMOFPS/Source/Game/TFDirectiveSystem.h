/**
 * @file TFDirectiveSystem.h
 * @brief Server-tracked per-player directives (missions) with tiered rewards.
 *
 * OWNERSHIP: this header + TFDirectiveSystem.cpp (+ TFDirectiveData.h) belong
 * to ONE implementation agent. The lifecycle below is the uniform module
 * contract (called from Main.cpp) — extend this class freely, but do not
 * change the lifecycle signatures.
 *
 * Design (kept lean — one system class + one data header):
 *  - Definitions are code tables in TFDirectiveData.h (kill infantry /
 *    headshots, capture regions, destroy vehicles incl. a per-VehicleId
 *    variant, place deployables), three reward tiers each.
 *  - Progress feeds off EXISTING bus events, authority-only:
 *      EvPlayerKilled     -> KillInfantry (same suicide/teamkill filter as
 *                            TFProgressionSystem::OnPlayerKilled)
 *      EvVehicleDestroyed -> DestroyVehicles (param filters on VehicleId)
 *      EvXPAwarded        -> CaptureRegions via the canonical capture reason
 *                            codes 4/5/6 (per-player capture credit already
 *                            flows through TFRegionSystem::AwardCaptureXP ->
 *                            ServerAwardXP, so no new event is needed)
 *    Deployable placements have NO bus event yet, so DeployItems progress
 *    diff-polls TFDeployableSystem::ForEachDeployable (0.5 s cadence, first
 *    poll seeds the baseline without crediting). If a later wave adds an
 *    EvDeployablePlaced event, swap the poll for a subscription.
 *  - Tier payouts go through TFProgressionSystem's REAL API only:
 *    ServerAwardXP(player, tier.xp, kXPReasonDirective) — which also pushes
 *    the existing TF_XPEvent toast to the owning client — plus
 *    ServerGrantFlux(player, tier.flux) (wallet-capped there). Re-entrancy:
 *    ServerAwardXP synchronously fires EvXPAwarded back at this system;
 *    OnXPAwarded filters kXPReasonDirective FIRST, so payouts never
 *    self-count (see the comment in the handler before reordering it).
 *  - State is session-scoped per PlayerId (like W2 progression pre-W5); no
 *    disk persistence this wave. ClearPlayer() exists for the TFServerSim
 *    disconnect sweep so recycled PlayerIds do not inherit progress
 *    (wired by the integrator; see the lane wiring notes).
 */
#pragma once

#include "Core/TFTypes.h"
#include "Core/TFEvents.h"
#include "Game/TFDirectiveData.h"

#include <array>
#include <functional>
#include <unordered_map>
#include <unordered_set>

namespace Terrafront
{

    class TFDirectiveSystem
    {
      public:
        TFDirectiveSystem();
        ~TFDirectiveSystem();

        bool Initialize(TFGameContext& ctx, TFEventBus& events);
        void Update(float deltaTime);
        void FixedUpdate(float fixedDeltaTime);
        void Shutdown();
        void RenderDebugUI();

        // --- cross-system surface ----------------------------------------------

        /// One player's standing on one directive (def points into kTFDirectives).
        struct Status
        {
            const TFDirectiveDef* def;
            uint32_t progress;
            uint8_t tiersDone; ///< 0..kTFDirectiveTiers already paid
        };

        /// Visit every directive with this player's progress (zeros if unknown).
        /// Server truth — meaningful on the authority (UI on listen host /
        /// standalone; a client-facing mirror is a later-wave net message).
        void ForEachStatus(PlayerId player, const std::function<void(const Status&)>& fn) const;

        /// Cumulative progress on directive `directiveId` (0 if unknown id/player).
        uint32_t ProgressOf(PlayerId player, uint16_t directiveId) const;

        /// Paid tier count on directive `directiveId` (0 if unknown id/player).
        uint8_t TiersDoneOf(PlayerId player, uint16_t directiveId) const;

        /// Drop a player's directive state (disconnect sweep; recycled PlayerIds
        /// must not inherit the prior occupant's progress).
        void ClearPlayer(PlayerId player);

      private:
        struct Rec
        {
            std::array<uint32_t, kTFDirectiveCount> progress{};
            std::array<uint8_t, kTFDirectiveCount> tiersDone{};
        };

        void OnPlayerKilled(const EvPlayerKilled& ev);
        void OnVehicleDestroyed(const EvVehicleDestroyed& ev);
        void OnXPAwarded(const EvXPAwarded& ev);

        /// Add `amount` to every directive of `kind` whose param accepts `value`,
        /// paying any newly crossed tiers.
        void AddProgress(PlayerId player, TFDirectiveKind kind, uint8_t value, uint32_t amount);
        void GrantTier(PlayerId player, const TFDirectiveDef& def, uint8_t tierIdx);
        void PollDeployables(float dt);

        static bool Matches(const TFDirectiveDef& def, uint8_t value);
        static int IndexOfId(uint16_t directiveId); ///< -1 if unknown

        TFGameContext* m_ctx{nullptr};
        TFEventBus* m_events{nullptr};
        bool m_initialized{false};

        std::unordered_map<PlayerId, Rec> m_players;

        // DeployItems diff-poll state (no placement bus event exists yet).
        std::unordered_set<EntityId> m_knownDeployables;
        float m_deployPollAccum{0.0f};
        bool m_deploySeeded{false};

        // debug counters
        uint32_t m_progressTicks{0};
        uint32_t m_tiersGranted{0};
    };

} // namespace Terrafront
