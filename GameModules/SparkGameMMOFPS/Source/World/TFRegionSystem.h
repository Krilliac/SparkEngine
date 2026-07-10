/**
 * @file TFRegionSystem.h
 * @brief Hex regions, conduit lattice, capture points, Dominion.
 *
 * OWNERSHIP: this header + TFRegionSystem*.cpp belong to ONE implementation agent.
 * The lifecycle below is the frozen module contract (called from Main.cpp) —
 * extend this class freely, but do not change the lifecycle signatures.
 *
 * W2 implementation (split across TFRegionSystem.cpp = authoritative capture
 * loop + accessors + debug UI, and TFRegionSystemNet.cpp = wire broadcasts,
 * client mirror handlers and territory persistence):
 *  - Server (authority roles): 1 Hz capture tick over the alive-pawn set
 *    (10 m XZ radius around any capture point). Single attacker faction that
 *    is lattice-linked advances progress at 1/captureSec; several distinct
 *    factions on the point => contested (frozen); defenders alone decay the
 *    progress at 2x; an empty point bleeds back at 1x. Progress 1.0 flips the
 *    owner: EvRegionCaptured, reliable TF_RegionState broadcast, capture XP
 *    (500/250/100 by facility/fort/outpost, reason 3) to every attacker in
 *    radius, atomic persist.
 *  - TF_CaptureTick (unreliable) broadcast whenever a region's progress/
 *    contested/capturing changed this tick; full reliable TF_RegionState
 *    burst to every newly connected client (GetClients() diff poll, the
 *    TFReplication pattern).
 *  - Dominion: one faction owns every non-skyanchor region => EvDominion +
 *    full state broadcast, 10 min hold (captures frozen), then soft-reset to
 *    regions.json initialOwnership and persist.
 *  - Persistence: Saves/terrafront_territory.json next to the exe, written
 *    atomically (tmp + rename) on every ownership change and every 30 s when
 *    dirty; loaded on boot (falls back to initialOwnership when absent/stale).
 *  - Client: handlers for TF_RegionState/TF_CaptureTick keep a mirror store
 *    behind the SAME accessors, and the local player's capture-radius status
 *    feeds TFHUD::SetCaptureProgress each frame (all roles with a local player).
 *    The HUD feed uses the SAME eligibility predicate as the server tick
 *    (playable faction + the point can react to that faction) so the capture
 *    indicator never shows where progress cannot move (2026-07-10 play-test:
 *    faction-less/owned/unlinked points showed a forever-frozen bar).
 *  - Diagnosis: tf_capture_debug (registered by Initialize) dumps per-region
 *    occupants by faction, capturing/progress/contested and, on clients, the
 *    age of the last replicated progress packet. On authorities it also trips
 *    a loud "NO ENGINE ECS WORLD" banner when IEngineContext::GetWorld() is
 *    null — the 2026-07-10 "capturing isn't working" root cause: the headless
 *    (dedicated-server) engine boot registered no World service, so every
 *    pawn's position read collapsed to (0,0,0) and no point ever saw an
 *    occupant. Capture logic itself was verified correct (windowed control
 *    run: occupancy -> progress -> flip on the same build).
 */
#pragma once

