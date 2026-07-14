/**
 * @file TFPingSystem.h
 * @brief Squad-scoped tactical pings (W11): server-validated placement, squad
 *        rebroadcast, client mirror store, and the 3D world-marker rendering
 *        (billboarded diamond, TFGroundFx recipe).
 *
 * OWNERSHIP: ping-system lane (this header + TFPingSystem.cpp + the
 * UI/TFPingUI.h/.cpp input/label layer).
 *
 * Design (TFGrenadeSystem precedent: server registry + client mirror in one
 * class; wire ids + packed structs live HERE, not in TFNetProtocol.h):
 *  - SERVER (authority roles): a placement request (C->S TF_PingPlace via the
 *    TFServerSim::RouteClientMessage enter-world-gated choke point — wiring
 *    snippet in the wave report) is validated: alive pawn, finite payload,
 *    position within kTFPingMaxRangeM (+ slack) of the sender's authoritative
 *    pawn, kTFPingIntervalSec per-player rate limit. Enemy pings must name a
 *    live enemy-faction pawn entity (position snaps to the SERVER's pawn
 *    position — client data is never trusted for it) and are refused for
 *    veiled Ghosts (TFAbilitySystem::IsPawnVeiled — a ping may never out a
 *    cloak, parity with TFNameplates). Need-Support pings snap to the
 *    sender's own pawn. Placing beyond kTFMaxLivePingsPerPlayer live pings
 *    replaces the sender's oldest (modern-FPS convention, cap preserved).
 *  - SCOPE: S->C TF_PingState (add/remove) goes ONLY to the placer's squad
 *    members (TFSquadSystem::SquadOf over the server registry); solo players
 *    receive their own pings and nothing else. Membership is evaluated per
 *    event — a player who joins the squad mid-life misses the add (pings live
 *    <= 10 s; documented gap, not a bug), one who leaves keeps the marker
 *    until its client-side lifetime runs out. The listen-host/standalone
 *    local player is fed by a direct mirror call (TFGrenadeSystem::
 *    ServerBroadcast pattern).
 *  - CLIENT: mirror keyed by ping id; entries age out locally from the echoed
 *    lifeSec so a lost remove packet cannot leave a stale marker. Handlers
 *    self-register when the socket connects (TFAbilitySystem
 *    EnsureClientHandlers pattern) — no TFClientNet edits.
 *  - RENDER: RenderClientFx draws each live ping as an additive camera-facing
 *    diamond (unit plane rotated 45 deg in the billboard basis, TFGroundFx /
 *    TFGrenadeSystem boom recipe) with a gentle vertical bob, colored by ping
 *    type, fading over the last seconds of life. Called once per frame from
 *    TFWorldSetup::RenderWorld (wiring snippet) while BeginFrame is active;
 *    restores the blend/depth/texture state it touches. Distance labels and
 *    the Q-key input layer live in UI/TFPingUI (this lane's second file);
 *    map/minimap icons are wiring snippets consuming ForEachActivePing.
 *
 * Determinism contract: pure comms/presentation — never touches movement,
 * collision, or damage state on either role.
 */
#pragma once

#include "Core/TFTypes.h"
#include "Core/TFEvents.h"

#include "Core/Platform.h" // DirectXMath on Windows / vector-math stubs on Linux
#ifdef SPARK_PLATFORM_WINDOWS
#include "Core/Platform.h"
#endif

#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

class GraphicsEngine;
class Mesh;

namespace Terrafront
{

    // ---------------------------------------------------------------------------
    // Wire protocol — reserved TFMsg id block 0x5468-0x546B (ping-system lane).
    // TFNetProtocol.h (contended) only gains the C->S enum entry via the wave
    // wiringNotes; these constants keep this lane compiling standalone and
    // MUST stay value-identical to any future enum entries.
    // ---------------------------------------------------------------------------

    constexpr uint16_t kTFMsgPingPlace = 0x5468; // C->S  TF_PingPlace (server validates)
    constexpr uint16_t kTFMsgPingState = 0x5469; // S->C  TF_PingState (add/remove, squad-scoped)
    // 0x546A-0x546B free (reserved to this lane).

