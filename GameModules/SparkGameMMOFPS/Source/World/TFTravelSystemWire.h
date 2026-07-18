/**
 * @file TFTravelSystemWire.h
 * @brief Travel wire channel for TFTravelSystem: the reserved TFMsg block
 *        0x5434-0x5437, TFTravelErr, and the packed C<->S payload structs
 *        (layouts frozen once merged — see TFTravelSystem.h for the channel
 *        rationale). Split from TFTravelSystem.h, which includes this header;
 *        include TFTravelSystem.h, not this file.
 */
#pragma once

#include <cstdint>

namespace Terrafront
{

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

} // namespace Terrafront
