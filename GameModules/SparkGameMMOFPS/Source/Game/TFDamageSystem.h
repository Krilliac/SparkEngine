/**
 * @file TFDamageSystem.h
 * @brief Health/shields, TTK model, friendly fire, kill credit.
 *
 * OWNERSHIP: this header + TFDamageSystem.cpp belong to ONE implementation agent.
 * The lifecycle + ServerApplyDamage are the frozen module contract.
 *
 * W1: server-authoritative pools tracked here (health + shield per pawn,
 * seeded from classes.json on EvPlayerSpawned), shield-first absorb, faction
 * regen delay, friendly fire at 50% with TK tally (log-only W1), kill credit
 * via TFPlayerSystem::ServerKillPawn, TF_HitConfirm / TF_DamageEvent /
 * TF_KillEvent feedback. TF-W2: move pools onto ECS components + grief kick.
 *
 * W11 death-recap lane (damage-log section, owned by that lane): per-pawn
 * rolling damage log (TFDamageLog, last kTFDeathRecapWindowSec) recorded in
 * ServerApplyDamage; on death the victim gets TF_DeathRecap (0x5474, reliable,
 * victim only) — timeline + killer summary + the victim's damage on the
 * killer. Wire structs live below (TFMedalSystem in-lane-header precedent);
 * UI/TFDeathRecap.h/.cpp renders the client panel.
 */
#pragma once

#include "Core/TFTypes.h"
#include "Core/TFEvents.h"

#include <array>
#include <functional>
#include <unordered_map>

namespace Terrafront
{

    // ---------------------------------------------------------------------------
    // Death-recap wire protocol — reserved TFMsg id block 0x5474-0x5477 (W11
    // death-recap lane). TFNetProtocol.h (contended) only gains a block comment
    // via the wave wiringNotes; these constants keep the lane compiling
    // standalone (TFMedalSystem 0x5460 precedent) and MUST stay value-identical
    // to any future enum entries.
    // ---------------------------------------------------------------------------

    constexpr uint16_t kTFMsgDeathRecap = 0x5474; // S->C TF_DeathRecap (reliable, victim only)
    // 0x5475-0x5477 reserved for future death-recap traffic.

    /// Rolling damage-log window (seconds before death) and the ring/wire cap.
    constexpr float kTFDeathRecapWindowSec = 8.0f;
    constexpr uint32_t kTFDeathRecapMaxEntries = 8;

#pragma pack(push, 1)

    /// One timeline row: ids/amounts only — weapon icon + name resolve
    /// client-side from WeaponId (keeps the wire small).
    struct TF_DeathRecapEntry
    {
        uint32_t attackerPlayer; // kInvalidPlayer == environment/unknown
        uint16_t weaponId;       // WeaponId (kInvalidWeapon when unknown)
        uint16_t damage;         // post-mitigation, rounded down
        uint16_t ageDeciSec;     // how long before death this hit landed (0.1 s units)
        uint8_t headshot;        // 0/1
        uint8_t damageKind;      // TF_DamageEvent vocabulary (0 bullet, 1 explosive, ...)
    };
    static_assert(sizeof(TF_DeathRecapEntry) == 12, "wire layout frozen");

    /// Sent to the victim only, on death. Entries are oldest-first.
    struct TF_DeathRecap
    {
        uint8_t entryCount;      // valid rows in entries[] (0..kTFDeathRecapMaxEntries)
        uint8_t killerClass;     // ClassId; ClassId::COUNT == unknown
        uint8_t killerFaction;   // FactionId
        uint8_t hitsOnKiller;    // the victim's hits on the killer inside the window
        uint32_t killerPlayer;   // kInvalidPlayer == environment/unknown
        uint16_t killerHealth;   // killer HP left at the victim's death; 0xFFFF unknown
        uint16_t killerShield;   // 0xFFFF unknown
        uint16_t damageToKiller; // total damage the victim did to the killer inside the window
        uint16_t distanceDm;     // killer distance in decimeters; 0xFFFF unknown
        TF_DeathRecapEntry entries[kTFDeathRecapMaxEntries];
    };
    static_assert(sizeof(TF_DeathRecap) == 16 + 12 * kTFDeathRecapMaxEntries, "wire layout frozen");

#pragma pack(pop)

    // ---------------------------------------------------------------------------
    // Killcam wire protocol — reserved TFMsg id block 0x5484-0x5487 (W13 killcam
    // lane). Same in-lane-header precedent as the death-recap block above:
    // TFNetProtocol.h (contended) only gains a block comment via the wave
    // wiringNotes; these values MUST stay identical to any future enum entries.
    // ---------------------------------------------------------------------------

