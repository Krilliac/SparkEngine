/**
 * @file TFNetProtocol.h
 * @brief TERRAFRONT wire protocol — every app-level message on the network.
 *
 * FROZEN CONTRACT (see DESIGN.md §3). Messages ride NetworkManager's handler
 * registry. Entity transforms/health replicate through EntityReplicator, NOT
 * through these messages — TFMsg is for events and commands only.
 *
 * All structs are POD, packed, little-endian on the wire (x64 native order;
 * cross-endian hosts are out of scope v1). Every struct is static_asserted so
 * accidental layout drift breaks the build, not the game.
 */
#pragma once

#include "Core/TFTypes.h"
#include <cstdint>

namespace Terrafront
{

    // Message ids: 0x5400 block ("TF")
    enum class TFMsg : uint16_t
    {
        ClientInput = 0x5400,   // C->S  TF_ClientInput, 60Hz
        SpawnRequest = 0x5401,  // C->S  TF_SpawnRequest
        SpawnReply = 0x5402,    // S->C  TF_SpawnReply
        FireEvent = 0x5403,     // C->S  TF_FireEvent (server validates)
        HitConfirm = 0x5404,    // S->C  TF_HitConfirm (hitmarker)
        DamageEvent = 0x5405,   // S->C  TF_DamageEvent (victim feedback)
        KillEvent = 0x5406,     // S->A  TF_KillEvent (killfeed)
        RegionState = 0x5407,   // S->A  TF_RegionState (full region snapshot)
        CaptureTick = 0x5408,   // S->A  TF_CaptureTick (progress delta)
        VehicleEnter = 0x5409,  // C->S  TF_VehicleSeatOp
        VehicleExit = 0x540A,   // C->S  TF_VehicleSeatOp
        AegisDeploy = 0x540B,   // C->S  TF_AegisDeploy (toggle)
        SquadMsg = 0x540C,      // C<->S TF_SquadMsg
        ChatMsg = 0x540D,       // C<->S TF_ChatMsg
        XPEvent = 0x540E,       // S->C  TF_XPEvent
        LoadoutChange = 0x540F, // C->S  TF_LoadoutChange
        FactionSelect = 0x5410, // C->S  TF_FactionSelect
        WorldWelcome = 0x5411,  // S->C  TF_WorldWelcome (on join: your id, territory hash)

        // --- W5 onboarding (Task 4): login -> char-select/create -> enter-world.
        // TF_WorldWelcome above is now gated behind a successful EnterWorldReq —
        // it is no longer sent immediately on connect (see TFServerSim.cpp
        // PollClientJoinsLeaves / HandleEnterWorld).
        LoginRequest = 0x5412,    // C->S  TF_AuthRequest
        LoginReply = 0x5413,      // S->C  TF_AuthReply
        RegisterRequest = 0x5414, // C->S  TF_AuthRequest
        RegisterReply = 0x5415,   // S->C  TF_AuthReply
        CharListRequest = 0x5416, // C->S  (empty payload)
        CharListReply = 0x5417,   // S->C  TF_CharListReply
        CharCreateReq = 0x5418,   // C->S  TF_CharCreateRequest
        CharCreateReply = 0x5419, // S->C  TF_CharOpReply
        CharDeleteReq = 0x541A,   // C->S  TF_CharDeleteRequest
        CharDeleteReply = 0x541B, // S->C  TF_CharOpReply
        EnterWorldReq = 0x541C,   // C->S  TF_EnterWorldRequest (reply is the gated TF_WorldWelcome)

        // W6 progression: unlock-tree purchases (Persistence/TFUnlockTree.h).
        UnlockRequest = 0x541D, // C->S  TF_UnlockRequest (buy a TFUnlockTree node)
        UnlockReply = 0x541E,   // S->C  TF_UnlockReply

        // W7 ui-map-keys (reserved block 0x5430-0x5437, shared with continents:
        // 0x5434-0x5437 travel — ids + structs in World/TFTravelSystem.h): map
        // redeploy. Packed structs + frozen-layout static_asserts live in
        // Net/TFRedeployProtocol.h (its kTFMsg_* constants name the same wire
        // values so the UI lane compiles standalone).
        RedeployRequest = 0x5430, // C->S  TF_RedeployRequest
        RedeployReply = 0x5431,   // S->C  TF_RedeployReply

