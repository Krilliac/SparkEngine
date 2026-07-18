/**
 * @file TFSquadTypes.h
 * @brief Squad constants and the W11 squad-v2 wire block (TF_SquadWaypoint on
 *        the RESERVED 0x546C lane, SquadWpOp) split out of TFSquadSystem.h.
 *        Included by the TFSquadSystem.h umbrella — include that header, not
 *        this one, from other translation units.
 */
#pragma once

#include "Core/TFTypes.h"

#include <cstdint>

namespace Terrafront
{

    constexpr SquadId kInvalidSquad = 0;            ///< 0 == "no squad"
    constexpr float kSquadSpawnCooldownSec = 15.0f; ///< squad-leader spawn cd (W11 squad-v2, was DESIGN §4 30 s)

    // --- W11 squad-v2 wire (RESERVED TFMsg block 0x546C-0x546F) ----------------
    // Ids live OUTSIDE the frozen TFMsg enum (TFRepProtocol / TFSocialProtocol
    // precedent); they ride NetworkManager's registry cast to MessageType and
    // TFClientNet::SendMsg via static_cast<TFMsg>.

    constexpr uint16_t kTFMsgSquadWaypoint = 0x546C; // C<->S TF_SquadWaypoint (0x546D-0x546F free)

    /// TF_SquadWaypoint.op. C->S: the request. S->C echo: what happened
    /// (Set/Clear broadcast to members; Request forwarded to the LEADER only).
    enum class SquadWpOp : uint8_t
    {
        Set = 0, // leader: place/replace the squad waypoint
        Clear,   // leader: remove it
        Request  // non-leader: ping the leader with a suggested waypoint
    };

    /// Seconds a leader-side waypoint request ping stays promotable/visible.
    constexpr float kSquadWpRequestTTLSec = 20.0f;

#pragma pack(push, 1)
    struct TF_SquadWaypoint
    {
        uint8_t op; // SquadWpOp
        uint8_t _pad;
        uint16_t regionId;   // map hex hint for icons (kInvalidRegion if none)
        uint32_t fromPlayer; // S->C echo: who set/requested (server-filled)
        float wpX, wpY, wpZ; // world position (Y = terrain height at set time)
    };
    static_assert(sizeof(TF_SquadWaypoint) == 20, "wire layout frozen");
#pragma pack(pop)

} // namespace Terrafront
