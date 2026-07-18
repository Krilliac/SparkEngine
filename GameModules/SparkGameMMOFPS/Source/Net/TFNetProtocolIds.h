/**
 * @file TFNetProtocolIds.h
 * @brief TFMsg message-id enum for the TERRAFRONT wire protocol (0x5400 block).
 *
 * Part of the frozen TFNetProtocol.h contract (see DESIGN.md §3), split into a
 * sibling part-header purely for file-size sanity. Do not include directly —
 * include Net/TFNetProtocol.h, the umbrella.
 */
#pragma once

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

} // namespace Terrafront
