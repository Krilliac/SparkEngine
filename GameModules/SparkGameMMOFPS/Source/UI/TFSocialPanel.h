/**
 * @file TFSocialPanel.h
 * @brief Social window: friends / recent players / block list / squad roster.
 *
 * OWNERSHIP: chat-social lane. Toggled with 'O' (guarded against text input,
 * same pattern as TFHUD's ENTER-chat) or the tf_social console command; the
 * ui-map-keys lane can also call Toggle() from its keybind dispatch.
 *
 * All state it renders lives elsewhere:
 *  - friends/blocked/recent + online roster: TFSocialSystem client mirror
 *    (server-authoritative, persisted per character server-side)
 *  - squad roster/invites: TFSquadSystem's client mirror via the additive
 *    GetLocalSquadView()/UiSendOp() surface (server stays authoritative)
 * The panel only issues ops (ClientSendOp / UiSendOp) and draws.
 */
#pragma once

#include "Core/TFTypes.h"
#include "Core/TFEvents.h"

#include <string>

namespace Terrafront
{

    class TFSocialSystem;

    class TFSocialPanel
    {
      public:
        TFSocialPanel();
        ~TFSocialPanel();

        bool Initialize(TFGameContext& ctx, TFEventBus& events);
        void Update(float deltaTime);
        void FixedUpdate(float fixedDeltaTime);
        void Shutdown();
        void RenderDebugUI();
        void RenderUI();

        /// Wired from Main.cpp after construction (see lane wiring notes).
        void SetSocial(TFSocialSystem* social) { m_social = social; }

        // --- keybind seam ------------------------------------------------------
        bool IsOpen() const { return m_open; }
        void Toggle();

      private:
        void DrawFriendsTab();
        void DrawRecentTab();
        void DrawBlockedTab();
        void DrawSquadTab();
        bool LocalPawnAlive() const;
        std::string RosterNameOf(PlayerId player) const;

        TFGameContext* m_ctx{nullptr};
        TFEventBus* m_events{nullptr};
        TFSocialSystem* m_social{nullptr};
        bool m_initialized{false};

        bool m_open{false};
        bool m_releasedMouse{false};
        bool m_socialCmd{false};
        char m_friendInput[24]{};
        char m_blockInput[24]{};
        char m_inviteInput[24]{};
    };

} // namespace Terrafront
