/**
 * @file TFHUD.h
 * @brief ImGui HUD: health/shield/ammo, crosshair, hitmarkers, killfeed,
 *        damage direction indicators, death panel, minimap.
 *
 * OWNERSHIP: this header + the TFHUD*.cpp translation units (TFHUD.cpp core,
 * TFHUDDraw.cpp overlay drawing, TFHUDChat.cpp chat, TFHUDCombat.cpp combat
 * state, TFHUDCombatDraw.cpp combat drawing, TFHUDInternal.h shared
 * internals) belong to ONE implementation agent (split per the repo
 * file-size rule).
 * The lifecycle below is the frozen module contract (called from Main.cpp) —
 * extend this class freely, but do not change the lifecycle signatures.
 *
 * W1 surface consumed by TFClientNet (frozen for W1):
 *   ShowHitmarker / PushKillfeed / SetCaptureProgress / SetRespawnState
 * W1 additions (documented in the wave report):
 *   ShowDamageFrom  — feed TF_DamageEvent.dirOctant to the 8-octant indicator
 *   SetWeaponStatus — weapon system (client side) feeds name/ammo/reload
 *   SetRank         — progression/TFClientNet feeds TF_XPEvent.newRank
 * W6 combat-HUD additions (this wave, additive):
 *   PushKillfeed (extended overload) — headshot marker + player ids for the
 *     local-row highlight and the death panel. The 5-arg overload stays and
 *     forwards here; upgrading the TFClientNet call site is optional wiring.
 *   LastDeath — killer/weapon/distance summary of the last local death;
 *     rendered on the HUD death overlay and by TFSpawnScreen's deploy header.
 *   World-anchored damage pings — resolved from EvPlayerDamaged + attacker
 *     pawn positions so the indicator rotates with the camera; the octant
 *     flash (ShowDamageFrom) stays as the fallback when no position is known.
 *   Minimap v2 — player-centered zoomed view with same-faction deployables
 *     (TFDeployableSystem client mirror) and friendly/squad pawn markers.
 */
#pragma once

#include "Core/TFTypes.h"
#include "Core/TFEvents.h"
#include "Net/TFNetProtocol.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>

namespace Terrafront
{

    class TFHUD
    {
      public:
        /// What killed the local player last. Valid from the killing blow until
        /// the next local spawn (death panel + TFSpawnScreen deploy header).
        struct DeathSummary
        {
            bool valid{false};
            bool headshot{false};
            float distanceM{-1.0f}; ///< < 0 == unknown
            std::string killerName;
            std::string weaponName;
        };

        TFHUD();
        ~TFHUD();

        bool Initialize(TFGameContext& ctx, TFEventBus& events);
        void Update(float deltaTime);
        void FixedUpdate(float fixedDeltaTime);
        void Shutdown();
        void RenderDebugUI();
        void RenderUI();

        // --- W1 consumer surface (called by TFClientNet) --------------------
        void ShowHitmarker(bool killed);
        void PushKillfeed(const char* killer, const char* weapon, const char* victim, FactionId killerF,
                          FactionId victimF);
        void SetCaptureProgress(float progress01, FactionId capturing, bool visible);
        void SetRespawnState(bool dead, float secondsLeft);

        // --- W1 additions (see file header) ---------------------------------
        void ShowDamageFrom(uint8_t dirOctant);
        void SetWeaponStatus(const char* name, int mag, int reserve, bool reloading);
        void SetRank(uint16_t rank);
        bool IsChatOpen() const { return m_chatOpen; }

        // --- W6 additions (see file header) ----------------------------------
        /// Extended killfeed push: headshot marker + player ids (used for the
        /// local-row highlight and to capture the death panel data on pure
        /// clients). Pass kInvalidPlayer ids when unknown.
        void PushKillfeed(const char* killer, const char* weapon, const char* victim, FactionId killerF,
                          FactionId victimF, bool headshot, PlayerId killerPlayer, PlayerId victimPlayer);

        /// Last local death; `valid` is cleared when the local pawn respawns.
        const DeathSummary& LastDeath() const { return m_death; }

      private:
        struct PawnView
        {
            bool valid{false};
            bool alive{false};
            float health{0.0f}, shield{0.0f};
            float maxHealth{500.0f}, maxShield{500.0f};
            float speed{0.0f}; // horizontal, m/s (crosshair spread)
            float pos[3]{};    // feet position (world)
            ClassId cls{ClassId::Striker};
        };

