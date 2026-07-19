/**
 * @file TFViewModelInternal.h
 * @brief Shared internals for the TFViewModel*.cpp split parts: the viewmodel
 *        feel-tuning constants (recoil intake in TFViewModel.cpp, spring
 *        recovery + draw in TFViewModelRender.cpp) and the per-slot charm /
 *        sling-strap dressing helpers. Include only from the TFViewModel
 *        translation units.
 */
#pragma once

#include "Game/TFSecondaryMotion.h" // TFPendulumParams

#include <string>

namespace Terrafront
{
    namespace ViewModelDetail
    {

        // ---------------------------------------------------------------- feel tuning
        // All amplitudes deliberately small — the viewmodel should breathe, not swim.

        inline constexpr float kMaxFrameDtSec = 0.05f; ///< clamp render dt (alt-tab / hitches)

        // Recoil kick per weapons.json recoil unit (Cyclone-9 recoilVert 0.55 ->
        // ~0.9 deg pitch + ~9 mm slide per shot; Longtooth 2.4 -> capped heavy thump).
        inline constexpr float kRecoilPitchPerVert = 0.028f; ///< rad muzzle-up per recoilVert
        inline constexpr float kRecoilYawPerHoriz = 0.012f;  ///< rad, random sign per shot
        inline constexpr float kRecoilBackPerVert = 0.016f;  ///< m slide toward the eye
        inline constexpr float kRecoilMinVert = 0.20f;       ///< floor so 0-recoil defs still tick
        inline constexpr float kRecoilPitchCapRad = 0.20f;
        inline constexpr float kRecoilYawCapRad = 0.10f;
        inline constexpr float kRecoilBackCapM = 0.085f;
        inline constexpr float kRecoilPitchSpringK = 260.0f; ///< 1/s^2 (critically damped)
        inline constexpr float kRecoilBackSpringK = 340.0f;
        inline constexpr float kDryFireDipRad = 0.014f; ///< empty-mag click-twitch (muzzle dip)

        // View-turn lag (weapon trails the camera slightly).
        inline constexpr float kSwayPerYawRate = 0.010f; ///< m of lateral lag per rad/s of turn
        inline constexpr float kSwayPerPitchRate = 0.008f;
        inline constexpr float kSwayMaxM = 0.045f;
        inline constexpr float kSwaySpringK = 90.0f;

        // Idle breathing.
        inline constexpr float kIdleAmpM = 0.0035f;
        inline constexpr float kIdleHz = 0.45f;

        // Walk bob, driven by predicted horizontal speed.
        inline constexpr float kBobPhasePerMeter = 4.4f; ///< rad of stride phase per meter walked
        inline constexpr float kBobVertAmpM = 0.012f;
        inline constexpr float kBobLatAmpM = 0.009f;
        inline constexpr float kBobRefSpeed = 5.2f; ///< amplitude reference (base run speed)

        // Sprint lower (blend 0..1 spring; applied as drop + tilt-down).
        inline constexpr float kSprintLowerM = 0.10f;
        inline constexpr float kSprintLowerPitchRad = 0.30f;
        inline constexpr float kLowerSpringK = 55.0f;

        // Secondary motion: dangling charm + sling-strap hint (TFSecondaryMotion
        // world-space chains anchored through the animated grip frame) plus a
        // transient barrel flex. Amplitudes deliberately small — jewelry, not rope.
        inline constexpr float kBarrelFlexPerVert = 0.010f; ///< rad extra muzzle flex per recoilVert
        inline constexpr float kBarrelFlexCapRad = 0.035f;
        inline constexpr float kBarrelFlexSpringK = 700.0f; ///< snappier than the recoil spring
        inline constexpr float kCharmGravityMps2 = 6.5f;    ///< sub-g: lazier, more readable swing
        inline constexpr float kCharmDampingPerSec = 2.8f;
        inline constexpr float kStrapLinkLenM = 0.05f;
        inline constexpr int kStrapLinks = 3;
        inline constexpr float kStrapDampingPerSec = 4.5f;                 ///< webbing swings heavier than chain
        inline constexpr float kStrapAnchor[3] = {-0.01f, -0.06f, -0.03f}; ///< grip space, under stock
        inline constexpr float kCharmFireBackMps = 0.55f; ///< charm impulse per recoilVert (camera-back)
        inline constexpr float kCharmFireUpMps = 0.30f;   ///< ... and camera-up
        inline constexpr float kCharmFireKickCap = 3.0f;  ///< accumulated recoilVert cap per frame
        inline constexpr float kStrapImpulseScale = 0.5f; ///< strap reacts softer than the charm
        inline constexpr float kJumpVelDeltaMps = 3.0f;   ///< +vel step that reads as a jump
        inline constexpr float kJumpMinUpMps = 1.5f;
        inline constexpr float kLandMinFallMps = 3.0f;   ///< must fall faster than this to "land"
        inline constexpr float kLandRefFallMps = 9.0f;   ///< fall speed mapping to full land kick
        inline constexpr float kLandSwayDipMps = 0.055f; ///< m/s kick into the vertical sway spring
        inline constexpr float kJumpSwayRiseMps = 0.030f;
        inline constexpr float kCharmJumpKickMps = 0.35f; ///< world-down charm impulse on jump
        inline constexpr float kCharmLandKickMps = 0.80f; ///< world-down charm impulse, full impact
        inline constexpr float kCharmLinkThickM = 0.006f;
        inline constexpr float kStrapLinkThickM = 0.013f;
        inline constexpr float kCharmLinkColor[4] = {0.36f, 0.37f, 0.41f, 1.0f}; ///< chain metal
        inline constexpr float kStrapColor[4] = {0.11f, 0.11f, 0.13f, 1.0f};     ///< dark webbing
        inline constexpr float kCharmFobBase[3] = {0.45f, 0.45f, 0.48f};
        inline constexpr float kCharmFobTintK = 0.75f; ///< faction color strength on the fob

