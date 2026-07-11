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
 */
#pragma once

#include "Core/TFTypes.h"
#include "Core/TFEvents.h"

#include <functional>
#include <unordered_map>

namespace Terrafront
{

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

        TFGameContext* m_ctx{nullptr};
        TFEventBus* m_events{nullptr};
        bool m_initialized{false};
        double m_clock{0.0};

        std::unordered_map<EntityId, HealthRec> m_pools;
        std::unordered_map<PlayerId, uint32_t> m_teamKills;
        uint32_t m_killCount{0};

        /// class-abilities lane (W9): optional pre-pool damage filter (see setter).
        IncomingDamageFilter m_incomingFilter;
    };

} // namespace Terrafront