        // Outfits lane (reserved block 0x5438-0x543F; 0x543D-0x543F free).
        // Packed wire structs + static_assert layout guards live in
        // Game/TFOutfitSystem.h (its kTFMsgOutfit* constants name the same
        // wire values so the lane compiles standalone).
        OutfitRequest = 0x5438,   // C->S  TF_OutfitRequest (create/invite/accept/decline/leave/kick/setrank/disband)
        OutfitReply = 0x5439,     // S->C  TF_OutfitReply (result of your own op)
        OutfitRoster = 0x543A,    // S->C  TF_OutfitRoster (own-outfit roster snapshot, chunked)
        OutfitTagUpdate = 0x543B, // S->C  TF_OutfitTagUpdate (broadcast player->tag for nameplates/killfeed)
        OutfitInvite = 0x543C,    // S->C  TF_OutfitInvite (invite notice to the invitee)

        // W9 class-abilities block (reserved 0x5458-0x545F; 0x545A-0x545F free).
        // Packed wire structs + static_assert layout guards live in
        // Game/TFAbilitySystem.h (its kTFMsgAbility* constants name the same wire
        // values so the lane compiles standalone).
        AbilityRequest = 0x5458, // C->S  TF_AbilityRequest (activate / toggle-off)
        AbilityState = 0x5459,   // S->C  TF_AbilityState (self detail + visible-pawn broadcast)

        // W10 grenades block (reserved 0x5464-0x5467). S->C fx ids 0x5465-0x5467
        // + packed structs + static_assert layout guards live in
        // Game/TFGrenadeSystem.h (its kTFMsgGrenade* constants name the same
        // wire values so the lane compiles standalone).
        GrenadeThrow = 0x5464, // C->S  TF_GrenadeThrow (server validates + simulates)

        // W11 ping-system block (reserved 0x5468-0x546B). S->C state id 0x5469
        // + packed structs + static_assert layout guards live in
        // Game/TFPingSystem.h (its kTFMsgPing* constants name the same wire
        // values so the lane compiles standalone).
        PingPlace = 0x5468, // C->S  TF_PingPlace (server validates + squad-rebroadcasts)

        // 0x5440-0x5447: chat-social block — ids + structs in
        // Net/TFSocialProtocol.h (TFRepProtocol precedent; not enumerators).
        // 0x5448-0x544B: turret-aim block — id + struct in
        // Game/TFVehicleSystem.h (TFRepProtocol precedent; not enumerators).
        // 0x5448 = S->C TF_RepVehicleAim; 0x5449-0x544B free.
        // 0x544C-0x544F: directives-ui block — ids + structs in
        // UI/TFDirectivePanel.h. 0x544C = C->S DirectiveStatusReq,
        // 0x544D = S->C TF_DirectiveStatus; 0x544E-0x544F free.
        // 0x5460-0x5463: medals-scoreboard block — ids + structs in
        // Game/TFMedalSystem.h (TFRepProtocol precedent; not enumerators).
        // 0x5460 = S->C TF_MedalAward; 0x5461 = S->C TF_ScoreUpdate;
        // 0x5462-0x5463 free.
        // 0x546C-0x546F: squad-v2 block — id + struct in Game/TFSquadSystem.h.
        // 0x546C = C<->S TF_SquadWaypoint; 0x546D-0x546F free.
        // 0x5470-0x5473: alerts block — id + struct in World/TFAlertSystem.h
        // (TFRepProtocol precedent; not enumerators). 0x5470 = S->C
        // TF_AlertState; 0x5471-0x5473 free.
        // 0x5474-0x5477: death-recap block — id + packed structs in
        // Game/TFDamageSystem.h (TFRepProtocol precedent; not enumerators).
        // 0x5474 = S->C TF_DeathRecap (reliable, victim only); 0x5475-0x5477 free.
        // 0x5478-0x547B time-of-day lane (W12): 0x5478 TF_TimeOfDay S->C reliable
        // — ids + struct live in World/TFDayNight.h (TFFireFxProtocol precedent).
        // 0x5479-0x547B free.
        // 0x547C-0x547F: weather block — id + packed struct in World/TFWeatherFx.h
        // (TFRepProtocol precedent; not enumerators). 0x547C = S->C
        // TF_WeatherState heartbeat; 0x547D-0x547F free.
        // 0x5480-0x5483: outfit-leaderboards block (W12) — id + packed struct in
        // Game/TFOutfitSystem.h. 0x5480 = S->C TF_OutfitLeaderboard (the request is
        // TFOutfitOp::Leaderboard riding OutfitRequest 0x5438); 0x5481-0x5483 free.
        // 0x5484-0x5487: killcam lane (W13) — 0x5484 is LIVE (this comment
        // previously said the block was reserved-but-unused; that went stale
        // the moment the killcam shipped). Game/TFDamageSystem.h defines
        // kTFMsgKillcamData = 0x5484 (S->C TF_KillcamData, reliable, victim
        // only, sent via TFDamageSystem::SendToOwner alongside TF_DeathRecap)
        // — same TFRepProtocol-precedent split as the other in-lane-header
        // blocks below: TFNetProtocol.h (contended) only carries this block
        // comment, the id + struct live in the owning lane header, and both
        // MUST stay numerically identical. 0x5485-0x5487 free.

