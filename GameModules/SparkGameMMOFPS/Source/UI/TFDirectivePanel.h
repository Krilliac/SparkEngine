/**
 * @file TFDirectivePanel.h
 * @brief Player-facing directives panel (J): active directives with live
 *        progress bars, per-tier XP/flux rewards, and paid-tier state.
 *
 * OWNERSHIP: ui-polish lane (W8) — this header + TFDirectivePanel.cpp.
 *
 * Data sources (server-authoritative, no client-side guessing):
 *  - Authority roles (listen host / standalone / dedicated): read
 *    TFDirectiveSystem::ForEachStatus directly — zero wire traffic.
 *  - Pure clients: a tiny request/snapshot mirror on this lane's reserved
 *    TFMsg block 0x544C-0x544F (ids + packed structs below, TFTravelSystem /
 *    TFOutfitSystem "constants, not enumerators" precedent so this lane
 *    compiles standalone). The panel registers its OWN NetworkManager
 *    handlers on both roles (TFVehicleNet pattern) — no TFServerSim edits.
 *
 * Claim buttons are deliberately ABSENT: TFDirectiveSystem pays every tier
 * automatically the moment its goal is crossed (see TFDirectiveSystem::
 * AddProgress -> GrantTier), so there is nothing to claim — the panel shows
 * completed tiers as PAID instead. Wiring a manual claim would mean changing
 * another lane's payout semantics (anti-bloat rule: reuse, don't rebuild).
 * Ids 0x544E-0x544F of the reserved block stay free for a future claim flow
 * if directives ever move to claim-on-demand.
 *
 * Keybind: TFKeys::Action::ToggleDirectives (default 'J') via the ui-map-keys
 * TFKeybinds seam — the enum/table addition itself ships in this lane's
 * wiringNotes (TFKeybinds.h belongs to the ui-map-keys lane).
 */
#pragma once

#include "Core/TFTypes.h"
#include "Core/TFEvents.h"
#include "Game/TFDirectiveData.h"

#include <array>
#include <cstdint>

namespace Terrafront
{

    // ---------------------------------------------------------------------------
    // Wire protocol — ui-polish reserved block 0x544C-0x544F (0x544E/0x544F free)
    // ---------------------------------------------------------------------------

    /// C->S: "send me my directive snapshot". Empty payload (CharListRequest
    /// precedent — SendMsg(nullptr, 0)); the sender IS the subject player.
    constexpr uint16_t kTFMsg_DirectiveStatusReq = 0x544C;

    /// S->C: TF_DirectiveStatus reply (full per-player snapshot, reliable).
    constexpr uint16_t kTFMsg_DirectiveStatus = 0x544D;

#pragma pack(push, 1)

    /// One directive's per-player standing on the wire.
    struct TF_DirectiveStatusEntry
    {
        uint16_t directiveId; ///< TFDirectiveDef::id (stable content key)
        uint8_t tiersDone;    ///< tiers already paid (0..kTFDirectiveTiers)
        uint8_t _pad;
        uint32_t progress; ///< cumulative progress
    };
    static_assert(sizeof(TF_DirectiveStatusEntry) == 8, "wire layout frozen");

    /// Fixed wire capacity — headroom over kTFDirectiveCount (6 today) so the
    /// ADDITIVE-only directive table can grow without a layout break. Clients
    /// ignore ids they don't know (older client vs newer server table).
    constexpr size_t kTFDirectiveStatusMax = 16;

    struct TF_DirectiveStatus
    {
        uint8_t count; ///< valid leading entries (<= kTFDirectiveStatusMax)
        uint8_t _pad[3];
        TF_DirectiveStatusEntry entries[kTFDirectiveStatusMax];
    };
    static_assert(sizeof(TF_DirectiveStatus) == 132, "wire layout frozen");
    static_assert(kTFDirectiveCount <= kTFDirectiveStatusMax,
                  "directive table outgrew the status wire capacity — bump kTFDirectiveStatusMax (layout break!)");

#pragma pack(pop)

    // ---------------------------------------------------------------------------
    // Panel
    // ---------------------------------------------------------------------------

    class TFDirectivePanel
    {
      public:
        TFDirectivePanel();
        ~TFDirectivePanel();

        bool Initialize(TFGameContext& ctx, TFEventBus& events);
        void Update(float deltaTime);
        void FixedUpdate(float fixedDeltaTime);
        void Shutdown();
        void RenderDebugUI();
        void RenderUI(); ///< player-facing (Main.cpp OnImGui, InWorld-gated)

        bool IsOpen() const { return m_open; }
        void Toggle();
        void Close();

      private:
        /// Row the renderer consumes, identical on every role.
        struct Row
        {
            const TFDirectiveDef* def{nullptr};
            uint32_t progress{0};
            uint8_t tiersDone{0};
        };

        /// Fill `m_rows` from the local system (authority) or the mirror (client).
        void BuildRows();
        bool LocalPawnAlive() const;

#ifdef ENABLE_NETWORKING
        bool ServerNetActive() const;
        bool ClientNetActive() const;
        void ServerEnsureHandlers();
        void ServerReleaseHandlers();
        void ClientEnsureHandlers();
        void ClientReleaseHandlers();
        void ServerOnStatusRequest(PlayerId sender, const void* data, size_t size);
        void ClientOnStatus(const void* data, size_t size);
        void ClientPumpRequests(float dt);
#endif

        TFGameContext* m_ctx{nullptr};
        TFEventBus* m_events{nullptr};
        bool m_initialized{false};

        bool m_open{false};
        bool m_releasedMouse{false};

        std::array<Row, kTFDirectiveCount> m_rows{};

        // Pure-client mirror of the last TF_DirectiveStatus snapshot.
        TF_DirectiveStatus m_remote{};
        bool m_hasRemote{false};
        float m_remoteAge{0.0f};    ///< seconds since the last snapshot arrived
        float m_requestAccum{0.0f}; ///< 1 Hz request pacing while open

        bool m_serverHandlers{false};
        bool m_clientHandlers{false};

        // debug counters
        uint32_t m_requestsServed{0};
        uint32_t m_snapshotsApplied{0};
        uint32_t m_badPackets{0};
    };

    /// Pure-client snapshot request cadence while the panel is open.
    constexpr float kTFDirectiveRequestSec = 1.0f;

} // namespace Terrafront
