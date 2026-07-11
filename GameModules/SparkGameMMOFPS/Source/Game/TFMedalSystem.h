/**
 * @file TFMedalSystem.h
 * @brief Medals (killstreak / multikill / Savior / Avenger) + the server-tracked
 *        score rows behind the scoreboard (W10 medals-scoreboard lane).
 *
 * OWNERSHIP: medals-scoreboard lane (this header + TFMedalSystem.cpp, plus
 * UI/TFScoreboard.h/.cpp).
 *
 * Design (TFAbilitySystem/TFOutfitSystem precedent: server registry + client
 * mirror in one class; wire ids + packed structs live HERE, not in
 * TFNetProtocol.h):
 *  - SERVER (authority roles): detects medals purely from EXISTING bus events —
 *      EvPlayerKilled  -> kill/death/score tallies (same suicide/team-kill
 *                         filter as TFProgressionSystem/TFScoreboard),
 *                         killstreaks (5/10/15 -> Rampage/Dominating/
 *                         Unstoppable, streak resets on own death), multikills
 *                         (2/3/4 kills within kTFMultikillWindowSec -> Double/
 *                         Triple/Quad) and Avenger (kill the enemy that killed
 *                         YOU within kTFAvengerWindowSec).
 *      EvPlayerDamaged -> recent-damager ring per victim (entity ids resolved
 *                         through TFPlayerSystem::GetPawnByEntity); Savior =
 *                         kill an enemy who damaged one of YOUR squadmates
 *                         (TFSquadSystem::SquadOf) within kTFSaviorWindowSec.
 *      EvXPAwarded     -> capture participation via the canonical capture
 *                         reason codes 4/5/6 (TFDirectiveSystem precedent —
 *                         per-player capture credit already flows through
 *                         TFRegionSystem -> ServerAwardXP; no new event).
 *    Every medal pays XP through the REAL progression hook only:
 *    TFProgressionSystem::ServerAwardXP(player, def.xp, kXPReasonMedal) — the
 *    exact seam directives use (kXPReasonDirective, 11); medals claim 12.
 *    Re-entrancy is safe: ServerAwardXP synchronously fires EvXPAwarded back
 *    at this system, whose handler only accepts reasons 4/5/6.
 *    Score is server truth: kills*100 + captures*250 + medals*50.
 *  - WIRE: reserved TFMsg block 0x5460-0x5463.
 *      0x5460 S->C TF_MedalAward  (owner only; HUD medal toast, drawn by THIS
 *                                  system's RenderUI — no TFHUD edits needed)
 *      0x5461 S->C TF_ScoreUpdate (broadcast; scoreboard row truth on clients)
 *      0x5462-0x5463 free.
 *    Dirty rows are flushed at kTFScoreFlushHz; a late-joiner burst rides the
 *    NetworkManager::GetClients() diff poll (TFAbilitySystem pattern), and the
 *    same diff clears state for leavers (recycled-PlayerId hygiene — no
 *    TFServerSim edit required). Listen-host/standalone local player is fed by
 *    direct mirror calls (TFSquadSystem::SendEchoTo pattern).
 *  - CLIENT: score-row mirror consumed by TFScoreboard (this lane wires itself:
 *    Initialize calls ctx.scoreboard->SetMedalSystem(this), the TFClientNet::
 *    SetChatUI self-registration pattern — context pointers are published
 *    before any Initialize, so boot order is irrelevant) + a medal toast queue
 *    for the LOCAL player, rendered top-center by RenderUI with the shipped
 *    Assets/Textures/MMOFPS/ui/128 medal icons (hex = multikill, star =
 *    killstreak, shield = Savior, ring = Avenger; Savior/Avenger escalate
 *    their icon tier with the session count). Handlers self-register when the
 *    socket connects (TFSquadSystem EnsureClientHandlers pattern) — no
 *    TFClientNet edits.
 *  - State is session-scoped per PlayerId (TFDirectiveSystem precedent); no
 *    disk persistence this wave. Determinism contract untouched: nothing here
 *    feeds movement or collision.
 */
#pragma once

