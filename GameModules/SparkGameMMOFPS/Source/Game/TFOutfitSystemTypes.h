/**
 * @file TFOutfitSystemTypes.h
 * @brief Outfit wire protocol split out of TFOutfitSystem.h (umbrella): the
 *        reserved TFMsg id constants, op/result enums, name/tag limits, W12
 *        competition-score weights, packed + static_asserted wire structs and
 *        the render-site tag-label helper.
 *
 * OWNERSHIP: outfits lane (with TFOutfitSystem.h/.cpp,
 * Persistence/TFOutfitStore.h/.cpp, UI/TFOutfitPanel.h/.cpp).
 *
 * @see TFOutfitSystem.h
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace Terrafront
{

    // ---------------------------------------------------------------------------
    // Wire protocol — reserved TFMsg id block 0x5438-0x543F (outfits lane).
    // The TFMsg enum additions for TFNetProtocol.h (contended) are in the wave
    // wiringNotes; these constants keep this lane compiling standalone and MUST
    // stay value-identical to those enum entries.
    // ---------------------------------------------------------------------------

    constexpr uint16_t kTFMsgOutfitRequest = 0x5438;   // C->S  TF_OutfitRequest
    constexpr uint16_t kTFMsgOutfitReply = 0x5439;     // S->C  TF_OutfitReply (your own op's result)
    constexpr uint16_t kTFMsgOutfitRoster = 0x543A;    // S->C  TF_OutfitRoster (own-outfit roster, chunked)
    constexpr uint16_t kTFMsgOutfitTagUpdate = 0x543B; // S->C  TF_OutfitTagUpdate (broadcast player->tag)
    constexpr uint16_t kTFMsgOutfitInvite = 0x543C;    // S->C  TF_OutfitInvite (notice to the invitee)
    // 0x543D-0x543F reserved for future outfit traffic.

    // W12 outfit-leaderboards — reserved TFMsg block 0x5480-0x5483. The C->S
    // side needs NO new id: the request is TFOutfitOp::Leaderboard riding the
    // already-routed TF_OutfitRequest (0x5438), so TFServerSim gains no new
    // wiring. Only the S->C snapshot uses the new block.
    constexpr uint16_t kTFMsgOutfitLeaderboard = 0x5480; // S->C  TF_OutfitLeaderboard (top-10 + your row)
    // 0x5481-0x5483 reserved for future outfit-competition traffic.

    enum class TFOutfitOp : uint8_t
    {
        Create = 0, // name+tag
        Invite,     // name = target character name
        Accept,
        Decline,
        Leave,
        Kick,    // targetCharId
        SetRank, // targetCharId + rank (SetRank to Leader == leadership transfer)
        Disband,
        Leaderboard, // W12: request the TF_OutfitLeaderboard snapshot (no other fields used)
    };

    enum class TFOutfitResult : uint8_t
    {
        Ok = 0,
        ServerError, // store unavailable / not authority / sender has no character
        BadRequest,
        NotInOutfit,
        AlreadyInOutfit,
        TargetInOutfit,
        NameInvalid,
        TagInvalid,
        NameTaken,
        TagTaken,
        NoSuchPlayer, // invite target not online / unknown character name
        NoSuchMember,
        NoInvite,
        NotPermitted,
        OutfitFull,
    };

    const char* OutfitResultText(TFOutfitResult r);

    constexpr size_t kTFOutfitNameMin = 3;
    constexpr size_t kTFOutfitNameMax = 24;
    constexpr size_t kTFOutfitTagMin = 2;
    constexpr size_t kTFOutfitTagMax = 5;
    constexpr uint32_t kTFMaxOutfitMembers = 128;
    constexpr uint8_t kTFOutfitRosterChunk = 8; ///< members per TF_OutfitRoster message

    // --- W12 competition score (design weights) + leaderboard sizing ----------
    // Server-side aggregation hooks (TFMedalSystem precedent — EXISTING event
    // surfaces only): kills via EvPlayerKilled (same suicide/team-kill filter),
    // capture participation via the canonical XP reasons 4/5/6, alert wins via
    // kXPReasonAlert (13) — the per-participant payout TFAlertSystem routes
    // through ServerAwardXP, so "won an alert" needs no new event.
    constexpr uint32_t kTFOutfitScorePerKill = 1;
    constexpr uint32_t kTFOutfitScorePerCapture = 10;
    constexpr uint32_t kTFOutfitScorePerAlertWin = 100;
    constexpr uint8_t kTFOutfitLbTopN = 10;                     ///< ranked rows in the snapshot
    constexpr uint8_t kTFOutfitLbMaxRows = kTFOutfitLbTopN + 1; ///< + requester's own out-of-top row
    constexpr float kTFOutfitLbRefreshSec = 60.0f;              ///< panel auto-refresh cadence

#pragma pack(push, 1)

    struct TF_OutfitRequest
    {
        uint8_t op;   // TFOutfitOp
        uint8_t rank; // TFOutfitRank, for op==SetRank
        uint8_t _pad[2];
        uint64_t targetCharId; // Kick / SetRank
        char name[32];         // Create: outfit name (NUL-terminated, 3-24 used)
                               // Invite: target CHARACTER name (23-char max, TF_CharBrief)
        char tag[8];           // Create: outfit tag (NUL-terminated, 2-5 used)
    };
    static_assert(sizeof(TF_OutfitRequest) == 52, "wire layout frozen");

    struct TF_OutfitReply
    {
        uint8_t op;     // TFOutfitOp echoed
        uint8_t result; // TFOutfitResult
        uint8_t _pad[2];
        uint32_t outfitId; // affected outfit (0 if none)
    };
    static_assert(sizeof(TF_OutfitReply) == 8, "wire layout frozen");

    struct TF_OutfitMemberBrief
    {
        uint64_t charId;
        char name[24]; // NUL-terminated (matches TF_CharBrief)
        uint8_t rank;  // TFOutfitRank
        uint8_t online;
        uint8_t _pad[2];
    };
    static_assert(sizeof(TF_OutfitMemberBrief) == 36, "wire layout frozen");

    /// Own-outfit roster snapshot, chunked kTFOutfitRosterChunk members per
    /// message. outfitId==0 means "you are in no outfit" (clears the mirror —
    /// sent on leave/kick/disband). chunkIndex 0 resets the mirror roster.
    struct TF_OutfitRoster
    {
        uint32_t outfitId;
        uint64_t yourCharId; // receiver's member row (LocalRank resolution)
        char name[32];
        char tag[8];
        uint16_t totalMembers;
        uint8_t chunkIndex;
        uint8_t count; // valid entries in members[]
        TF_OutfitMemberBrief members[kTFOutfitRosterChunk];
    };
    static_assert(sizeof(TF_OutfitRoster) == 56 + 8 * 36, "wire layout frozen");

    /// Broadcast player->tag binding for nameplates/killfeed on ALL clients.
    /// Empty tag == player has no outfit (clears the entry).
    struct TF_OutfitTagUpdate
    {
        uint32_t player; // PlayerId
        char tag[8];
    };
    static_assert(sizeof(TF_OutfitTagUpdate) == 12, "wire layout frozen");

    struct TF_OutfitInvite
    {
        uint32_t outfitId;
        char name[32];    // outfit name
        char tag[8];      // outfit tag
        char inviter[24]; // inviting character's name
    };
    static_assert(sizeof(TF_OutfitInvite) == 68, "wire layout frozen");

    /// One leaderboard row (W12).
    struct TF_OutfitLbRow
    {
        uint32_t outfitId;
        uint32_t rank; // 1-based true rank (own row keeps its rank outside the top 10)
        uint64_t weekly;
        uint64_t allTime;
        char name[32]; // NUL-terminated
        char tag[8];   // NUL-terminated
    };
    static_assert(sizeof(TF_OutfitLbRow) == 64, "wire layout frozen");

    /// Leaderboard snapshot (W12): top kTFOutfitLbTopN outfits ranked by weekly
    /// score (all-time, then name break ties) + the requester's own outfit row
    /// appended with its true rank when it falls outside the top N.
    struct TF_OutfitLeaderboard
    {
        uint8_t count;     // valid entries in rows[]
        uint8_t yourIndex; // index of the requester's outfit in rows[] (0xFF == not in an outfit)
        uint16_t totalOutfits;
        uint32_t weekKey; // TFOutfitISOWeekKey the weekly column belongs to
        TF_OutfitLbRow rows[kTFOutfitLbMaxRows];
    };
    static_assert(sizeof(TF_OutfitLeaderboard) == 8 + 11 * 64, "wire layout frozen");

#pragma pack(pop)

    /// Render-site helper: "[TAG] P7" when tag is non-empty, "P7" otherwise.
    /// For the nameplate/killfeed/scoreboard one-line integrations (wave
    /// wiringNotes) — pairs with TFOutfitSystem::GetOutfitTag.
    inline void OutfitTaggedLabel(const char* tag, const char* base, char* out, size_t outSize)
    {
        if (tag && tag[0] != '\0')
            std::snprintf(out, outSize, "[%s] %s", tag, base);
        else
            std::snprintf(out, outSize, "%s", base);
    }

} // namespace Terrafront