#include "Core/TFTypes.h"
#include "Core/TFEvents.h"
#include "Net/TFNetProtocol.h"

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace Terrafront
{

    struct RegionDef; // Data/TFDataTables.h

    class TFRegionSystem
    {
      public:
        TFRegionSystem();
        ~TFRegionSystem();

        bool Initialize(TFGameContext& ctx, TFEventBus& events);
        void Update(float deltaTime);
        void FixedUpdate(float fixedDeltaTime);
        void Shutdown();
        void RenderDebugUI();

        // --- W2 cross-agent contract (FROZEN). Served from the authoritative
        //     state on server roles and from the TF_RegionState/TF_CaptureTick
        //     mirror on pure clients — callers never need to care which. --------
        FactionId OwnerOf(RegionId region) const;
        /// Lattice rule: non-skyanchor, not already owned by `attacker`, and
        /// conduit-linked to at least one region `attacker` owns.
        bool IsCapturable(RegionId region, FactionId attacker) const;
        float CaptureProgress(RegionId region, FactionId& outCapturing, bool& outContested) const;
        uint32_t RegionsHeld(FactionId faction) const;
        /// Owned + conduit-linked: region belongs to `faction` AND is connected to
        /// the faction's skyanchor through a chain of owned regions.
        bool CanSpawnAt(RegionId region, FactionId faction) const;

        // --- W2 public surface beyond the contract (console wiring is the
        //     ORCHESTRATOR's; these are the clean entry points) ----------------
        uint32_t RegionCount() const { return static_cast<uint32_t>(m_state.size()); }
        /// FNV-1a over the owner list — the TF_WorldWelcome.territoryHash source.
        uint32_t TerritoryHash() const;
        /// True while a Dominion hold is running; outputs the locking faction and
        /// the seconds until the soft reset.
        bool DominionActive(FactionId& outFaction, float& outSecondsLeft) const;
        /// Debug/console owner override (authority only, skyanchors refused).
        /// Fires the same events/broadcasts/persist as a real capture, no XP.
        bool ServerForceOwner(RegionId region, FactionId newOwner);
        /// Force an immediate atomic persist (authority only).
        bool SaveNow();
        /// Debug panel toggle (hidden by default; wired from tf_* console commands).
        void ToggleDebugUI() { m_showDebug = !m_showDebug; }

      private:
        /// Runtime per-region state: the server truth on authority roles, the
        /// broadcast-fed mirror on pure clients.
        struct RegionState
        {
            FactionId owner = FactionId::None;
            FactionId capturing = FactionId::None;
            float progress = 0.0f; ///< 0..1 toward `capturing`
            bool contested = false;
            double lastNetAt = -1.0; ///< client mirror only: m_time when the last
                                     ///< RegionState/CaptureTick arrived (-1 = never)
        };

        // topology / seeding (TFRegionSystem.cpp)
        void EnsureState(); ///< lazy (re)build when table size changes
        void RebuildFromData(bool preserveOwners);
        void OnDataReloaded();

        // authoritative capture loop (TFRegionSystem.cpp)
        void TickCapture(float dt);
        void FlipOwner(size_t idx, FactionId newOwner, bool awardXp);
        void AwardCaptureXP(const RegionDef& def, FactionId newOwner);
        void CheckDominion();
        void StartDominion(FactionId faction);
        void SoftResetToInitial(const char* reason);

        // local-player HUD feed (TFRegionSystem.cpp)
        void FeedLocalCaptureHUD();

        // Client-side capture-point landmarks: a cap tower + a slowly rotating
        // banner ring per non-skyanchor region, tinted by the current owner.
        // Spawned once, then rotated/retinted each frame. Viewer-only (gated on
        // HasLocalPlayer) so the dedicated server never builds decor.
        void UpdateCaptureVisuals(float dt);
        bool m_capVisualsSpawned = false;
        std::vector<uint32_t> m_capBannerEnt; ///< region-indexed banner entity (0 = none)
        std::vector<int> m_capBannerOwner;    ///< last-applied owner tint per banner

        // tf_capture_debug payload (TFRegionSystem.cpp)
        std::string DebugCaptureReport() const;

        // persistence (TFRegionSystemNet.cpp)
        std::string SavePath() const;
        bool LoadPersisted();
        bool PersistNow();

#ifdef ENABLE_NETWORKING
        // wire (TFRegionSystemNet.cpp)
        bool ServerNetActive() const;
        bool ClientNetActive() const;
        void ServerPollNewClients();
        /// target == kInvalidPlayer broadcasts to all clients.
        void SendFullStateTo(PlayerId target);
        void SendRegionState(PlayerId target, size_t idx, bool reliable);
        void SendCaptureTick(size_t idx);
        void SendNet(PlayerId target, uint16_t msgId, const void* payload, size_t size, bool reliable);
        void EnsureClientHandlers();
        void ReleaseClientHandlers();
        void OnNetRegionState(const void* data, size_t size);
        void OnNetCaptureTick(const void* data, size_t size);
#endif

        TFGameContext* m_ctx{nullptr};
        TFEventBus* m_events{nullptr};
        bool m_initialized{false};

        std::vector<RegionState> m_state;
        double m_time{0.0};          ///< fixed-step clock (dominion timing)
        float m_captureAccum{0.0f};  ///< 1 Hz capture tick accumulator
        float m_saveAccum{0.0f};     ///< 30 s dirty-save accumulator
        bool m_dirty{false};         ///< unsaved ownership/dominion change
        bool m_persistLoaded{false}; ///< boot load attempted once

        // Dominion hold
        bool m_domActive{false};
        FactionId m_domFaction{FactionId::None};
        double m_domEndsAt{0.0};

#ifdef ENABLE_NETWORKING
        std::unordered_set<PlayerId> m_knownClients; ///< server: burst bookkeeping
        bool m_clientHandlers{false};
#endif

        // stats / debug
        uint32_t m_flips{0};
        uint32_t m_ticksSent{0};
        uint32_t m_stateRx{0};
        uint32_t m_tickRx{0};
        uint32_t m_badPackets{0};
        bool m_showDebug{false};
        bool m_debugCmd{false}; ///< tf_capture_debug registered by this instance
    };

} // namespace Terrafront