        // 0x5488-0x548B: loadout-depth wave (reserved). 0x5488 = C->S
        // TF_LoadoutExtChange (grenade + suit picks) — dispatched via
        // RouteClientMessage to TFProgressionSystem::ServerHandleLoadoutExtMsgRaw;
        // struct + static_assert live in Game/TFProgressionSystem.h (its
        // kTFMsgLoadoutExtChange constant names the same wire value so that lane
        // compiles standalone). 0x5489 TF_FlashState (S->C unicast) and 0x548A
        // TF_SmokeSpawn (S->C broadcast) are grenade-lane FX ids defined in
        // Game/TFGrenadeSystem.h (TFRepProtocol precedent: no enum entry needed,
        // never routed through RouteClientMessage). 0x548B free.
        LoadoutExtChange = 0x5488, // C->S  TF_LoadoutExtChange (server validates)

        // 0x548C-0x548F: continent-hop redirect block (W13 multimap
        // server-authoritative follow-up, docs/TERRAFRONT_MULTIMAP.md §2.2).
        // Fresh reserved block (the travel lane's own 0x5434-0x5437 is full)
        // per that doc's stated upgrade path. Unlike the travel lane's block
        // (owned in World/TFTravelSystem.h, its own channel), this one is
        // dispatched through the SAME RouteClientMessage choke point as the
        // other onboarding/gameplay ids, so — like TF_AuthRequest/TF_AuthReply
        // above — its structs live directly here rather than in an in-lane
        // header. Net/TFServerSim.cpp owns the handler and answers from
        // World/TFTravelSystem.h's continents.json-sourced registry
        // (TFTravelSystem::LookupContinentEndpoint) rather than letting the
        // client's own copy of that file dictate where it connects next —
        // the server is the trust boundary. 0x548E-0x548F free.
        ContinentHopRequest = 0x548C, // C->S  TF_ContinentHopRequest
        ContinentHopReply = 0x548D,   // S->C  TF_ContinentHopReply
    };

#pragma pack(push, 1)

    // Bitfield for TF_ClientInput.buttons
    enum TFButton : uint16_t
    {
        TFB_Fire = 1 << 0,
        TFB_AltFire = 1 << 1, // ADS
        TFB_Jump = 1 << 2,
        TFB_Crouch = 1 << 3,
        TFB_Sprint = 1 << 4,
        TFB_Reload = 1 << 5,
        TFB_Interact = 1 << 6, // enter vehicle / capture assist / revive
        TFB_Ability = 1 << 7,  // class ability (jets / overshield / cloak)
        TFB_Melee = 1 << 8,
    };

    struct TF_ClientInput
    {
        uint32_t seq;        // monotonically increasing, for prediction ack
        uint16_t buttons;    // TFButton bits
        int8_t moveX;        // -127..127 strafe
        int8_t moveY;        // -127..127 forward
        float viewYaw;       // radians
        float viewPitch;     // radians
        uint16_t weaponSlot; // active slot index
        uint16_t _pad;
    };
    static_assert(sizeof(TF_ClientInput) == 20, "wire layout frozen");

    struct TF_SpawnRequest
    {
        uint8_t classId;      // ClassId
        uint8_t spawnKind;    // 0 skyanchor, 1 region, 2 aegis, 3 squad-leader
        uint16_t regionId;    // for spawnKind==1
        uint32_t aegisEntity; // for spawnKind==2
    };
    static_assert(sizeof(TF_SpawnRequest) == 8, "wire layout frozen");

    struct TF_SpawnReply
    {
        uint8_t accepted; // 0/1; if 0, reason below
        uint8_t reason;   // 0 ok, 1 bad-point, 2 timer, 3 contested, 4 class-locked
        uint16_t _pad;
        uint32_t entityId; // your new pawn's replicated entity
        float posX, posY, posZ;
        float respawnDelay; // seconds to wait if !accepted && reason==timer
    };
    static_assert(sizeof(TF_SpawnReply) == 24, "wire layout frozen");

