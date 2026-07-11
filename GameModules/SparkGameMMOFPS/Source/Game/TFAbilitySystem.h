/**
 * @file TFAbilitySystem.h
 * @brief Class abilities (W9): server-authoritative activation, durations,
 *        cooldowns, and per-class effects for the classes.json ability rows.
 *
 * OWNERSHIP: class-abilities lane (this header + TFAbilitySystem.cpp).
 *
 * Design (TFOutfitSystem precedent: server registry + client mirror in one
 * class; wire ids + packed structs live HERE, not in TFNetProtocol.h):
 *  - SERVER (authority roles): tracks one ability record per player, seeded
 *    lazily from TFPlayerSystem::GetPawnByPlayer + classes.json ClassDef::
 *    ability. Validates every activation (alive pawn, class has an ability,
 *    phase == Ready, jets fuel floor), runs the FixedUpdate state machine
 *    (Active -> duration expiry / toggle-off / fuel-empty / death ->
 *    Cooldown -> Ready) and applies the server-side effects:
 *      veil      (Ghost)      visual+HUD only — broadcast the active flag;
 *                             clients ghost the pawn mesh for enemy viewers.
 *                             NOT harder to hit: the lag-comp hit model knows
 *                             nothing about veil (documented limitation).
 *      jets      (Striker)    fuel tank (durationSec of thrust, regenPerSec
 *                             refill); movement thrust via the mirrored
 *                             TFServerSim/TFClientNet move-mod snippets.
 *      surge     (Medtech)    kTFSurgeHealPerSec to same-faction alive pawns
 *                             (incl. self) within kTFSurgeRadiusM each tick,
 *                             through TFDamageSystem::ServerHeal.
 *      forge     (Fabricator) instant: routes to TFDeployableSystem::
 *                             ServerTryPlaceDeployable(FabTurret); only a
 *                             SUCCESSFUL placement burns the 30 s cooldown.
 *      aegiswall (Bulwark)    kTFAegisAbsorbHp damage absorb through the
 *                             TFDamageSystem incoming-damage filter seam +
 *                             kTFAegisSpeedMult move slow; ends early when
 *                             the pool breaks.
 *      lockdown  (Colossus)   toggle: rooted (speedMult 0, no jump) +
 *                             kTFLockdownRoFMult exposed for the weapons
 *                             lane's RoF seam (RoFMultiplierForPawn).
 *    Unknown future ability keys degrade to a generic timed buff
 *    (activate/duration/cooldown/broadcast, no effect).
 *  - WIRE: reserved TFMsg block 0x5458-0x545F. C->S TF_AbilityRequest rides
 *    TFServerSim::RouteClientMessage (enter-world-gated choke point, wiring
 *    snippet in the wave report). S->C TF_AbilityState is sent by this
 *    system on every phase transition to ALL clients (self reads the full
 *    detail; everyone else uses the pawn-active flag for veil ghosting /
 *    future FX), with a late-joiner burst via the GetClients() diff poll
 *    (TFDeployableSystem pattern). Listen-host/standalone local player is
 *    fed by direct mirror calls (TFSquadSystem::SendEchoTo pattern).
 *  - CLIENT: F-key edge -> request (suppressed while any fullscreen UI/chat
 *    owns input, same gate set as TFClientNet::SampleAndSendInput); local
 *    self mirror (phase/remaining/fuel, counted down client-side between
 *    authoritative transitions); veil ghosting by toggling the replicated
 *    pawn's MeshRenderer::visible (engine CoreComponents) — no render-path
 *    edits needed. Client handlers self-register when the socket connects
 *    (TFSquadSystem EnsureClientHandlers pattern) — no TFClientNet edits.
 *  - BOTS: CanUseAbility/UseAbility are the public server-side entry points
 *    for TFBotSystem (direct calls, same validation path; bots never pass
 *    through the enter-world gate, mirroring their spawn/fire entry points).
 */
#pragma once

