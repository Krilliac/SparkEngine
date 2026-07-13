/**
 * @file TFMinimap.h
 * @brief Corner minimap v3 (W13 minimap-v2 lane): owner-colored region hexes +
 *        conduit lattice, capture-point states (pulsing when contested), the
 *        local player arrow, squadmate diamonds, ping diamonds, the squad
 *        waypoint (edge-clamped), friendly vehicle icons, zoom levels with a
 *        range ring, and a continent-alert flash overlay.
 *
 * OWNERSHIP: minimap-v2 lane (this header + TFMinimap.cpp). TFHUD::DrawMinimap
 * (UI/TFHUDCombat.cpp, same lane) delegates here; the minimap's own state
 * (zoom level, label TTL) lives in TFMinimapState because TFHUD.h is contended
 * this wave and gains no new members.
 *
 * Orientation: NORTH-UP (world +Z = screen up), deliberately — it matches the
 * compass strip, the fullscreen map (W7) and the v2 minimap it replaces, so
 * marker positions stay put while the player turns (only the arrow rotates)
 * and no per-frame rotation of the hex geometry is needed.
 *
 * Alert feed seam: TFGameContext has no TFAlertSystem pointer, so Main.cpp
 * (wiringNotes) publishes it through the header-only inline pointer below
 * (TFKeybinds inline-table precedent — one instance module-wide; this header
 * is only included by SparkGameMMOFPS TUs, so the per-image-DLL-statics
 * gotcha does not bite). Unwired (null) the alert flash simply stays off —
 * every other layer keeps working.
 *
 * Determinism contract: pure presentation — reads replicated mirrors only,
 * never touches movement/collision/damage state.
 */
#pragma once

#include "Core/TFTypes.h"

namespace Terrafront
{

    class TFAlertSystem;

    // -----------------------------------------------------------------------
    // Cross-lane alert feed (Main.cpp wiring; null-safe when never wired)
    // -----------------------------------------------------------------------
    namespace TFMinimapFeed
    {
        namespace Detail
        {
            inline TFAlertSystem* g_alerts = nullptr;
        } // namespace Detail

        /// Main.cpp boot wiring: publish the alert system (pass nullptr again
        /// before TFAlertSystem::Shutdown to disarm the flash overlay).
        inline void SetAlertSystem(TFAlertSystem* alerts) { Detail::g_alerts = alerts; }
        inline TFAlertSystem* GetAlertSystem() { return Detail::g_alerts; }
    } // namespace TFMinimapFeed

    // -----------------------------------------------------------------------
    // Zoom levels (cycled with TFKeys::Action::MinimapZoom, default 'N')
    // -----------------------------------------------------------------------
    inline constexpr int kTFMinimapZoomCount = 3;
    /// World meters spanned edge-to-edge per zoom level. Index 1 (340 m) is
    /// the v2 default span, so the out-of-the-box view is unchanged.
    inline constexpr float kTFMinimapZoomSpanM[kTFMinimapZoomCount] = {180.0f, 340.0f, 680.0f};
    /// Range-ring world radius per zoom level (round numbers for reading).
    inline constexpr float kTFMinimapRingM[kTFMinimapZoomCount] = {50.0f, 100.0f, 200.0f};

    /// Minimap presentation state. TFHUD.h is contended this wave, so the one
    /// TFHUD instance keeps this as a function-local static in DrawMinimap.
    struct TFMinimapState
    {
        int zoomIdx{1};          ///< index into kTFMinimapZoomSpanM (1 == v2 span)
        float zoomLabelTtl{0.0f}; ///< seconds the "SPAN x m" readout stays up
    };

    /// Draw the corner minimap. Must be called between the HUD overlay
    /// window's Begin/End (ImGui frame active). `playerPos` is the local
    /// pawn's feet position (TFHUD::m_view.pos); `inputLocked` suppresses the
    /// zoom hotkey (chat open etc). No-op without SPARK_HAS_IMGUI.
    void TFMinimapDraw(TFGameContext& ctx, TFMinimapState& st, const float playerPos[3], bool inputLocked);

} // namespace Terrafront