    struct TF_FireEvent
    {
        uint32_t seq; // client input seq at trigger time (lag comp anchor)
        uint16_t weaponId;
        uint16_t _pad;
        float originX, originY, originZ;
        float dirX, dirY, dirZ;
    };
    static_assert(sizeof(TF_FireEvent) == 32, "wire layout frozen");

    struct TF_HitConfirm
    {
        uint32_t victimEntity;
        uint16_t damage;
        uint8_t headshot; // 0/1
        uint8_t killed;   // 0/1
    };
    static_assert(sizeof(TF_HitConfirm) == 8, "wire layout frozen");

    struct TF_DamageEvent
    {
        uint32_t attackerEntity; // 0 = environment
        uint16_t damage;
        uint8_t damageKind; // 0 bullet, 1 explosive, 2 melee, 3 fall, 4 pain-field
        uint8_t dirOctant;  // 0-7 damage direction indicator for HUD
    };
    static_assert(sizeof(TF_DamageEvent) == 8, "wire layout frozen");

    struct TF_KillEvent
    {
        uint32_t killerPlayer;
        uint32_t victimPlayer;
        uint16_t weaponId;
        uint8_t killerFaction; // FactionId
        uint8_t victimFaction;
        uint8_t headshot;
        uint8_t _pad[3];
    };
    static_assert(sizeof(TF_KillEvent) == 16, "wire layout frozen");

    struct TF_RegionState
    {
        uint16_t regionId;
        uint8_t owner;                          // FactionId
        uint8_t contested;                      // 0/1
        float captureProgress;                  // 0..1 toward `capturingFaction`
        uint8_t capturingFaction;               // FactionId or 0
        uint8_t pointOwners[kMaxCapturePoints]; // FactionId per point
    };
    static_assert(sizeof(TF_RegionState) == 12, "wire layout frozen");

    struct TF_CaptureTick
    {
        uint16_t regionId;
        uint8_t capturingFaction;
        uint8_t contested;
        float progress; // 0..1
    };
    static_assert(sizeof(TF_CaptureTick) == 8, "wire layout frozen");

    struct TF_VehicleSeatOp
    {
        uint32_t vehicleEntity;
        uint8_t seatIndex; // 0 = driver
        uint8_t _pad[3];
    };
    static_assert(sizeof(TF_VehicleSeatOp) == 8, "wire layout frozen");

    struct TF_AegisDeploy
    {
        uint32_t vehicleEntity;
        uint8_t deploy; // 1 deploy, 0 undeploy
        uint8_t _pad[3];
    };
    static_assert(sizeof(TF_AegisDeploy) == 8, "wire layout frozen");

    enum class SquadOp : uint8_t
    {
        Create = 0,
        Invite,
        Accept,
        Leave,
        Kick,
        Promote,
        SetWaypoint
    };

    struct TF_SquadMsg
    {
        uint8_t op; // SquadOp
        uint8_t _pad;
        uint16_t squadId;
        uint32_t targetPlayer; // for Invite/Accept/Kick/Promote
        float wpX, wpY, wpZ;   // for SetWaypoint
    };
    static_assert(sizeof(TF_SquadMsg) == 20, "wire layout frozen");

    enum class ChatChannel : uint8_t
    {
        Region = 0,
        Faction,
        Squad,
        Yell,
        Outfit // W8 ui-polish: outfit-scoped chat (wire value 4; routing rules in Net/TFChatRules.h)
    };

    struct TF_ChatMsg
    {
        uint32_t fromPlayer; // filled by server
        uint8_t channel;     // ChatChannel
        uint8_t _pad;
        char text[122]; // utf-8, null-terminated
    };
    static_assert(sizeof(TF_ChatMsg) == 128, "wire layout frozen");

    struct TF_XPEvent
    {
        uint16_t amount;
        uint8_t reasonCode; // indexes xp reason table (kill/assist/revive/...)
        uint8_t _pad;
        uint32_t newTotalXP;
        uint16_t newRank;
        uint16_t fluxWallet;
    };
    static_assert(sizeof(TF_XPEvent) == 12, "wire layout frozen");

