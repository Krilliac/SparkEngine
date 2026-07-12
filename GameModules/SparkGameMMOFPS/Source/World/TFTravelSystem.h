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
 * static_asserted below — layouts are frozen once merged.
 */
#pragma once

#include "Core/TFTypes.h"
#include "Core/TFEvents.h"
#include "World/TFSanctuaryZone.h"

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
    // Travel wire channel (reserved TFMsg block 0x5434-0x5437, C<->S)
    // ---------------------------------------------------------------------

    constexpr uint16_t kTFTravelMsg_Request = 0x5434;     ///< C->S TF_TravelRequest
    constexpr uint16_t kTFTravelMsg_Reply = 0x5435;       ///< S->C TF_TravelReply
    constexpr uint16_t kTFTravelMsg_InfoRequest = 0x5436; ///< C->S empty payload
    constexpr uint16_t kTFTravelMsg_Info = 0x5437;        ///< S->C TF_ContinentInfo

    /// TF_TravelReply.reason
    enum class TFTravelErr : uint8_t
    {
        Ok = 0,
        BadMap = 1,       ///< unknown/disabled destination mapId
        NotEntered = 2,   ///< session has not passed the enter-world gate
        NotAlive = 3,     ///< no alive pawn (spawn first)
        AlreadyThere = 4, ///< destination == current mapId
        NoTerminal = 5,   ///< not within kTFTravelTerminalUseM of the terminal
        ServerError = 6,  ///< missing system/data on the server
    };

#pragma pack(push, 1)

    struct TF_TravelRequest
    {
        uint8_t mapId; ///< destination (kTFMap*)
        uint8_t _pad[3];
    };
    static_assert(sizeof(TF_TravelRequest) == 4, "wire layout frozen");

    struct TF_TravelReply
    {
        uint8_t accepted; ///< 0/1
        uint8_t reason;   ///< TFTravelErr
        uint8_t mapId;    ///< destination echoed back
        uint8_t _pad;
        float posX, posY, posZ; ///< arrival position when accepted
    };
    static_assert(sizeof(TF_TravelReply) == 16, "wire layout frozen");

    /// Per-continent summary for the terminal menu. Arrays are indexed by
    /// FactionId (0 = None slot unused but kept so indexing is branch-free).
    struct TF_ContinentInfo
    {
        uint8_t mapId;
        uint8_t _pad;
        uint16_t pop[4];  ///< alive pawns on the continent per faction
        uint16_t held[4]; ///< regions held per faction (TFRegionSystem)
    };
    static_assert(sizeof(TF_ContinentInfo) == 18, "wire layout frozen");

#pragma pack(pop)

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
        /// by a DIFFERENT server process (continents.json host/port). No-op
        /// (sets m_lastTravelMsg) unless role==Client and target has a
        /// configured endpoint — see docs/TERRAFRONT_MULTIMAP.md for the
        /// process-per-continent design and the known teardown-gap caveat.
        void ClientRequestContinentHop(const ContinentMeta& target);

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
