/**
 * @file TFTravelSystem.h
 * @brief Continent/sanctuary travel: per-player mapId, sanctuary first-spawn
 *        placement, travel terminal UX, and the 0x5434-0x5437 travel channel.
 *
 * OWNERSHIP: continents lane (2026-07-10 warpgate/sanctuary wave), alongside
 * World/TFSanctuaryZone.h, Assets/Scenes/MMOFPS/sanctuary_haven.scene and
 * Assets/MMOFPS/Data/continents.json. Lifecycle mirrors every other TF system
 * (Initialize/Update/FixedUpdate/Shutdown/RenderDebugUI + RenderUI) and is
 * wired from Main.cpp by the integrator.
 *
 * ## PS2-style flow (play-test request 2026-07-10)
 *  - New sessions first-spawn in Sanctuary Haven (no combat, no capture): the
 *    server keeps a per-player mapId defaulting to sanctuary; EvPlayerSpawned
 *    for a sanctuary-mapped player queues a placement teleport onto a spawn
 *    pad (applied next authoritative FixedUpdate via TFServerSim::TeleportPawn
 *    — the spawn handler overwrites MoveState AFTER the synchronous event, so
 *    the deferred tick is load-bearing, not cosmetic).
 *  - Walking up to the sanctuary terminal ([E]) opens the continent-select
 *    menu (population + territory summary); confirming sends a TravelRequest.
 *  - The server validates (entered-world gate, alive, terminal proximity) and
 *    teleports the player to their faction's warpgate (skyanchor first spawn
 *    from regions.json), flipping their mapId to the continent.
 *
 * ## Architecture: why no replication filtering
 * See TFSanctuaryZone.h — sanctuary is a reserved coordinate zone of the ONE
 * shared sim ("position isolation"). Pawns in sanctuary still replicate to
 * everyone (they are 1.7 km from the nearest region — visually negligible,
 * and the zone rules make them inert). Chosen over per-mapId interest
 * filtering because filtering would touch TFReplication + TFRepProtocol +
 * TFServerSim + the AreaServer topology (all other-lane/contended files) for
 * zero v1 gameplay difference.
 *
 * ## Wire (reserved block 0x5434-0x5437; 0x5430-0x5433 belong to ui-map-keys)
 * Ids live here (not in the frozen TFNetProtocol.h) following the vehicle
 * channel precedent (kTFVehMsg_Purchase in TFRepProtocol.h): a raw uint16
 * riding NetworkManager's handler registry, server-authoritative. Packed +
 * static_asserted in TFTravelSystemWire.h (included below) — layouts are
 * frozen once merged.
 */
#pragma once