    // ---------------------------------------------------------------------------
    // Ping kinds + tuning (gameplay contract from the wave brief)
    // ---------------------------------------------------------------------------

    enum class TFPingType : uint8_t
    {
        Location = 0, ///< yellow — "look here" world point (Q tap on world geometry)
        Enemy,        ///< red — spotted enemy pawn (Q tap while aiming at one)
        NeedSupport,  ///< blue — "help me here", anchored at the placer's pawn
        COUNT
    };

    constexpr float kTFPingMaxRangeM = 300.0f;       ///< context-ping raycast + server range gate
    constexpr float kTFPingRangeSlackM = 25.0f;      ///< server slack (eye vs feet + latency drift)
    constexpr float kTFPingLifetimeSec = 10.0f;      ///< Location / Need-Support lifetime
    constexpr float kTFPingEnemyLifetimeSec = 6.0f;  ///< Enemy ping lifetime (brief)
    constexpr float kTFPingIntervalSec = 1.0f;       ///< min seconds between placements per player
    constexpr uint32_t kTFMaxLivePingsPerPlayer = 3; ///< oldest is replaced beyond this
    constexpr float kTFPingMarkerFadeSec = 1.5f;     ///< alpha ramps out over the final seconds

    /// RGBA 0-1 marker color per ping type (UI + world marker share this).
    inline void PingColor(TFPingType t, float out[4])
    {
        switch (t)
        {
        case TFPingType::Enemy:
            out[0] = 0.95f;
            out[1] = 0.20f;
            out[2] = 0.18f;
            break; // red
        case TFPingType::NeedSupport:
            out[0] = 0.30f;
            out[1] = 0.70f;
            out[2] = 1.00f;
            break; // blue
        default:
            out[0] = 1.00f;
            out[1] = 0.82f;
            out[2] = 0.25f;
            break; // Location yellow
        }
        out[3] = 1.0f;
    }

    inline const char* PingTypeName(TFPingType t)
    {
        switch (t)
        {
        case TFPingType::Enemy:
            return "Enemy";
        case TFPingType::NeedSupport:
            return "Need Support";
        default:
            return "Location";
        }
    }

#pragma pack(push, 1)

    /// C->S: place request. `pos` is advisory for Location pings (server range-
    /// checks it); Enemy pings snap to the server's pawn position for
    /// `targetEntity`; Need-Support snaps to the sender's own pawn.
    struct TF_PingPlace
    {
        uint8_t type; // TFPingType
        uint8_t _pad[3];
        float posX, posY, posZ;
        uint32_t targetEntity; // net entity for Enemy pings (0 = none)
    };
    static_assert(sizeof(TF_PingPlace) == 20, "wire layout frozen");

    /// S->C: squad-scoped add/remove echo. `lifeSec` is the remaining lifetime
    /// at send time so late mirrors expire in lockstep with the server.
    struct TF_PingState
    {
        uint16_t pingId; // server-monotonic (wraps, never 0)
        uint8_t op;      // 0 = add, 1 = remove
        uint8_t type;    // TFPingType
        uint32_t owner;  // placer PlayerId
        float posX, posY, posZ;
        float lifeSec;        // remaining seconds (add); 0 on remove
        uint8_t ownerFaction; // FactionId of the placer
        uint8_t _pad[3];
    };
    static_assert(sizeof(TF_PingState) == 28, "wire layout frozen");

#pragma pack(pop)

    /// Read-only view of one live client-side ping (minimap / map / label
    /// consumers — see the wave wiring snippets).
    struct TFPingView
    {
        uint16_t pingId;
        TFPingType type;
        FactionId faction;
        PlayerId owner;
        float pos[3];
        float lifeLeft; ///< seconds until local expiry
        float age;      ///< seconds since the mirror learned of it
    };

    // ---------------------------------------------------------------------------
    // System
    // ---------------------------------------------------------------------------

