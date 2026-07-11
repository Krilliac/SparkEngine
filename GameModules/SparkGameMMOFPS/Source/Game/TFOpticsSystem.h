/**
 * @file TFOpticsSystem.h
 * @brief Weapon optics (W11 weapon-optics lane): per-weapon sight selection
 *        (iron sights / red dot / 4x scope) — ADS FOV zoom, screen-space
 *        reticle overlays, scoped hold-breath sway, B-key sight cycling.
 *
 * OWNERSHIP: this header + TFOpticsSystem.cpp belong to the weapon-optics
 * lane (together with Game/TFViewModel.cpp's ADS/optics section).
 *
 * CLIENT PRESENTATION ONLY — deliberately zero gameplay authority. Spread,
 * recoil and damage stay data-driven server truth (weapons.json spreadAdsDeg/
 * recoil*, validated in TFWeaponServer.cpp); the fire ray is still built from
 * the UNswayed camera pose in TFWeaponSystem::BuildViewRay. The only thing an
 * optic changes is what the local player SEES: projection FOV, a reticle
 * overlay, and viewmodel visibility. No client-side accuracy advantage is
 * encoded here beyond image magnification itself.
 *
 * Optics per weapon (additive weapons.json field, parsed HERE — not in
 * TFDataTables, whose public surface is a frozen contended contract):
 *     "optics": ["irons", "reddot", "x4"]
 *  - absent field == ["irons"] == exact pre-W11 ADS behavior;
 *  - the FIRST entry is that weapon's default sight (snipers author "x4"
 *    first, so they raise scoped — sniper standard);
 *  - irons: no zoom, no overlay. reddot: mild 0.9x-FOV zoom + dot reticle.
 *    x4: quarter-FOV zoom + scope vignette/crosshair, viewmodel hidden while
 *    fully scoped, ADS blend slowed to adsSec * 1.5, gentle hold-breath sway.
 *
 * Wiring (see the lane wiring notes for the exact contended-file snippets):
 *  - TFViewModel::Render calls Update once per frame (that call site already
 *    gates on gfx + alive + unseated + armed) and skips the weapon/arms/flash
 *    draws while HideViewModel(); TFViewModel::Reset routes to NotifyInactive
 *    so death/vehicle-entry/holster snap the zoom out.
 *  - TFNameplates::RenderUI calls RenderOverlay in the OnImGui phase (module
 *    frame order: OnRender draws 3D, the pre-present hook runs NewFrame ->
 *    OnImGui -> Render — foreground-drawlist writes from OnRender are lost),
 *    downstream of its fullscreen-UI/death gates so the scope overlay yields
 *    to map/spawn/scoreboard exactly like plates do.
 *  - TFWorldSetup::ComputeViewProj (contended; snippet in wiringNotes)
 *    multiplies its first-person FOV by CameraFovScale() and rotates the view
 *    forward by CameraSwayRad(...). Without that hook the overlays and sight
 *    cycling still work — there is just no zoom/sway (graceful degrade).
 *  - UI/TFNameplates mirrors CameraFovScale() in its matching hardcoded
 *    projection so plates stay glued to pawns while zoomed.
 *
 * Selection persistence: SESSION-LOCAL by design this wave. The durable
 * loadout path (TFLoadout -> TFDatabase::SaveCharacterMeta and the
 * TF_LoadoutChange wire message) has a fixed field set on contended files, so
 * a per-weapon optic key cannot ride it without cross-lane edits. The store
 * below keys selections by weapons.json weapon KEY so a future wave can lift
 * it into TFLoadout verbatim (see the wave report follow-up note).
 *
 * Why a singleton: same justification as TFViewModel/TFImpactFx — one local
 * client, pure presentation, and the producers (viewmodel render path, the
 * contended camera path) each stay a single line of wiring.
 */
#pragma once

#include "Core/TFTypes.h"

#include <cstdint>
#include <string>
#include <unordered_map>