#include "Core/TFTypes.h"
#include "Core/TFEvents.h"

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Terrafront
{

    // ---------------------------------------------------------------------------
    // Wire protocol — reserved TFMsg id block 0x5460-0x5463 (medals-scoreboard
    // lane). TFNetProtocol.h (contended) only gains a block comment via the wave
    // wiringNotes; these constants keep this lane compiling standalone and MUST
    // stay value-identical to any future enum entries.
    // ---------------------------------------------------------------------------

    constexpr uint16_t kTFMsgMedalAward = 0x5460;  // S->C  TF_MedalAward (owner-only toast + career count)
    constexpr uint16_t kTFMsgScoreUpdate = 0x5461; // S->C  TF_ScoreUpdate (broadcast scoreboard row)
    // 0x5462-0x5463 reserved for future medal/score traffic.

    /// XP reason code for medal payouts (TF_XPEvent.reasonCode). 0-10 are the
    /// canonical list in TFProgressionSystem.h; 11 is kXPReasonDirective
    /// (Game/TFDirectiveData.h); 12 is claimed here — keep in sync.
    inline constexpr uint8_t kXPReasonMedal = 12;

    /// Medal identities (wire values — append only).
    enum class TFMedalId : uint8_t
    {
        DoubleKill = 0, ///< 2 kills within the multikill window
        TripleKill,     ///< 3 kills within the multikill window
        QuadKill,       ///< 4 kills within the multikill window
        Rampage,        ///< 5-killstreak (no death)
        Dominating,     ///< 10-killstreak
        Unstoppable,    ///< 15-killstreak
        Savior,         ///< killed an enemy who recently damaged a squadmate
        Avenger,        ///< killed the enemy who recently killed you
        COUNT
    };

#pragma pack(push, 1)

    /// Owner-only medal notification (toast + session count for icon tiering).
    struct TF_MedalAward
    {
        uint32_t player; // earning player (echoed for the local mirror path)
        uint8_t medal;   // TFMedalId
        uint8_t _pad;
        uint16_t sessionCount; // how many of THIS medal the player has this session
    };
    static_assert(sizeof(TF_MedalAward) == 8, "wire layout frozen");

    /// One scoreboard row (server truth, broadcast on change + join burst).
    struct TF_ScoreUpdate
    {
        uint32_t player;
        uint32_t score; // kills*100 + captures*250 + medals*50
        uint16_t kills;
        uint16_t deaths;
        uint16_t captures; // capture participations (XP reasons 4/5/6)
        uint16_t medals;   // total medals earned this session
        uint8_t faction;   // FactionId
        uint8_t cls;       // last-known ClassId (ClassId::COUNT == unknown)
        uint8_t _pad[2];
    };
    static_assert(sizeof(TF_ScoreUpdate) == 20, "wire layout frozen");

#pragma pack(pop)

    // ---------------------------------------------------------------------------
    // Tuning (session-scoped detection windows + the DESIGN score weights)
    // ---------------------------------------------------------------------------

    constexpr float kTFMultikillWindowSec = 4.0f; ///< chain window between kills
    constexpr float kTFSaviorWindowSec = 8.0f;    ///< "recently damaged a squadmate"
    constexpr float kTFAvengerWindowSec = 20.0f;  ///< "recently killed you"
    constexpr float kTFScoreFlushHz = 4.0f;       ///< dirty-row broadcast cadence
    constexpr uint32_t kTFScorePerKill = 100;
    constexpr uint32_t kTFScorePerCapture = 250;
    constexpr uint32_t kTFScorePerMedal = 50;

    /// Static definition of one medal (display name, XP payout, icon mapping).
    /// `iconTier` 1-3 picks medal_<family>_<tier>.png directly; 0 means the
    /// tier escalates with the session count (Savior/Avenger families).
    struct TFMedalDef
    {
        const char* key;        ///< stable lowercase key (logs/debug)
        const char* name;       ///< toast display name
        uint16_t xp;            ///< paid via ServerAwardXP(kXPReasonMedal)
        const char* iconFamily; ///< "hex" | "star" | "shield" | "ring"
        uint8_t iconTier;       ///< 1..3 fixed, 0 = session-count tiered
    };

    /// Table lookup (safe for any uint8_t — out-of-range ids get a fallback row).
    const TFMedalDef& TFMedalDefOf(uint8_t medalId);

    /// Full asset path of the 128 px icon for a medal at `sessionCount` earned
    /// (session-count tiers: <10 -> _1, <50 -> _2, else _3 for tiered medals).
    std::string TFMedalIconPath(uint8_t medalId, uint16_t sessionCount);

    /// One player's scoreboard row as consumed by TFScoreboard (server truth on
    /// authority roles, TF_ScoreUpdate mirror on pure clients).
    struct TFScoreRow
    {
        uint32_t score = 0;
        uint16_t kills = 0;
        uint16_t deaths = 0;
        uint16_t captures = 0;
        uint16_t medals = 0;
        FactionId faction = FactionId::None;
        ClassId cls = ClassId::COUNT; ///< COUNT == unknown
    };

    // ---------------------------------------------------------------------------
    // System
    // ---------------------------------------------------------------------------

    class TFMedalSystem
    {
      public:
        TFMedalSystem();
        ~TFMedalSystem();

        bool Initialize(TFGameContext& ctx, TFEventBus& events);
        void Update(float deltaTime);
        void FixedUpdate(float fixedDeltaTime);
        void Shutdown();
        void RenderDebugUI();
        void RenderUI(); ///< medal toast overlay (local player only)

        // --- cross-system surface (TFScoreboard) --------------------------------

        /// Row for one player (false if unknown). Authority: server truth;
        /// pure client: last TF_ScoreUpdate mirror.
        bool GetScoreRow(PlayerId player, TFScoreRow& out) const;

        /// Visit every known row (same source split as GetScoreRow).
        void ForEachScoreRow(const std::function<void(PlayerId, const TFScoreRow&)>& fn) const;

        /// Session count of one medal for a player (authority only; 0 otherwise).
        uint16_t MedalCountOf(PlayerId player, TFMedalId medal) const;

        /// Drop a player's medal/score state (recycled-PlayerId hygiene). Called
        /// internally from the GetClients() leaver diff; public for tests/console.
        void ClearPlayer(PlayerId player);

      private:
        // --- server state ---
        struct ServerRec
        {
            TFScoreRow row;
            std::array<uint16_t, static_cast<size_t>(TFMedalId::COUNT)> medalCounts{};
            uint32_t streak = 0; ///< kills since last own death
            uint32_t chain = 0;  ///< kills inside the multikill window
            double lastKillAt = -1.0e9;
            bool dirty = false;
        };
        struct Damager
        {
            PlayerId attacker = kInvalidPlayer;
            double at = 0.0;
        };
        struct LastKiller
        {
            PlayerId killer = kInvalidPlayer;
            double at = 0.0;
        };

        // --- client toast ---
        struct Toast
        {
            uint8_t medal = 0;
            uint16_t sessionCount = 0;
            float ttl = 0.0f;
        };

        double NowSec() const;
        ServerRec& Ensure(PlayerId player);
        void RefreshIdentity(PlayerId player, ServerRec& rec); ///< faction/cls from TFPlayerSystem
        static void RecomputeScore(ServerRec& rec);

        void OnPlayerKilled(const EvPlayerKilled& ev);
        void OnPlayerDamaged(const EvPlayerDamaged& ev);
        void OnXPAwarded(const EvXPAwarded& ev);

        void Award(PlayerId player, ServerRec& rec, TFMedalId medal);
        void CheckSavior(PlayerId killer, PlayerId victim, ServerRec& kr, double now);
        void CheckAvenger(PlayerId killer, PlayerId victim, ServerRec& kr, double now);

        void FlushDirtyRows();
        void BroadcastRow(PlayerId player, const ServerRec& rec);
        void SendMedalToOwner(PlayerId player, const TF_MedalAward& award);

        // client internals
        void ClientHandleScore(const TF_ScoreUpdate& st);
        void ClientHandleMedal(const TF_MedalAward& award);

#ifdef ENABLE_NETWORKING
        bool ClientNetActive() const;
        void EnsureClientHandlers();
        void ReleaseClientHandlers();
        void SendWire(PlayerId target, uint16_t msgId, const void* payload, size_t size);
        void PollJoinsLeaves(); ///< late-joiner burst + leaver ClearPlayer
#endif

        TFGameContext* m_ctx{nullptr};
        TFEventBus* m_events{nullptr};
        bool m_initialized{false};
        double m_clock{0.0}; ///< fallback time base when serverSim is absent

        // server state (authority only)
        std::unordered_map<PlayerId, ServerRec> m_server;
        std::unordered_map<PlayerId, std::vector<Damager>> m_recentDamage; ///< victim -> recent attackers
        std::unordered_map<PlayerId, LastKiller> m_lastKiller;             ///< victim -> who killed them last
        float m_flushAccum{0.0f};
#ifdef ENABLE_NETWORKING
        std::unordered_set<PlayerId> m_knownClients; ///< late-joiner burst / leaver sweep
#endif
        bool m_clientHandlers{false};

        // client state (any role with a local player)
        std::unordered_map<PlayerId, TFScoreRow> m_mirror;
        std::deque<Toast> m_toasts;

        // debug counters
        uint32_t m_medalsAwarded{0};
        uint32_t m_rowsSent{0};
        uint32_t m_badPackets{0};
    };

} // namespace Terrafront
