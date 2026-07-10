/**
 * @file TFChatWindow.h
 * @brief Client chat window: bottom-left overlay, channel tabs, slash-prefix
 *        commands, player-name resolution, block filtering, fading history.
 *
 * OWNERSHIP: chat-social lane. Supersedes TFHUD's minimal built-in chat —
 * the integrator gates TFHUD::DrawChat / its ENTER-open block on
 * `m_ctx->chatWindow == nullptr` (see the lane wiring notes) so exactly one
 * chat UI is live. Until that wiring lands this class compiles inert.
 *
 * Reuses the EXISTING chat pipeline end to end — no parallel protocol:
 *  - send:    TFClientNet::SendChat (normalization + TFMsg::ChatMsg)
 *  - receive: TFClientNet::ChatHistory (fed by OnChatMsg, TTL-decayed)
 *  - rules:   Net/TFChatRules.h (channels, 12 s visibility, history cap);
 *             client send cooldown mirrors TFServerSim::HandleChatMsg's 0.5 s.
 *
 * Whisper is intentionally absent: TF_ChatMsg carries no target player and
 * ShouldReceiveChat has no whisper route — adding one is a protocol change
 * that belongs to a future wave.
 *
 * Keybind seam (ui-map-keys lane): ENTER-to-open is handled internally with
 * the same guards TFHUD used; external keybind dispatch can call
 * OpenChatInput() / Close() instead — both are idempotent.
 */
#pragma once

#include "Core/TFTypes.h"
#include "Core/TFEvents.h"
#include "Net/TFNetProtocol.h"

#include <deque>
#include <string>

namespace Terrafront
{

    class TFSocialSystem;

    class TFChatWindow
    {
      public:
        TFChatWindow();
        ~TFChatWindow();

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
        void OpenChatInput(); ///< open + focus the input line (idempotent)
        void Close();         ///< close, restoring mouse capture if we released it

      private:
        struct Notice
        {
            std::string text;
            float visibleFor{0.0f};
        };

        /// Consume a leading channel prefix ("/f ", "/squad ", ...) from `text`;
        /// returns false with feedback set when the slash command is unknown.
        bool ParseChannelPrefix(std::string& text);
        void Submit();
        void PushFeedback(const std::string& text);
        bool LocalPawnAlive() const;
        void ResolveSenderLabel(PlayerId from, std::string& outName, FactionId& outFaction) const;

        TFGameContext* m_ctx{nullptr};
        TFEventBus* m_events{nullptr};
        TFSocialSystem* m_social{nullptr};
        bool m_initialized{false};

        bool m_open{false};
        bool m_focusInput{false};
        bool m_releasedMouse{false};
        ChatChannel m_channel{ChatChannel::Region};
        char m_input[122]{};

        double m_clock{0.0};
        double m_nextSendAt{0.0}; ///< client-side anti-spam (mirrors the server's 0.5 s)

        std::deque<Notice> m_notices; ///< local system lines (friend on/offline, feedback)
    };

    /// Client-side minimum interval between chat sends; mirrors the server
    /// limiter in TFServerSim::HandleChatMsg (m_chatNextAt + 0.5).
    constexpr double kTFChatSendCooldownSec = 0.5;

} // namespace Terrafront
