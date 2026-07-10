/**
 * @file TFSocialProtocol.h
 * @brief Wire structs + message ids for the TERRAFRONT social layer
 *        (friends / block list / recent players / online-player roster).
 *
 * OWNERSHIP: chat-social lane (same agent as Game/TFSocialSystem and
 * UI/TFChatWindow / UI/TFSocialPanel).
 *
 * Ids live in the lane's RESERVED 0x5440-0x5447 block, deliberately OUTSIDE
 * the frozen TFMsg enum (TFNetProtocol.h) — the exact precedent set by
 * TFRepProtocol.h's 0x54F0 block. They ride NetworkManager's handler registry
 * cast to Spark::Net::MessageType at the call site; TFClientNet::SendMsg
 * accepts them via static_cast<TFMsg>.
 *
 * Channel map:
 *   0x5440 SocialOp      C->S reliable  TF_SocialOp        (friend/block add+remove, list request)
 *   0x5441 SocialOpReply S->C reliable  TF_SocialOpReply   (op result feedback)
 *   0x5442 SocialList    S->C reliable  TF_SocialListHeader + N TF_SocialEntry
 *   0x5443 SocialRoster  S->C reliable  TF_RosterHeader + N TF_RosterEntry
 *   0x5444-0x5447 reserved (unused)
 *
 * All structs are packed PODs with static_assert frozen-layout guards, exactly
 * like TFNetProtocol.h / TFRepProtocol.h. Server-authoritative: clients only
 * ever send TF_SocialOp; every list/roster state flows S->C.
 */
#pragma once

#include "Core/TFTypes.h"
#include <cstdint>

namespace Terrafront
{

    // Message ids (chat-social lane reserved block 0x5440-0x5447).
    constexpr uint16_t kTFSocialMsg_Op = 0x5440;      // C->S TF_SocialOp
    constexpr uint16_t kTFSocialMsg_OpReply = 0x5441; // S->C TF_SocialOpReply
    constexpr uint16_t kTFSocialMsg_List = 0x5442;    // S->C TF_SocialListHeader + N entries
    constexpr uint16_t kTFSocialMsg_Roster = 0x5443;  // S->C TF_RosterHeader + N entries

    /// Character-name wire width; matches TF_CharBrief.name (23-char max + NUL).
    constexpr size_t kTFSocialNameLen = 24;

    /// Server-side per-character list caps (persisted lists stay bounded).
    constexpr size_t kTFMaxFriends = 32;
    constexpr size_t kTFMaxBlocked = 32;
    constexpr size_t kTFMaxRecent = 16;

    enum class SocialOp : uint8_t
    {
        FriendAdd = 0,
        FriendRemove,
        BlockAdd,
        BlockRemove,
        RequestLists // resend friends/blocked/recent snapshots (no target name)
    };

    enum class SocialOpResult : uint8_t
    {
        Ok = 0,
        NotFound,      // no character with that name exists
        Self,          // tried to friend/block yourself
        AlreadyListed, // duplicate add
        NotListed,     // remove target not on the list
        ListFull,      // kTFMaxFriends / kTFMaxBlocked reached
        BadName,       // empty / unparseable target name
        ServerError    // db unavailable / not entered world
    };

    enum class SocialListKind : uint8_t
    {
        Friends = 0,
        Blocked,
        Recent
    };

#pragma pack(push, 1)

    /// C->S: one friend/block mutation (or a full-list refresh request).
    struct TF_SocialOp
    {
        uint8_t op; // SocialOp
        uint8_t _pad[3];
        char targetName[kTFSocialNameLen]; // NUL-terminated character name (unused for RequestLists)
    };
    static_assert(sizeof(TF_SocialOp) == 28, "wire layout frozen");

    /// S->C: result of a TF_SocialOp (panel feedback line).
    struct TF_SocialOpReply
    {
        uint8_t op;     // SocialOp echoed
        uint8_t result; // SocialOpResult
        uint8_t _pad[2];
        char targetName[kTFSocialNameLen]; // canonical (db-cased) name when resolved
    };
    static_assert(sizeof(TF_SocialOpReply) == 28, "wire layout frozen");

    /// S->C framing: TF_SocialListHeader followed by exactly `count`
    /// TF_SocialEntry records (TF_RepUpdateHeader + N pattern).
    struct TF_SocialListHeader
    {
        uint8_t listKind; // SocialListKind
        uint8_t count;
        uint8_t _pad[2];
    };
    static_assert(sizeof(TF_SocialListHeader) == 4, "wire layout frozen");

    struct TF_SocialEntry
    {
        char name[kTFSocialNameLen];
        uint8_t faction; // FactionId (0 if unknown)
        uint8_t online;  // 0/1 at snapshot time (clients re-derive live from the roster)
        uint8_t _pad[2];
    };
    static_assert(sizeof(TF_SocialEntry) == 28, "wire layout frozen");

    /// S->C framing: TF_RosterHeader followed by exactly `count` TF_RosterEntry.
    /// full==1 replaces the client's whole roster mirror (sent on enter-world);
    /// full==0 is a delta (one player entered/left the world).
    struct TF_RosterHeader
    {
        uint8_t full; // 0/1
        uint8_t count;
        uint8_t _pad[2];
    };
    static_assert(sizeof(TF_RosterHeader) == 4, "wire layout frozen");

    struct TF_RosterEntry
    {
        uint32_t playerId;
        char name[kTFSocialNameLen];
        uint8_t faction; // FactionId
        uint8_t online;  // 1 = in world, 0 = left (delta only)
        uint8_t _pad[2];
    };
    static_assert(sizeof(TF_RosterEntry) == 32, "wire layout frozen");

#pragma pack(pop)

} // namespace Terrafront