    constexpr uint16_t kTFMsgKillcamData = 0x5484; // S->C TF_KillcamData (reliable, victim only)
    // 0x5485-0x5487 reserved for future killcam traffic.

#pragma pack(push, 1)

    /// Sent to the victim only, on death, alongside TF_DeathRecap: who killed
    /// you + a server-truth pose snapshot. The client killcam (TFSpectator
    /// Mode::Killcam) follows the killer's replicated pawn live and falls back
    /// to this snapshot when the pawn is no longer resolvable (killer died /
    /// despawned / respawned into a new pawn). No replay buffer — one sample.
    struct TF_KillcamData
    {
        uint32_t killerPlayer; // kInvalidPlayer == environment (client shows no killcam)
        uint32_t killerEntity; // network EntityId of the killer pawn at the kill (0 == unknown)
        float killerPos[3];    // killer position at the victim's death (server truth)
        float killerYawRad;    // killer yaw at the victim's death (radians)
        uint8_t hasPose;       // 0 == pos/yaw invalid (killer pawn already gone server-side)
    };
    static_assert(sizeof(TF_KillcamData) == 25, "wire layout frozen");

#pragma pack(pop)

    /// One recorded hit in a pawn's rolling damage log (server bookkeeping).
    struct TFDamageLogHit
    {
        PlayerId attackerPlayer = kInvalidPlayer;
        WeaponId weapon = kInvalidWeapon;
        float amount = 0.0f; // post-mitigation
        uint8_t kind = 0;    // TF_DamageEvent vocabulary
        bool headshot = false;
        double at = 0.0; // TFDamageSystem clock seconds
    };

    /// Fixed 8-slot ring of the most recent hits on one pawn. Header-only and
    /// self-contained so the wire/window behavior is testable without compiling
    /// the module .cpp into SparkTests (TestTFRedeployRules pattern).
    class TFDamageLog
    {
      public:
        void Push(const TFDamageLogHit& hit)
        {
            m_hits[m_head] = hit;
            m_head = (m_head + 1) % kTFDeathRecapMaxEntries;
            if (m_count < kTFDeathRecapMaxEntries)
                ++m_count;
        }

        /// Window-filtered hits, oldest first. Returns the row count (0..8).
        uint32_t Collect(double now, float windowSec, TFDamageLogHit out[kTFDeathRecapMaxEntries]) const
        {
            uint32_t n = 0;
            const uint32_t start = (m_head + kTFDeathRecapMaxEntries - m_count) % kTFDeathRecapMaxEntries;
            for (uint32_t i = 0; i < m_count; ++i)
            {
                const TFDamageLogHit& h = m_hits[(start + i) % kTFDeathRecapMaxEntries];
                if (now - h.at <= static_cast<double>(windowSec))
                    out[n++] = h;
            }
            return n;
        }

        /// Total damage + hit count from one attacker inside the window.
        void SumFrom(PlayerId attacker, double now, float windowSec, float& outDamage, uint32_t& outHits) const
        {
            outDamage = 0.0f;
            outHits = 0;
            if (attacker == kInvalidPlayer)
                return;
            TFDamageLogHit rows[kTFDeathRecapMaxEntries];
            const uint32_t n = Collect(now, windowSec, rows);
            for (uint32_t i = 0; i < n; ++i)
            {
                if (rows[i].attackerPlayer != attacker)
                    continue;
                outDamage += rows[i].amount;
                ++outHits;
            }
        }

      private:
        std::array<TFDamageLogHit, kTFDeathRecapMaxEntries> m_hits{};
        uint32_t m_head{0};
        uint32_t m_count{0};
    };

    class TFDamageSystem
    {
      public:
        TFDamageSystem();
        ~TFDamageSystem();

        bool Initialize(TFGameContext& ctx, TFEventBus& events);
        void Update(float deltaTime);
        void FixedUpdate(float fixedDeltaTime);
        void Shutdown();
        void RenderDebugUI();

        // --- FROZEN cross-system API (W1) ---
        void ServerApplyDamage(EntityId victim, EntityId attackerPawn, PlayerId attackerPlayer, float amount,
                               uint8_t kind, WeaponId weapon, bool headshot);

        /// Current pools (server). Returns false if the pawn is untracked.
        bool GetPools(EntityId pawn, float& outHealth, float& outShield) const;

        // --- W3 shared-edit additions (deployables/colossus agent; minimal &
        //     additive per the wave-3 coordination grant) ------------------------

        /// Server: heal a tracked pawn's HEALTH pool (shield untouched, never
        /// revives, clamped to maxHealth). Used by Medtech beacons / Fabricator
        /// packs. No-op for unknown or dead pawns.
        void ServerHeal(EntityId pawn, float amount);

