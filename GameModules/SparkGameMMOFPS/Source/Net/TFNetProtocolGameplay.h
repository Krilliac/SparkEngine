/**
 * @file TFNetProtocolGameplay.h
 * @brief Packed gameplay wire structs: input, spawn, combat, capture,
 *        vehicles, squads, chat, XP, loadout, faction, world-welcome.
 *
 * Part of the frozen TFNetProtocol.h contract (see DESIGN.md §3), split into a
 * sibling part-header purely for file-size sanity. Do not include directly —
 * include Net/TFNetProtocol.h, the umbrella. Layouts are frozen and
 * static_asserted; all structs are POD, packed, little-endian on the wire.
 */
#pragma once

#include "Core/TFTypes.h"
#include <cstdint>

namespace Terrafront
{

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

#pragma pack(pop)

} // namespace Terrafront
