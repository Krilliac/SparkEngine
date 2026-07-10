/**
 * @file TFRedeployProtocol.h
 * @brief Wire structs for the W7 map redeploy flow (ui-map-keys lane).
 *
 * RESERVED TFMsg id block 0x5430-0x5437 (ui-map-keys). The ids live here as
 * constants (not TFMsg enumerators) so this lane's translation units compile
 * before the integrator lands the matching two-line TFMsg enum addition in
 * TFNetProtocol.h (see the lane wiringNotes) — both spellings name the same
 * wire value, PS2-style server-authoritative.
 *
 * Flow: client clicks a friendly, non-contested region on the open map ->
 * local countdown (kTFRedeployCountdownSec, TFRedeployRules.h) ->
 * TF_RedeployRequest -> server re-validates via TFRedeployRules::CanRedeploy
 * (the SAME predicate the client pre-checked with) + its own per-player
 * cooldown -> on success moves the authoritative pawn (TFServerSim::
 * TeleportPawn) to the region's spawn point and replies TF_RedeployReply.
 */
#pragma once

#include <cstdint>

namespace Terrafront
{

    // --- reserved ids (0x5430-0x5437, ui-map-keys lane) -----------------------
    inline constexpr uint16_t kTFMsg_RedeployRequest = 0x5430; // C->S TF_RedeployRequest
    inline constexpr uint16_t kTFMsg_RedeployReply = 0x5431;   // S->C TF_RedeployReply

    // --- TF_RedeployReply.reason values ----------------------------------------
    // Shared by the client pre-check (TFRedeployRules::CanRedeploy) and the
    // server validation so denial text needs no translation table per side.
    enum : uint8_t
    {
        kTFRedeployOk = 0,
        kTFRedeployBadRegion = 1, ///< not owned by your faction / not lattice-linked / unknown id
        kTFRedeployCooldown = 2,  ///< server redeploy interval not elapsed (see cooldownSec)
        kTFRedeployContested = 3, ///< region is being contested right now
        kTFRedeployInVehicle = 4, ///< leave the vehicle first
        kTFRedeployNotAlive = 5,  ///< dead players use the spawn flow, not redeploy
        kTFRedeployBlocked = 6,   ///< refused by an installed extra rule (e.g. sanctuary)
    };

#pragma pack(push, 1)

    struct TF_RedeployRequest
    {
        uint16_t regionId; // RegionId (regions.json index)
        uint16_t _pad;
    };
    static_assert(sizeof(TF_RedeployRequest) == 4, "wire layout frozen");

    struct TF_RedeployReply
    {
        uint8_t accepted;       // 0/1
        uint8_t reason;         // kTFRedeploy* above (0 when accepted)
        uint16_t regionId;      // echoes the request
        float posX, posY, posZ; // teleport destination when accepted
        float cooldownSec;      // remaining server cooldown when reason==Cooldown
    };
    static_assert(sizeof(TF_RedeployReply) == 20, "wire layout frozen");

#pragma pack(pop)

} // namespace Terrafront