        /// Server: drop bookkeeping for a pawn despawned WITHOUT dying (Colossus
        /// purchase swap; the kill path already erases its own record). Prevents
        /// stale HealthRec entries from accumulating across suit swaps.
        void ServerForgetPawn(EntityId pawn);

        // --- class-abilities lane (W9): one-function ability damage seam --------

        /// Optional pre-pool damage filter (Bulwark Field absorb). Installed by
        /// TFAbilitySystem::Initialize and uninstalled (nullptr) by its Shutdown
        /// so no dangling `this` survives; the returned amount is clamped to
        /// [0, amount] — a filter can only REDUCE damage, never amplify it.
        using IncomingDamageFilter = std::function<float(EntityId victim, float amount)>;
        void SetIncomingDamageFilter(IncomingDamageFilter filter) { m_incomingFilter = std::move(filter); }

        // --- death-recap lane (W11): local-victim mirror hook -------------------

        /// Listen-host/standalone victims have no socket, so TF_DeathRecap can't
        /// ride SendToOwner for them (TFMedalSystem::SendMedalToOwner precedent).
        /// TFDeathRecap installs this from its Initialize and uninstalls
        /// (nullptr) in its Shutdown so no dangling `this` survives (the
        /// TFAbilitySystem incoming-filter pattern).
        using DeathRecapMirror = std::function<void(const TF_DeathRecap&)>;
        void SetDeathRecapMirror(DeathRecapMirror mirror) { m_recapMirror = std::move(mirror); }

        // --- killcam lane (W13): local-victim mirror hook ------------------------

        /// Listen-host/standalone victims have no socket, so TF_KillcamData can't
        /// ride SendToOwner for them (SetDeathRecapMirror precedent, identical
        /// lifecycle: TFDeathRecap installs this from its Initialize and
        /// uninstalls (nullptr) in its Shutdown so no dangling `this` survives).
        using KillcamMirror = std::function<void(const TF_KillcamData&)>;
        void SetKillcamMirror(KillcamMirror mirror) { m_killcamMirror = std::move(mirror); }

      private:
        struct HealthRec
        {
            float health = 500, maxHealth = 500;
            float shield = 500, maxShield = 500;
            float regenDelaySec = 6.0f;
            double lastDamageAt = -1.0e9;
            bool noRegen = false;
            FactionId faction = FactionId::None;
            PlayerId owner = kInvalidPlayer;
        };

        void OnPawnSpawned(const EvPlayerSpawned& ev);
        void SendToOwner(PlayerId owner, uint16_t msgId, const void* payload, size_t size);
        void BroadcastKill(PlayerId killer, PlayerId victim, WeaponId weapon, FactionId killerF, FactionId victimF,
                           bool headshot);

        /// death-recap lane (W11): assemble + deliver TF_DeathRecap to the
        /// victim. Must run while the pawn registry still holds both pawns
        /// (i.e. before ServerKillPawn despawns the victim).
        void SendDeathRecap(EntityId victim, const HealthRec& rec, EntityId attackerPawn, PlayerId attackerPlayer,
                            FactionId attackerFaction);

        /// killcam lane (W13): deliver TF_KillcamData to the victim (skipped for
        /// environment deaths and suicides). Runs next to SendDeathRecap, before
        /// ServerKillPawn despawns anything, so the killer pawn registry entry is
        /// still the freshest server truth.
        void SendKillcam(const HealthRec& rec, EntityId attackerPawn, PlayerId attackerPlayer);

        TFGameContext* m_ctx{nullptr};
        TFEventBus* m_events{nullptr};
        bool m_initialized{false};
        double m_clock{0.0};

        std::unordered_map<EntityId, HealthRec> m_pools;
        std::unordered_map<PlayerId, uint32_t> m_teamKills;
        uint32_t m_killCount{0};

        /// class-abilities lane (W9): optional pre-pool damage filter (see setter).
        IncomingDamageFilter m_incomingFilter;

        // --- death-recap lane (W11) ---------------------------------------------
        /// Per-pawn rolling damage log (erased with the pool record: kill path,
        /// ServerForgetPawn, Shutdown).
        std::unordered_map<EntityId, TFDamageLog> m_damageLog;
        /// Local-victim delivery hook (see SetDeathRecapMirror).
        DeathRecapMirror m_recapMirror;
        uint32_t m_recapsSent{0};

        // --- killcam lane (W13) ---------------------------------------------------
        /// Local-victim delivery hook (see SetKillcamMirror).
        KillcamMirror m_killcamMirror;
        uint32_t m_killcamsSent{0};
    };

} // namespace Terrafront