#include "Core/TFTypes.h"
#include "Core/TFEvents.h"
#include "World/TFSanctuaryZone.h"
#include "World/TFTravelSystemWire.h" // 0x5434-0x5437 ids, TFTravelErr, packed payloads

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Terrafront
{

    class TFSanctuaryDecor; // sanctuary-v2 lane (W10): World/TFSanctuaryDecor.h
    class TFTargetRange;    // sanctuary-v2 lane (W10): Game/TFTargetRange.h
    class TFTutorial;       // tutorial-flow lane (W12): Game/TFTutorial.h

    // ---------------------------------------------------------------------
    // System
    // ---------------------------------------------------------------------

    class TFTravelSystem
    {
      public:
        /// One continents.json continent row (display metadata; W12 continent-2-data:
        /// the terminal lists EVERY registered continent, but only the one whose
        /// region lattice this process loaded is joinable — one continent per
        /// server process; the others render as different-server destinations).
        struct ContinentMeta
        {
            int mapId{-1};
            std::string key; ///< continents.json key (== tf_continent boot value)
            std::string name;
            std::string blurb;
            bool active{false}; ///< true == the continent THIS process loaded

            // multimap-plumbing lane (W13): OPTIONAL operator-configured
            // endpoint for a continent hosted by a DIFFERENT server process
            // (continents.json "host"/"port" keys, additive; see
            // docs/TERRAFRONT_MULTIMAP.md). Empty host / zero port == no known
            // server for this continent — the terminal says so and the hop
            // button stays disabled. Never set for the active entry.
            std::string host;
            uint16_t port{0};
        };

        TFTravelSystem();
        ~TFTravelSystem();

        bool Initialize(TFGameContext& ctx, TFEventBus& events);
        void Update(float deltaTime);
        void FixedUpdate(float fixedDeltaTime);
        void Shutdown();
        void RenderDebugUI(); ///< tf_travel_debug panel (Main.cpp debug block)
        void RenderUI();      ///< player-facing prompt + continent menu (OnImGui, InWorld gate)

        // --- cross-lane surface -------------------------------------------

        /// Authoritative per-player mapId (server roles). Players never seen by
        /// this system default to the sanctuary.
        uint8_t ServerMapOf(PlayerId player) const;

        /// Local player's mapId on ANY role (positional derivation — see
        /// TFSanctuaryZone.h). kTFMapCindralWastes when there is no pawn.
        uint8_t LocalMapId() const;

        /// Convenience mirror of the full redeploy rule for UI/consoles: false
        /// in the sanctuary (use the terminal) or for non-friendly/contested
        /// regions. NOTE the authoritative wiring is TFRedeployRules::
        /// SetExtraRule (installed by Initialize into the ui-map-keys seam) —
        /// this helper never gates anything by itself.
        bool CanRedeployTo(RegionId region) const;

        /// Client/loopback request entry (terminal menu + tf_travel console).
        void ClientRequestTravel(uint8_t destMapId);

        /// multimap-plumbing lane (W13): client-side hop to a continent hosted
        /// by a DIFFERENT server process. No-op (sets m_lastTravelMsg) unless
        /// role==Client and connected — see docs/TERRAFRONT_MULTIMAP.md for
        /// the process-per-continent design and the known teardown-gap
        /// caveat. server-authoritative follow-up (§2.2): `target.host`/
        /// `target.port` are used only for the request's display name now —
        /// the actual endpoint is resolved by THIS client's current server
        /// (via TF_ContinentHopRequest/Reply) and applied on a later Update()
        /// tick once the reply arrives (see OnNetContinentHopReply).
        void ClientRequestContinentHop(const ContinentMeta& target);

        /// server-authoritative continent-hop follow-up (W13,
        /// docs/TERRAFRONT_MULTIMAP.md §2.2): registry lookup Net/TFServerSim.cpp
        /// calls to answer a client's TF_ContinentHopRequest with THIS
        /// process's own continents.json-sourced host/port for `mapId` — a
        /// connecting client's local copy of that file is no longer trusted
        /// for the actual endpoint, only used client-side as a display hint.
        /// Returns false (host/port untouched) when `mapId` is unregistered
        /// or has no configured endpoint.
        bool LookupContinentEndpoint(uint8_t mapId, std::string& outHost, uint16_t& outPort) const;

        /// Debug panel toggle (tf_* console pattern).
        void ToggleDebugUI() { m_showDebug = !m_showDebug; }

      private:
        // --- server side ---------------------------------------------------
        void OnPlayerSpawned(const EvPlayerSpawned& ev);
        void ServerPlacePending();       ///< FixedUpdate: apply queued sanctuary placements
        void ServerPruneStale(float dt); ///< drop mapId entries for gone sessions
        bool ServerWarpgateOf(FactionId faction, float outPos[3]) const;
        void ServerSanctuaryPadOf(PlayerId player, float outPos[3]) const;
        void ServerHandleTravel(PlayerId sender, const TF_TravelRequest& req);
        void ServerBuildContinentInfo(TF_ContinentInfo& out) const;

        // --- client side ---------------------------------------------------
        bool LocalPawn(float outPos[3], bool& outAlive) const;
        PlayerId LocalPlayerId() const;
        bool NearTerminal(const float pawnPos[3], float radiusM) const;
        void SetMenuOpen(bool open);
        void ClientRequestInfo();
        void ApplyTravelReply(const TF_TravelReply& rep);
        void ApplyContinentInfo(const TF_ContinentInfo& info);

        /// server-authoritative continent-hop follow-up (W13): deferred apply
        /// of an arrived TF_ContinentHopReply — called from Update(), NOT
        /// from OnNetContinentHopReply itself. Disconnecting/reconnecting
        /// synchronously inside the network message-dispatch callback would
        /// tear down the very socket that dispatch loop is iterating (the
        /// same reentrancy hazard OnPlayerSpawned avoids by deferring
        /// TeleportPawn to ServerPlacePending on the next fixed tick).
        void ApplyPendingContinentHop();

#ifdef ENABLE_NETWORKING
        bool ServerNetActive() const;
        bool ClientNetActive() const;
        void ServerEnsureNetHandlers();
        void ServerReleaseNetHandlers();
        void ClientEnsureNetHandlers();
        void ClientReleaseNetHandlers();
        void ServerHandleTravelPacket(PlayerId sender, const void* data, size_t size);
        void ServerHandleInfoRequestPacket(PlayerId sender, const void* data, size_t size);
        void ServerSendTo(PlayerId player, uint16_t msgId, const void* payload, size_t size);
        void OnNetTravelReply(const void* data, size_t size);
        void OnNetContinentInfo(const void* data, size_t size);
        /// server-authoritative continent-hop follow-up (W13): stashes the
        /// reply into m_hopReply* and sets m_hopReplyPending — see
        /// ApplyPendingContinentHop for why it isn't applied here directly.
        void OnNetContinentHopReply(const void* data, size_t size);
#endif

        void LoadContinentMeta(); ///< Assets/MMOFPS/Data/continents.json (display metadata only)
        void RegisterConsoleCommands();

        TFGameContext* m_ctx{nullptr};
        TFEventBus* m_events{nullptr};
        bool m_initialized{false};

        // server state
        std::unordered_map<PlayerId, uint8_t> m_mapOf; ///< authoritative per-player mapId
        std::unordered_set<PlayerId> m_pendingPlace;   ///< spawned-this-tick, teleport next FixedUpdate
        float m_pruneAccum{0.0f};
        uint32_t m_travels{0};
        uint32_t m_travelsRejected{0};
        uint32_t m_placements{0};
        uint32_t m_badPackets{0};

        // client state
        bool m_menuOpen{false};
        bool m_nearTerminal{false};
        float m_interactDebounce{0.0f};
        TF_ContinentInfo m_lastInfo{}; ///< menu population/territory summary
        bool m_hasInfo{false};
        double m_infoRequestedAt{-1.0};
        double m_clock{0.0};
        char m_lastTravelMsg[96]{}; ///< last reply, surfaced in the menu

        // server-authoritative continent-hop follow-up (W13,
        // docs/TERRAFRONT_MULTIMAP.md §2.2): request in flight + the reply,
        // once it arrives, staged for ApplyPendingContinentHop on the next
        // Update() tick (see that method's doc comment for why it's deferred).
        int m_hopRequestMapId{-1};    ///< mapId awaiting a reply; -1 == none in flight
        std::string m_hopRequestName; ///< display name for status text / logging
        bool m_hopReplyPending{false};
        bool m_hopReplyOk{false};
        std::string m_hopReplyHost;
        uint16_t m_hopReplyPort{0};

        // display metadata from continents.json (falls back to built-ins).
        // m_continentDisplayName/Blurb mirror the ACTIVE entry of m_continentList
        // (W12: the continent whose lattice TFDataTables loaded this process).
        std::string m_sanctuaryDisplayName{"Sanctuary Haven"};
        std::string m_continentDisplayName{"Cindral Wastes"};
        std::string m_continentBlurb{"The contested desert continent."};
        std::vector<ContinentMeta> m_continentList; ///< every registered continent

#ifdef ENABLE_NETWORKING
        bool m_serverHandlers{false};
        bool m_clientHandlers{false};
#endif
        bool m_showDebug{false};
        bool m_travelCommandRegistered{false};
        bool m_hopCommandRegistered{false};
        bool m_debugCommandRegistered{false};

        // sanctuary-v2 lane (W10): sanctuary decor + class terminal and the
        // cosmetic firing range are owned + driven here (TFRegionSystem::
        // m_decor precedent) so they inherit this system's lifecycle without
        // touching contended Main.cpp. Both are viewer-only and no-op on
        // dedicated servers.
        std::unique_ptr<TFSanctuaryDecor> m_sanctuaryDecor;
        std::unique_ptr<TFTargetRange> m_targetRange;

        // tutorial-flow lane (W12): the first-join sanctuary tutorial is
        // owned + driven here too (same precedent as the two members above)
        // so it inherits this system's lifecycle without touching contended
        // Main.cpp. Client-presentation-only; no-op on dedicated servers.
        std::unique_ptr<TFTutorial> m_tutorial;
    };

} // namespace Terrafront