namespace Terrafront
{

    /// Sight kinds, in cycle order. Values are bit indices in the allowed mask.
    enum class TFOpticKind : uint8_t
    {
        Irons = 0,
        RedDot = 1,
        X4 = 2,
    };

    class TFOpticsSystem
    {
      public:
        static TFOpticsSystem& Get();

        /// Advance the ADS blend / sway clocks and handle the B-key sight
        /// cycle. Call once per frame from TFViewModel::Render AFTER its gates
        /// (local pawn alive, not seated, weapon in hand) with the same
        /// clamped dt. NOTE the B key also toggles the Aegis deploy — that
        /// path only runs while SEATED, and this one only while on foot, so
        /// they never clash.
        void Update(TFGameContext& ctx, float dt);

        /// Draw the reticle/scope overlays + the sight-cycle toast onto the
        /// ImGui foreground draw list (headless no-op). MUST run in the
        /// OnImGui phase (between NewFrame and Render) — drawlist writes from
        /// OnRender are discarded by the overlay's own NewFrame. Hooked from
        /// TFNameplates::RenderUI after its fullscreen-UI/death suppression
        /// gates, which are exactly the gates a scope overlay wants too.
        void RenderOverlay();

        /// Snap the optic state idle (blend -> 0, no overlay). Called from
        /// TFViewModel::Reset (death / vehicle entry / weapon holster).
        void NotifyInactive();

        /// Multiplier for the first-person projection FOV angle: 1 at hip,
        /// blending to 0.9 (red dot) or 0.25 (4x) while ADS. Consumed by
        /// TFWorldSetup::ComputeViewProj and mirrored by TFNameplates.
        float CameraFovScale() const;

        /// Scoped hold-breath sway (radians), motion-sickness-safe amplitudes.
        /// Non-zero only while near-fully scoped with the 4x optic. Applied to
        /// the VIEW basis only — never to fire rays (visual-only by contract).
        void CameraSwayRad(float& outYawRad, float& outPitchRad) const;

        /// True while fully scoped with the 4x optic: TFViewModel skips the
        /// weapon/arms/flash draws (standard sniper-scope presentation).
        bool HideViewModel() const;

        /// Current sight on the held weapon (Irons when nothing is resolved).
        TFOpticKind ActiveOptic() const { return m_optic; }

        /// 0..1 aim-down-sights blend for the current sight (0 == hip).
        float AdsBlend() const { return m_blend; }

      private:
        TFOpticsSystem() = default;

        /// Allowed-sight set for one weapon: bit (1 << TFOpticKind) mask plus
        /// the authored default (first JSON entry).
        struct OpticList
        {
            uint8_t mask = 1u << static_cast<uint8_t>(TFOpticKind::Irons);
            uint8_t defaultSel = static_cast<uint8_t>(TFOpticKind::Irons);
        };

        /// Lazy one-shot parse of the additive "optics" arrays straight from
        /// Assets/MMOFPS/Data/weapons.json (JsonUtils, tolerant: missing file
        /// or field just leaves weapons irons-only).
        void EnsureTable();

        static const char* OpticDisplayName(TFOpticKind kind);

        // data (lazy-loaded, then immutable)
        bool m_tableLoaded = false;
        std::unordered_map<std::string, OpticList> m_allowed; ///< by weapons.json key

        // session-local selection (by weapons.json key — future TFLoadout field)
        std::unordered_map<std::string, uint8_t> m_selected;

        // per-frame state
        std::string m_activeKey; ///< weapons.json key of the held weapon
        TFOpticKind m_optic = TFOpticKind::Irons;
        float m_blend = 0.0f;       ///< 0..1 ADS blend (drives zoom/overlay)
        double m_clock = 0.0;       ///< accumulated presentation seconds
        double m_toastUntil = -1.0; ///< sight-cycle toast fade deadline
        std::string m_toastText;
    };

} // namespace Terrafront