    class TFPingSystem
    {
      public:
        TFPingSystem();
        ~TFPingSystem();

        bool Initialize(TFGameContext& ctx, TFEventBus& events);
        void Update(float deltaTime);
        void FixedUpdate(float fixedDeltaTime);
        void Shutdown();
        void RenderDebugUI();

        // --- server surface ------------------------------------------------------

        /// Server: TFServerSim routes TFMsg 0x5468 (PingPlace) here
        /// (size-validates, then dispatches) — same one-line hook shape as
        /// TFGrenadeSystem::ServerHandleThrowMsgRaw.
        void ServerHandlePingMsgRaw(PlayerId sender, const void* data, size_t size);

        // --- client surface (TFPingUI is the only intended caller) ---------------

        /// Route a placement request to the authority: direct (enter-world
        /// gated) server call on listen host / standalone, TFClientNet::SendMsg
        /// on pure clients (TFGrenadeSystem::SendThrow pattern).
        void UiRequestPing(TFPingType type, const float pos[3], EntityId targetEntity);

        /// Visit every live mirror ping (map/minimap icons + TFPingUI labels).
        void ForEachActivePing(const std::function<void(const TFPingView&)>& fn) const;

        /// Draw the 3D world markers. Called once per frame from
        /// TFWorldSetup::RenderWorld (wiring snippet) while BeginFrame is
        /// active, after the world/ECS passes. Restores the basic-path
        /// blend/depth/texture state it touches (TFGroundFx recipe).
        void RenderClientFx(GraphicsEngine* gfx, const DirectX::XMMATRIX& view, const DirectX::XMMATRIX& proj);

      private:
        // --- server state ---
        struct ServerPing
        {
            uint16_t id = 0;
            PlayerId owner = kInvalidPlayer;
            FactionId faction = FactionId::None;
            TFPingType type = TFPingType::Location;
            SquadId squad = 0; // squad at placement time (kInvalidSquad == solo)
            float pos[3]{};
            double expiresAt = 0.0;
            double placedAt = 0.0;
        };

        // --- client mirror ---
        struct ClientPing
        {
            TFPingType type = TFPingType::Location;
            FactionId faction = FactionId::None;
            PlayerId owner = kInvalidPlayer;
            float pos[3]{};
            float lifeLeft = 0.0f;
            float age = 0.0f;
        };

        double NowSec() const;

        // server internals
        bool ServerTryPlace(PlayerId sender, TFPingType type, const float pos[3], EntityId targetEntity);
        void ServerSweepExpired();
        void ServerRemovePing(size_t index); ///< broadcasts the remove, erases
        void BroadcastState(const ServerPing& ping, uint8_t op);
        void SendStateTo(PlayerId target, const TF_PingState& msg);
        /// Visit every player the ping is visible to under CURRENT squad state.
        void ForEachRecipient(PlayerId owner, SquadId squad, const std::function<void(PlayerId)>& fn) const;

        // client internals
        void ClientHandleState(const TF_PingState& msg);
        void ClientAdvance(float dt);

#ifdef ENABLE_NETWORKING
        bool ClientNetActive() const;
        void EnsureClientHandlers();
        void ReleaseClientHandlers();
#endif

        TFGameContext* m_ctx{nullptr};
        TFEventBus* m_events{nullptr};
        bool m_initialized{false};
        double m_clock{0.0}; ///< fallback time base when serverSim is absent

        // server state (authority only)
        std::vector<ServerPing> m_live;
        std::unordered_map<PlayerId, double> m_nextPingAt; // per-player rate limit
        uint16_t m_nextPingId{1};

        // client state (all roles with a local player)
        std::unordered_map<uint16_t, ClientPing> m_clientPings;
        bool m_clientHandlers{false};

        // render resources (lazy; render thread == game thread)
        std::unique_ptr<Mesh> m_quad; ///< unit plane billboarded per diamond

        // debug counters
        uint32_t m_placed{0}, m_rejected{0}, m_badPackets{0}, m_expired{0};
    };

} // namespace Terrafront