    struct TF_LoadoutChange
    {
        uint8_t classId;
        uint8_t _pad;
        uint16_t primary; // WeaponId
        uint16_t secondary;
        uint16_t tool;
    };
    static_assert(sizeof(TF_LoadoutChange) == 8, "wire layout frozen");

    struct TF_FactionSelect
    {
        uint8_t faction; // FactionId
        uint8_t _pad[3];
    };
    static_assert(sizeof(TF_FactionSelect) == 4, "wire layout frozen");

    struct TF_WorldWelcome
    {
        uint32_t yourPlayerId;
        uint8_t yourFaction; // 0 if not chosen yet
        uint8_t regionCount;
        uint16_t _pad;
        uint32_t territoryHash; // client re-requests RegionState burst on mismatch
        uint32_t serverTimeMs;
    };
    static_assert(sizeof(TF_WorldWelcome) == 16, "wire layout frozen");

    // --- W5 onboarding (Task 4) --------------------------------------------------

    struct TF_AuthRequest
    {
        char user[32]; // null-terminated username
        char pass[64]; // null-terminated plaintext password (login-time only; never stored)
    };
    static_assert(sizeof(TF_AuthRequest) == 96, "wire layout frozen");

    struct TF_AuthReply
    {
        uint8_t ok;  // 0/1
        uint8_t err; // TFAuthErr
        uint8_t _pad[2];
        uint64_t accountId; // 0 if !ok
    };
    static_assert(sizeof(TF_AuthReply) == 12, "wire layout frozen");

    struct TF_CharBrief
    {
        uint64_t id;
        char name[24];   // null-terminated, matches TFCharacterSystem's 23-char max name
        uint8_t faction; // FactionId
        uint16_t rank;
        uint8_t _pad;
    };
    static_assert(sizeof(TF_CharBrief) == 36, "wire layout frozen");

    struct TF_CharListReply
    {
        uint8_t count; // number of valid entries in `chars` (0..5)
        uint8_t _pad[3];
        TF_CharBrief chars[5]; // kTFMaxCharSlots (TFCharacterSystem.h)
    };
    static_assert(sizeof(TF_CharListReply) == 4 + 5 * 36, "wire layout frozen");

    struct TF_CharCreateRequest
    {
        char name[24];
        uint8_t faction; // FactionId
        uint8_t _pad[3];
    };
    static_assert(sizeof(TF_CharCreateRequest) == 28, "wire layout frozen");

    struct TF_CharOpReply
    {
        uint8_t ok;  // 0/1
        uint8_t err; // TFCharErr
        uint8_t _pad[2];
        uint64_t charId; // the affected character (0 if !ok and unknown)
    };
    static_assert(sizeof(TF_CharOpReply) == 12, "wire layout frozen");

    struct TF_CharDeleteRequest
    {
        uint64_t charId;
    };
    static_assert(sizeof(TF_CharDeleteRequest) == 8, "wire layout frozen");

    struct TF_EnterWorldRequest
    {
        uint64_t charId;
    };
    static_assert(sizeof(TF_EnterWorldRequest) == 8, "wire layout frozen");

    // --- W6 progression: unlock-tree purchase ------------------------------------

    struct TF_UnlockRequest
    {
        char unlockKey[32]; // NUL-terminated TFUnlockTree key
    };
    static_assert(sizeof(TF_UnlockRequest) == 32, "wire layout frozen");

    struct TF_UnlockReply
    {
        uint8_t result; // Terrafront::TFUnlockResult
        uint8_t _pad[3];
        char unlockKey[32];
    };
    static_assert(sizeof(TF_UnlockReply) == 36, "wire layout frozen");

    // --- W13 multimap server-authoritative continent-hop (docs/TERRAFRONT_MULTIMAP.md §2.2) ---

    struct TF_ContinentHopRequest
    {
        uint8_t mapId; // destination continent (continents.json mapId, NOT the
                       // position-isolation kTFMap* constants — see
                       // World/TFTravelSystem.h::ContinentMeta::mapId)
        uint8_t _pad[3];
    };
    static_assert(sizeof(TF_ContinentHopRequest) == 4, "wire layout frozen");

    struct TF_ContinentHopReply
    {
        uint8_t ok;    // 0/1 — 1 == host/port below are a live registry entry
        uint8_t mapId; // destination echoed back
        uint16_t port; // 0 if !ok
        char host[64]; // null-terminated; empty if !ok
    };
    static_assert(sizeof(TF_ContinentHopReply) == 68, "wire layout frozen");

#pragma pack(pop)

} // namespace Terrafront