        // Muzzle flash: camera-facing quad running the 4-frame horizontal
        // flipbook strips shipped in P0 (Textures/MMOFPS/fx/muzzle_flash_*.png,
        // frames selected via UVTiling (0.25,1) + UVTiling.zw offset). The
        // world-space flash/tracer from TFWorldSetup::SpawnMuzzleFx stays the
        // source of truth for other players.
        inline constexpr float kFlashLifeSec = 0.045f;
        inline constexpr int kFlashFrames = 4;
        inline constexpr float kFlashQuadSizeM = 0.28f; ///< billboard edge (sheet frames are square)

        // Arm palette. Sleeves take the faction hue (same moderated-lerp scheme as the
        // ECS pawn tint) so your own arms match your team color; gloves stay dark.
        inline constexpr float kSleeveGrey[3] = {0.34f, 0.35f, 0.38f};
        inline constexpr float kSleeveTintK = 0.55f;
        inline constexpr float kGloveColor[4] = {0.15f, 0.15f, 0.17f, 1.0f};

        // Arm joints, view-space meters RELATIVE to the weapon anchor (ViewmodelDef
        // place, i.e. the right-hand grip). Shoulders sit off the bottom corners of
        // the screen so the arms read as coming from the player's body.
        inline constexpr float kRightShoulder[3] = {0.26f, -0.42f, -0.48f};
        inline constexpr float kRightElbow[3] = {0.17f, -0.24f, -0.30f};
        inline constexpr float kRightHand[3] = {0.015f, -0.02f, -0.05f};
        inline constexpr float kLeftShoulder[3] = {-0.28f, -0.44f, -0.44f};
        inline constexpr float kLeftElbow[3] = {-0.20f, -0.26f, -0.22f};
        inline constexpr float kUpperArmThickM = 0.075f;
        inline constexpr float kForearmThickM = 0.062f;
        inline constexpr float kHandScaleM[3] = {0.075f, 0.055f, 0.10f};

        inline constexpr float kTwoPi = 6.28318531f;

        /// Per-slot charm dressing: where the trinket hangs off the weapon (grip
        /// space, right side so it stays clear of the support hand), chain length,
        /// and fob proportions. Weapon slot picks the silhouette, faction picks
        /// the fob color — both from existing defs, no new data files.
        struct CharmStyle
        {
            float anchor[3];
            int links;
            float linkLenM;
            float fobScale[3];
        };

        inline CharmStyle CharmStyleForSlot(const std::string& slot)
        {
            // default (rifle/carbine/lmg/shotgun): dogtag on a 3-link chain
            CharmStyle s{{0.035f, -0.045f, 0.16f}, 3, 0.026f, {0.020f, 0.030f, 0.006f}};
            if (slot == "pistol")
            {
                s = CharmStyle{{0.022f, -0.035f, 0.05f}, 2, 0.022f, {0.016f, 0.016f, 0.016f}}; // compact cube fob
            }
            else if (slot == "sniper")
            {
                s = CharmStyle{{0.030f, -0.045f, 0.24f}, 3, 0.032f, {0.012f, 0.038f, 0.012f}}; // long scope tassel
            }
            else if (slot == "launcher")
            {
                s = CharmStyle{{0.035f, -0.060f, 0.12f}, 2, 0.030f, {0.026f, 0.020f, 0.014f}}; // stubby tag
            }
            else if (slot == "melee" || slot == "tool")
            {
                s = CharmStyle{{0.0f, -0.040f, 0.06f}, 2, 0.020f, {0.014f, 0.020f, 0.005f}}; // small lanyard
            }
            return s;
        }

        inline TFPendulumParams CharmParams(const CharmStyle& style)
        {
            TFPendulumParams p;
            p.linkCount = style.links;
            p.linkLengthM = style.linkLenM;
            p.gravityMps2 = kCharmGravityMps2;
            p.dampingPerSec = kCharmDampingPerSec;
            return p;
        }

        inline TFPendulumParams StrapParams()
        {
            TFPendulumParams p;
            p.linkCount = kStrapLinks;
            p.linkLengthM = kStrapLinkLenM;
            p.dampingPerSec = kStrapDampingPerSec;
            return p;
        }

    } // namespace ViewModelDetail
} // namespace Terrafront