        struct KillfeedEntry
        {
            std::string killer, weapon, victim;
            FactionId killerF{FactionId::None};
            FactionId victimF{FactionId::None};
            bool headshot{false};
            bool involvesLocal{false};
            bool annotated{false}; ///< headshot/ids already resolved
            float ttl{0.0f};
        };

        /// World-anchored damage ping — unlike the octant flashes these rotate
        /// with the camera because the attacker direction is stored in world XZ.
        struct DamagePing
        {
            float dirX{0.0f}, dirZ{0.0f}; ///< unit, victim -> attacker
            float intensity{0.0f};        ///< 0 == free slot
        };

        /// EvPlayerKilled details that can arrive before OR after the matching
        /// PushKillfeed call (bus subscriber order is not guaranteed).
        struct PendingKill
        {
            bool valid{false};
            bool headshot{false};
            PlayerId killer{kInvalidPlayer};
            PlayerId victim{kInvalidPlayer};
            float ttl{0.0f};
        };

        void GatherPawnView();
        void OnPlayerKilledBus(const EvPlayerKilled& ev);
        void OnPlayerDamagedBus(const EvPlayerDamaged& ev);
        void RecordDeath(PlayerId killer, WeaponId weapon, bool headshot);
        void AddWorldPing(const float attackerPos[3], const float myPos[3]);
        bool ResolvePlayerPosition(PlayerId player, float out[3]) const;
        bool ResolveEntityPosition(EntityId entity, float out[3]) const;

        // Drawing helpers — called between the overlay window's Begin/End;
        // each fetches ImGui state itself so no ImGui types leak into this header.
        void DrawVitals();
        void DrawWeaponBox();
        void DrawCrosshairAndHitmarker();
        void DrawKillfeed();
        void DrawDamageOctants();
        void DrawCaptureBar();
        void DrawRespawnOverlay();
        void DrawCompass();
        void DrawMinimap();
        void DrawChat();
        void DrawDeathActions(); ///< clickable DEPLOY/MAP buttons (own window)
        void CloseChat();

        TFGameContext* m_ctx{nullptr};
        TFEventBus* m_events{nullptr};
        bool m_initialized{false};

        PawnView m_view{};
        bool m_wasViewAlive{false};            // dead->alive edge clears m_death
        float m_lastAlivePos[3]{};             // death-distance anchor
        ClassId m_lastClass{ClassId::Striker}; // used for SPACE-to-deploy request
        uint16_t m_rank{1};

        // Hitmarker
        float m_hitTimer{0.0f};
        bool m_hitKilled{false};

        // Damage direction flashes (one intensity per octant, 0 = ahead, clockwise)
        float m_octant[8]{};
        // World-anchored pings + octant/ping dedup (one hit must not draw both)
        static constexpr size_t kMaxPings = 8;
        DamagePing m_pings[kMaxPings]{};
        float m_octantSuppress{0.0f};  ///< skip octant flashes just after a ping
        int m_lastOctant{-1};          ///< octant set most recently...
        float m_lastOctantAge{999.0f}; ///< ...and how long ago (same-hit cleanup)

        // Killfeed (newest at front)
        std::deque<KillfeedEntry> m_killfeed;
        PendingKill m_pendingKill;

        // Death panel
        DeathSummary m_death;

        // Capture progress (W2 fills in; TF_CaptureTick placeholder)
        float m_captureProgress{0.0f};
        FactionId m_captureFaction{FactionId::None};
        bool m_captureVisible{false};
        float m_captureTTL{0.0f};

        // Respawn overlay
        bool m_dead{false};
        float m_respawnLeft{0.0f};
        float m_deployCooldown{0.0f}; // debounce for SPACE deploy requests

        // Weapon status (fed by SetWeaponStatus; -1 ammo == not wired yet)
        std::string m_weapName;
        int m_weapMag{-1};
        int m_weapReserve{-1};
        bool m_weapReloading{false};

        bool m_chatOpen{false};
        bool m_focusChatInput{false};
        bool m_chatReleasedMouse{false};
        ChatChannel m_chatChannel{ChatChannel::Region};
        char m_chatInput[122]{};
    };

} // namespace Terrafront
