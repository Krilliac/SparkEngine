/**
 * @file TFOutfitPanel.h
 * @brief Outfit (clan) UI: create form, roster with ranks, invite-by-name,
 *        leave/disband — a standalone ImGui window that the chat-social lane
 *        can also embed as a tab.
 *
 * OWNERSHIP: outfits lane (with Game/TFOutfitSystem.h/.cpp and
 * Persistence/TFOutfitStore.h/.cpp).
 *
 * Embedding contract for the social panel (coordinate via wave wiringNotes;
 * this lane does not edit the social/chat files):
 *   - RenderUI()       — the standalone window path (Main.cpp OnImGui, gated
 *                        on ctx.InWorld(); toggled by the `tf_outfit` console
 *                        command or SetOpen/Toggle).
 *   - RenderContents() — window-less body (create form / invite box / roster /
 *                        status line). Call it inside your own tab item; it
 *                        never calls ImGui::Begin/End. When embedding, call
 *                        SetOpen(false) once so the standalone window and the
 *                        tab never render twice.
 *
 * All mutations go through TFOutfitSystem's ClientRequest* entry points, so
 * the panel behaves identically on standalone/listen-host (loopback) and pure
 * clients (socket) — it renders the mirror, never server state directly.
 */
#pragma once

#include "Core/TFTypes.h"
#include "Core/TFEvents.h"

namespace Terrafront
{

    class TFOutfitSystem;

    class TFOutfitPanel
    {
      public:
        TFOutfitPanel();
        ~TFOutfitPanel();

        /// `outfits` is the module-owned TFOutfitSystem (Main.cpp wiring passes
        /// *m_outfits). Extra parameter vs. the uniform two-arg lifecycle because
        /// TFGameContext (contended TFTypes.h) may not expose `outfits` until the
        /// integrator applies the context wiring — this keeps the lane standalone.
        bool Initialize(TFGameContext& ctx, TFEventBus& events, TFOutfitSystem& outfits);
        void Update(float deltaTime);
        void FixedUpdate(float fixedDeltaTime);
        void Shutdown();
        void RenderDebugUI();

        /// Standalone window (no-op unless open + local player + in world).
        void RenderUI();

        /// Window-less body for the social-panel tab (see the header comment).
        void RenderContents();

        bool IsOpen() const { return m_open; }
        void SetOpen(bool open) { m_open = open; }
        void Toggle() { m_open = !m_open; }

      private:
#ifdef SPARK_HAS_IMGUI
        void DrawStatusLine();
        void DrawInviteBox();
        void DrawCreateForm();
        void DrawRoster();
#endif

        TFGameContext* m_ctx{nullptr};
        TFEventBus* m_events{nullptr};
        TFOutfitSystem* m_outfits{nullptr};
        bool m_initialized{false};
        bool m_open{false};
        bool m_cmdRegistered{false};

        char m_nameBuf[25]{};   // kTFOutfitNameMax + NUL
        char m_tagBuf[6]{};     // kTFOutfitTagMax + NUL
        char m_inviteBuf[24]{}; // character-name max (TF_CharBrief) + NUL
        bool m_confirmLeave{false};
        bool m_confirmDisband{false};
    };

} // namespace Terrafront