#include "Core/TFTypes.h"
#include "Core/TFEvents.h"
#include "Game/TFMovementModel.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace Terrafront
{

    // ---------------------------------------------------------------------------
    // Wire protocol — reserved TFMsg id block 0x5458-0x545F (class-abilities
    // lane). TFNetProtocol.h (contended) only gains a block comment via the
    // wave wiringNotes; these constants keep this lane compiling standalone
    // and MUST stay value-identical to any future enum entries.
    // ---------------------------------------------------------------------------

    constexpr uint16_t kTFMsgAbilityRequest = 0x5458; // C->S  TF_AbilityRequest
    constexpr uint16_t kTFMsgAbilityState = 0x5459;   // S->C  TF_AbilityState (self detail + visible-pawn broadcast)
    // 0x545A-0x545F reserved for future ability traffic.

    /// Ability lifecycle phase (server truth; mirrored to clients).
    enum class TFAbilityPhase : uint8_t
    {
        Ready = 0,
        Active = 1,
        Cooldown = 2,
    };

#pragma pack(push, 1)

    struct TF_AbilityRequest
    {
        uint8_t on; // 1 = activate / toggle-on, 0 = deactivate (toggle & jets)
        uint8_t _pad[3];
    };
    static_assert(sizeof(TF_AbilityRequest) == 4, "wire layout frozen");

    /// Phase-transition notification. For the owning player this is the HUD
    /// truth (remainingSec/fuel01 re-seed the local countdown); for everyone
    /// else only pawnEntity + abilityClass + phase matter (veil ghosting).
    struct TF_AbilityState
    {
        uint32_t pawnEntity;  // subject pawn (network entity id; 0 = none)
        uint32_t player;      // owning player
        uint8_t abilityClass; // ClassId of the ability's class
        uint8_t phase;        // TFAbilityPhase
        uint8_t _pad[2];
        float remainingSec; // Active: time left (0 = indefinite); Cooldown: cd left
        float fuel01;       // jets fuel fraction 0..1 (1 for non-fuel abilities)
    };
    static_assert(sizeof(TF_AbilityState) == 20, "wire layout frozen");

#pragma pack(pop)

    // ---------------------------------------------------------------------------
    // Effect tuning (desc-level numbers from classes.json ability text; the
    // structured fields — duration/cooldown/regen/toggle — stay data-driven).
    // ---------------------------------------------------------------------------

    constexpr float kTFSurgeRadiusM = 8.0f;          ///< Triage Surge heal radius
    constexpr float kTFSurgeHealPerSec = 30.0f;      ///< 150 HP over the 5 s duration
    constexpr float kTFAegisAbsorbHp = 450.0f;       ///< Bulwark Field absorb pool
    constexpr float kTFAegisSpeedMult = 0.8f;        ///< Bulwark Field -20% move speed
    constexpr float kTFLockdownRoFMult = 1.4f;       ///< Anchor Lockdown +40% RoF (weapons seam)
    constexpr float kTFJetMinActivateFuel01 = 0.15f; ///< min fuel fraction to ignite jets
    constexpr float kTFJetUpAccelMps2 = 12.0f;       ///< net climb accel beyond gravity cancel
    constexpr float kTFJetMaxRiseMps = 9.0f;         ///< vertical rise speed cap under thrust

    /// Per-pawn movement modifiers the ability system feeds into THE shared
    /// movement step. Applied by the mirrored TFServerSim::StepPlayer /
    /// TFClientNet::SimulateMove snippets (wave wiringNotes, both-or-neither).
    struct TFAbilityMoveMods
    {
        float speedMult = 1.0f; ///< run/sprint max-speed multiplier (0 = rooted, no jump)
        bool jetThrust = false; ///< apply TFApplyJetThrust after the move step
    };

    /// Striker jet thrust — MUST run identically on server and client (called
    /// from both mirrored snippets, after TFMoveStep + collision resolve, so
    /// gravity applied inside the step is cancelled and the pawn climbs).
    inline void TFApplyJetThrust(TFMoveState& s, float dt)
    {
        s.vel[1] += (kTFGravity + kTFJetUpAccelMps2) * dt;
        if (s.vel[1] > kTFJetMaxRiseMps)
            s.vel[1] = kTFJetMaxRiseMps;
        s.grounded = false;
    }

    /// Behavior implemented for a classes.json ability key.
    enum class TFAbilityKind : uint8_t
    {
        None = 0,     ///< class has no ability / data unloaded
        Veil,         ///< "veil"      Ghost
        Jets,         ///< "jets"      Striker
        Surge,        ///< "surge"     Medtech
        Forge,        ///< "forge"     Fabricator
        AegisWall,    ///< "aegiswall" Bulwark
        Lockdown,     ///< "lockdown"  Colossus
        GenericTimed, ///< unknown key: timed activate/cooldown, no effect
    };

    struct ClassAbilityDef; // Data/TFDataTables.h

    // ---------------------------------------------------------------------------
    // System
    // ---------------------------------------------------------------------------

    class TFAbilitySystem
    {
      public:
        TFAbilitySystem();
        ~TFAbilitySystem();

        bool Initialize(TFGameContext& ctx, TFEventBus& events);
        void Update(float deltaTime);
        void FixedUpdate(float fixedDeltaTime);
        void Shutdown();
        void RenderDebugUI();

        // --- server surface ------------------------------------------------------

        /// Server: TFServerSim routes TFMsg 0x5458 (AbilityRequest) here
        /// (size-validates, then dispatches) — same one-line hook shape as
        /// TFOutfitSystem::ServerHandleOutfitMsgRaw.
        void ServerHandleAbilityMsgRaw(PlayerId sender, const void* data, size_t size);

        /// Server: true when `player` could activate its class ability RIGHT
        /// NOW (alive pawn, ability exists, phase Ready, jets fuel floor).
        /// Bots-lane query — free to call every think tick.
        bool CanUseAbility(PlayerId player) const;

        /// Server: activate (`on`=true) or end a toggle/jets ability
        /// (`on`=false) for `player`. Full validation; returns true when the
        /// state actually changed. Bots-lane entry point — bypasses the
        /// enter-world gate exactly like TFBotSystem's spawn/fire calls.
        bool UseAbility(PlayerId player, bool on = true);

        /// Server: movement modifiers for an alive pawn (network entity id).
        /// Called by the TFServerSim::StepPlayer wiring snippet each tick.
        TFAbilityMoveMods MoveModsForPawn(EntityId pawnNetEntity) const;

        /// Server: RoF multiplier for a firing pawn (Anchor Lockdown, else 1).
        /// Seam for the weapons lane (see wave wiringNotes; unapplied = the
        /// lockdown RoF bonus simply stays off, everything else still works).
        float RoFMultiplierForPawn(EntityId pawnNetEntity) const;

        // --- both-sides queries (server truth on authority, mirror on clients) ---

        /// True when this pawn's class ability is currently active.
        bool IsPawnAbilityActive(EntityId pawnNetEntity) const;

        /// True when this pawn is a veiled Ghost (drives ghosting/FX).
        bool IsPawnVeiled(EntityId pawnNetEntity) const;

        // --- client surface ------------------------------------------------------

        /// Client: movement modifiers for the LOCAL player's predicted pawn.
        /// Called by the TFClientNet::SimulateMove wiring snippet (the exact
        /// mirror of MoveModsForPawn on the server — prediction symmetry).
        TFAbilityMoveMods MoveModsLocal() const;

        /// Client: RoF multiplier for the LOCAL pawn (Anchor Lockdown, else 1)
        /// — the client half of the optional weapons-lane RoF seam (pairs with
        /// RoFMultiplierForPawn on the server; both-or-neither).
        float RoFMultiplierLocal() const;

        /// HUD widget view (TFHUD wiring snippet). `fraction01` is fill level:
        /// cooldown elapsed fraction, active remaining fraction, or jets fuel.
        struct HudView
        {
            bool valid = false;
            std::string name; ///< ability display name (classes.json)
            char hotkey = 'F';
            TFAbilityPhase phase = TFAbilityPhase::Ready;
            float remainingSec = 0.0f;
            float fraction01 = 1.0f;
            bool fuelBased = false; ///< jets: fraction01 is fuel, not a timer
        };
        HudView GetLocalHudView() const;

      private:
        // --- server state ---
        struct ServerRec
        {
            EntityId pawn = 0;
            ClassId cls = ClassId::COUNT;
            TFAbilityPhase phase = TFAbilityPhase::Ready;
            double activeUntil = 0.0; ///< 0 while Active == indefinite (toggle / fuel-driven)
            double cooldownUntil = 0.0;
            float fuel01 = 1.0f;     ///< jets tank
            float absorbPool = 0.0f; ///< aegiswall remaining absorb
        };

        // --- client mirror ---
        struct SelfMirror
        {
            bool valid = false;
            ClassId cls = ClassId::COUNT;
            TFAbilityPhase phase = TFAbilityPhase::Ready;
            float remainingSec = 0.0f; ///< counts down locally between transitions
            float fuel01 = 1.0f;       ///< re-simulated locally from data rates
        };
        struct ActivePawn
        {
            ClassId cls = ClassId::COUNT;
            PlayerId player = kInvalidPlayer;
        };

        static TFAbilityKind KindOfKey(const std::string& key);
        const ClassAbilityDef* AbilityDefOf(ClassId cls) const; ///< nullptr if none/unloaded
        double NowSec() const;

        // server internals
        bool ServerHandleRequest(PlayerId sender, bool on); ///< shared socket/loopback/bot path
        bool ServerActivate(PlayerId player, ServerRec& rec, const ClassAbilityDef& def, TFAbilityKind kind);
        void ServerEndActive(PlayerId player, ServerRec& rec, bool startCooldown);
        void ServerAdvancePhase(PlayerId player, ServerRec& rec); ///< timer-driven transitions
        bool ServerRefreshRec(PlayerId player, ServerRec& rec);   ///< sync pawn/cls from TFPlayerSystem
        void ServerTickSurge(const ServerRec& rec, float fdt);
        float ServerFilterIncomingDamage(EntityId victim, float amount); ///< TFDamageSystem seam
        void ServerBroadcastState(PlayerId player, const ServerRec& rec);

        // client internals
        void ClientPollAbilityKey();
        void SendRequest(bool on); ///< authority: direct (gated); client: SendMsg
        void ClientHandleState(const TF_AbilityState& st);
        void ClientTickMirror(float dt);
        void UpdateVeilVisuals();
        void RestorePawnVisibility(EntityId pawnNetEntity);
        void SetPawnMeshVisible(EntityId pawnNetEntity, bool visible);

        void OnPlayerSpawned(const EvPlayerSpawned& ev);
        void OnPlayerKilled(const EvPlayerKilled& ev);

        void RegisterConsoleCommands();

#ifdef ENABLE_NETWORKING
        bool ClientNetActive() const;
        void EnsureClientHandlers();
        void ReleaseClientHandlers();
        void SendStateWire(PlayerId target, const TF_AbilityState& st);
#endif

        TFGameContext* m_ctx{nullptr};
        TFEventBus* m_events{nullptr};
        bool m_initialized{false};
        double m_clock{0.0}; ///< fallback time base when serverSim is absent

        // server state (authority only)
        std::unordered_map<PlayerId, ServerRec> m_server;
        std::unordered_map<EntityId, PlayerId> m_pawnOwner; ///< alive ability pawns
        float m_sweepAccum{0.0f};
#ifdef ENABLE_NETWORKING
        std::unordered_set<PlayerId> m_knownClients; ///< late-joiner state burst
#endif

        // client state (all roles with a local player)
        SelfMirror m_mirror;
        std::unordered_map<EntityId, ActivePawn> m_activePawns; ///< broadcast ability-active pawns
        std::unordered_map<EntityId, uint32_t> m_hiddenMeshes;  ///< net entity -> local entity ghosted
        bool m_clientHandlers{false};
        bool m_cmdsRegistered{false};

        // debug counters
        uint32_t m_activations{0}, m_rejected{0}, m_badPackets{0};
        float m_healGiven{0.0f}, m_absorbed{0.0f};
    };

} // namespace Terrafront
