/**
 * @file TFHUD.h
 * @brief ImGui HUD: health/shield/ammo, crosshair, hitmarkers, killfeed, minimap.
 *
 * OWNERSHIP: this header + TFHUD.cpp belong to ONE implementation agent.
 * The lifecycle below is the frozen module contract (called from Main.cpp) —
 * extend this class freely, but do not change the lifecycle signatures.
 *
 * W1 surface consumed by TFClientNet (frozen for W1):
 *   ShowHitmarker / PushKillfeed / SetCaptureProgress / SetRespawnState
 * W1 additions (documented in the wave report):
 *   ShowDamageFrom  — feed TF_DamageEvent.dirOctant to the 8-octant indicator
 *   SetWeaponStatus — weapon system (client side) feeds name/ammo/reload
 *   SetRank         — progression/TFClientNet feeds TF_XPEvent.newRank
 */
#pragma once

#include "Core/TFTypes.h"
#include "Core/TFEvents.h"

#include <cstdint>
#include <deque>
#include <string>

namespace Terrafront {

class TFHUD {
  public:
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
    void PushKillfeed(const char* killer, const char* weapon, const char* victim,
                      FactionId killerF, FactionId victimF);
    void SetCaptureProgress(float progress01, FactionId capturing, bool visible);
    void SetRespawnState(bool dead, float secondsLeft);

    // --- W1 additions (see file header) ---------------------------------
    void ShowDamageFrom(uint8_t dirOctant);
    void SetWeaponStatus(const char* name, int mag, int reserve, bool reloading);
    void SetRank(uint16_t rank);

  private:
    struct PawnView {
        bool    valid{false};
        bool    alive{false};
        float   health{0.0f}, shield{0.0f};
        float   maxHealth{500.0f}, maxShield{500.0f};
        float   speed{0.0f};              // horizontal, m/s (crosshair spread)
        ClassId cls{ClassId::Striker};
    };

    struct KillfeedEntry {
        std::string killer, weapon, victim;
        FactionId   killerF{FactionId::None};
        FactionId   victimF{FactionId::None};
        float       ttl{0.0f};
    };

    void GatherPawnView();

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

    TFGameContext* m_ctx{nullptr};
    TFEventBus*    m_events{nullptr};
    bool           m_initialized{false};

    PawnView m_view{};
    ClassId  m_lastClass{ClassId::Striker};   // used for SPACE-to-deploy request
    uint16_t m_rank{1};

    // Hitmarker
    float m_hitTimer{0.0f};
    bool  m_hitKilled{false};

    // Damage direction flashes (one intensity per octant, 0 = ahead, clockwise)
    float m_octant[8]{};

    // Killfeed (newest at front)
    std::deque<KillfeedEntry> m_killfeed;

    // Capture progress (W2 fills in; TF_CaptureTick placeholder)
    float     m_captureProgress{0.0f};
    FactionId m_captureFaction{FactionId::None};
    bool      m_captureVisible{false};
    float     m_captureTTL{0.0f};

    // Respawn overlay
    bool  m_dead{false};
    float m_respawnLeft{0.0f};
    float m_deployCooldown{0.0f};   // debounce for SPACE deploy requests

    // Weapon status (fed by SetWeaponStatus; -1 ammo == not wired yet)
    std::string m_weapName;
    int         m_weapMag{-1};
    int         m_weapReserve{-1};
    bool        m_weapReloading{false};
};

} // namespace Terrafront
