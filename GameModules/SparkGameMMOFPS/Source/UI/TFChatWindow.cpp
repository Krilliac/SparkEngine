/**
 * @file TFChatWindow.cpp
 * @brief Client chat window implementation (see TFChatWindow.h).
 */
#include "UI/TFChatWindow.h"

#include "Game/TFOutfitSystem.h" // W8 ui-polish: /o channel membership pre-check
#include "Game/TFPlayerSystem.h"
#include "Game/TFSocialSystem.h"
#include "Net/TFChatRules.h"
#include "Net/TFClientNet.h"
#include "UI/TFKeybinds.h" // ui-map-keys seam: Action::FocusChat / CloseOverlay
#include "UI/TFUiCommon.h"

#include "Input/InputManager.h"
#include "Spark/IEngineContext.h"
#include "Utils/LogMacros.h"

#ifdef SPARK_HAS_IMGUI
#include <imgui.h>
#endif

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <vector>

namespace Terrafront
{

    namespace
    {

        constexpr size_t kMaxVisibleLines = 10; // closed-state fade + open history height
        constexpr size_t kMaxNotices = 8;       // local system lines kept around
        constexpr float kNoticeVisibleSec = 8.0f;

        struct ChannelStyle
        {
            ChatChannel channel;
            const char* tab;
            const char* prefixA; // short slash alias
            const char* prefixB; // long slash alias
        };

        constexpr ChannelStyle kChannels[] = {
            {ChatChannel::Region, "Region", "/r", "/region"},
            {ChatChannel::Faction, "Faction", "/f", "/faction"},
            {ChatChannel::Squad, "Squad", "/s", "/squad"},
            {ChatChannel::Yell, "Yell", "/y", "/yell"},
            // W8 ui-polish: outfit channel (server routes by TFOutfitSystem
            // membership — see TFChatRules::ShouldReceiveChat sameOutfit).
            {ChatChannel::Outfit, "Outfit", "/o", "/outfit"},
        };

        /// Is the LOCAL player in an outfit? Authority resolves live from the
        /// outfit registry; pure clients read their own roster mirror. Client
        /// pre-check only — the server's OutfitIdOf routing is the real gate.
        bool LocalInOutfit(const TFGameContext& ctx)
        {
            if (!ctx.outfits)
                return false;
            if (!ctx.IsAuthority())
                return ctx.outfits->InOutfit();
            const char* tag = ctx.outfits->GetOutfitTag(ctx.localPlayer);
            return tag != nullptr && tag[0] != '\0';
        }

    } // namespace

    TFChatWindow::TFChatWindow() = default;
    TFChatWindow::~TFChatWindow()
    {
        if (m_initialized)
            Shutdown();
    }

    bool TFChatWindow::Initialize(TFGameContext& ctx, TFEventBus& events)
    {
        m_ctx = &ctx;
        m_events = &events;

        // Self-register with the client net layer so its UI-open input
        // suppression covers the chat window (no TFGameContext change needed
        // for this direction — TFClientNet is owned by this same lane).
        if (ctx.clientNet)
            ctx.clientNet->SetChatUI(this);

        m_initialized = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] TFChatWindow initialized");
        return true;
    }

    void TFChatWindow::Update(float deltaTime)
    {
        if (!m_initialized)
            return;
        m_clock += deltaTime;

        for (Notice& n : m_notices)
            n.visibleFor = std::max(0.0f, n.visibleFor - deltaTime);

        // Friend on/offline notices from the social mirror become chat lines.
        if (m_social)
        {
            for (std::string& line : m_social->DrainFriendNotices())
            {
                m_notices.push_back(Notice{std::move(line), kNoticeVisibleSec});
                while (m_notices.size() > kMaxNotices)
                    m_notices.pop_front();
            }
        }
    }

    void TFChatWindow::FixedUpdate(float fixedDeltaTime)
    {
        (void)fixedDeltaTime;
    }

    void TFChatWindow::Shutdown()
    {
        if (!m_initialized)
            return;
        m_notices.clear();
        m_open = false;
        m_initialized = false;
    }

    bool TFChatWindow::LocalPawnAlive() const
    {
        if (!m_ctx || !m_ctx->players || m_ctx->localPlayer == kInvalidPlayer)
            return false;
        PawnInfo pawn{};
        return m_ctx->players->GetPawnByPlayer(m_ctx->localPlayer, pawn) && pawn.alive;
    }

    void TFChatWindow::OpenChatInput()
    {
        if (m_open)
        {
            m_focusInput = true;
            return;
        }
        m_open = true;
        m_focusInput = true;
        if (m_ctx && m_ctx->engine && m_ctx->engine->GetInput())
        {
            InputManager* input = m_ctx->engine->GetInput();
            m_releasedMouse = input->IsMouseCaptured();
            if (m_releasedMouse)
                input->CaptureMouse(false);
        }
    }

    void TFChatWindow::Close()
    {
        m_open = false;
        m_focusInput = false;
        if (m_releasedMouse && m_ctx && m_ctx->engine && m_ctx->engine->GetInput() && LocalPawnAlive())
            m_ctx->engine->GetInput()->CaptureMouse(true);
        m_releasedMouse = false;
    }

    void TFChatWindow::PushFeedback(const std::string& text)
    {
        m_notices.push_back(Notice{text, kNoticeVisibleSec});
        while (m_notices.size() > kMaxNotices)
            m_notices.pop_front();
    }

    bool TFChatWindow::ParseChannelPrefix(std::string& text)
    {
        if (text.empty() || text[0] != '/')
            return true;

        const size_t space = text.find(' ');
        const std::string cmd = text.substr(0, space);
        auto lower = [](std::string s)
        {
            for (char& c : s)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        };
        const std::string cmdLower = lower(cmd);

        for (const ChannelStyle& ch : kChannels)
        {
            if (cmdLower == ch.prefixA || cmdLower == ch.prefixB)
            {
                m_channel = ch.channel; // sticky channel switch
                text = (space == std::string::npos) ? std::string{} : text.substr(space + 1);
                return true;
            }
        }
        if (cmdLower == "/w" || cmdLower == "/whisper" || cmdLower == "/tell")
        {
            PushFeedback("whisper is not supported yet");
            return false;
        }
        PushFeedback("unknown command: " + cmd);
        return false;
    }

    void TFChatWindow::Submit()
    {
        std::string text = m_input;
        if (!ParseChannelPrefix(text))
        {
            m_input[0] = '\0';
            m_focusInput = true;
            return;
        }
        if (text.empty())
        {
            m_focusInput = true; // bare channel switch: keep typing
            m_input[0] = '\0';
            return;
        }
        if (m_channel == ChatChannel::Outfit && m_ctx && !LocalInOutfit(*m_ctx))
        {
            // Client pre-check (server still routes by real membership); keep
            // the typed text so the player can switch channels and resend.
            PushFeedback("you are not in an outfit — /outfit chat needs one (see tf_outfit)");
            m_focusInput = true;
            return;
        }
        if (m_clock < m_nextSendAt)
        {
            PushFeedback("sending too fast — wait a moment");
            m_focusInput = true;
            return;
        }
        if (!m_ctx || !m_ctx->clientNet || !m_ctx->clientNet->SendChat(m_channel, text))
        {
            PushFeedback("message not sent");
            m_focusInput = true;
            return;
        }
        m_nextSendAt = m_clock + kTFChatSendCooldownSec;
        m_input[0] = '\0';
        Close();
    }

    void TFChatWindow::ResolveSenderLabel(PlayerId from, std::string& outName, FactionId& outFaction) const
    {
        outFaction = FactionId::None;
        if (m_social && m_social->NameOfPlayer(from, outName))
        {
            outFaction = m_social->RosterFactionOf(from);
            return;
        }
        if (from == kTFLocalHostPlayer)
        {
            outName = "HOST";
            return;
        }
        char label[16];
        std::snprintf(label, sizeof(label), "p%u", from);
        outName = label;
    }

#ifdef SPARK_HAS_IMGUI

    void TFChatWindow::RenderUI()
    {
        if (!m_initialized || !m_ctx || !m_ctx->HasLocalPlayer() || !m_ctx->clientNet)
            return;

        // The FocusChat-bound key opens the input line (default ENTER; the
        // ui-map-keys TFKeybinds seam supplies the binding — same guards
        // TFHUD's built-in chat used).
        const ImGuiKey focusKey = TFKeys::ImGuiKeyFor(TFKeys::Action::FocusChat);
        const ImGuiKey closeKey = TFKeys::ImGuiKeyFor(TFKeys::Action::CloseOverlay);
        if (!m_open && m_ctx->InWorld() && !ImGui::GetIO().WantTextInput && !ImGui::GetIO().WantCaptureKeyboard &&
            focusKey != ImGuiKey_None && ImGui::IsKeyPressed(focusKey, false))
            OpenChatInput();
        if (m_open && closeKey != ImGuiKey_None && ImGui::IsKeyPressed(closeKey, false))
            Close();

        // --- collect visible lines (chat history + local notices) --------------
        const auto& history = m_ctx->clientNet->ChatHistory();
        std::vector<const TFClientNet::ChatLine*> visible;
        visible.reserve(kMaxVisibleLines);
        for (auto it = history.rbegin(); it != history.rend() && visible.size() < kMaxVisibleLines; ++it)
        {
            if (!m_open && it->visibleFor <= 0.0f)
                continue;
            if (m_social && m_social->IsBlockedPlayer(it->from))
                continue; // block list: suppress their chat client-side
            visible.push_back(&*it);
        }
        std::vector<const Notice*> notices;
        for (const Notice& n : m_notices)
        {
            if (m_open || n.visibleFor > 0.0f)
                notices.push_back(&n);
        }
        if (visible.empty() && notices.empty() && !m_open)
            return;

        // --- window shell (bottom-left, TFHUD chat geometry) --------------------
        ImGuiViewport* vp = ImGui::GetMainViewport();
        const float margin = std::clamp(vp->Size.x * 0.03f, 4.0f, 16.0f);
        const float width = std::max(1.0f, std::min(560.0f, vp->Size.x - margin * 2.0f));
        const float lineH = ImGui::GetTextLineHeightWithSpacing();
        const bool compact = width < 340.0f;
        const float controlsH = m_open ? ImGui::GetFrameHeight() * (compact ? 3.0f : 2.0f) +
                                             ImGui::GetStyle().ItemSpacing.y * (compact ? 3.0f : 2.0f)
                                       : 0.0f;
        const float maxHeight = std::max(1.0f, vp->Size.y - margin * 2.0f);
        const size_t lineCount = std::max<size_t>(visible.size() + notices.size(), 1);
        const float desiredHeight =
            lineH * static_cast<float>(lineCount) + controlsH + ImGui::GetStyle().WindowPadding.y * 2.0f;
        const float height = std::min(maxHeight, std::max(56.0f, desiredHeight));

        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + margin, vp->Pos.y + vp->Size.y - height - margin));
        ImGui::SetNextWindowSize(ImVec2(width, height));
        ImGui::SetNextWindowBgAlpha(m_open ? 0.86f : 0.59f);
        if (m_focusInput)
            ImGui::SetNextWindowFocus();

        ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
        if (!m_open)
            flags |= ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing;
        if (!ImGui::Begin("##TFChatWindow", nullptr, flags))
        {
            ImGui::End();
            return;
        }

        // --- history -------------------------------------------------------------
        const float historyH = std::max(1.0f, ImGui::GetContentRegionAvail().y - controlsH);
        ImGui::BeginChild("##TFChatWindowHistory", ImVec2(0.0f, historyH), false,
                          m_open ? ImGuiWindowFlags_None
                                 : ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBackground);
        for (auto it = visible.rbegin(); it != visible.rend(); ++it)
        {
            const TFClientNet::ChatLine& line = **it;

            const ImU32 channelColor = line.channel == ChatChannel::Squad     ? IM_COL32(110, 225, 150, 255)
                                       : line.channel == ChatChannel::Faction ? TFUi::FactionCol(m_ctx->localFaction)
                                       : line.channel == ChatChannel::Yell    ? IM_COL32(255, 180, 90, 255)
                                       : line.channel == ChatChannel::Outfit  ? IM_COL32(205, 145, 255, 255)
                                                                              : IM_COL32(130, 205, 255, 255);
            char tag[16];
            std::snprintf(tag, sizeof(tag), "[%s] ", ChatChannelName(line.channel));
            ImGui::PushStyleColor(ImGuiCol_Text, channelColor);
            ImGui::TextUnformatted(tag);
            ImGui::PopStyleColor();

            std::string name;
            FactionId senderFaction = FactionId::None;
            ResolveSenderLabel(line.from, name, senderFaction);
            name += ": ";
            ImGui::SameLine(0.0f, 0.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, TFUi::FactionCol(senderFaction));
            ImGui::TextUnformatted(name.c_str());
            ImGui::PopStyleColor();

            ImGui::SameLine(0.0f, 0.0f);
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(line.text.data(), line.text.data() + line.text.size());
            ImGui::PopTextWrapPos();
        }
        for (const Notice* n : notices)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(200, 200, 140, 255));
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(n->text.c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        }
        if (!visible.empty() || !notices.empty())
            ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();

        if (!m_open)
        {
            ImGui::End();
            return;
        }

        // --- channel tabs ----------------------------------------------------------
        for (size_t i = 0; i < std::size(kChannels); ++i)
        {
            const ChannelStyle& ch = kChannels[i];
            const bool active = m_channel == ch.channel;
            if (i > 0 && !compact)
                ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, active ? IM_COL32(70, 95, 130, 255) : IM_COL32(40, 44, 52, 255));
            if (ImGui::SmallButton(ch.tab))
            {
                m_channel = ch.channel;
                m_focusInput = true;
            }
            ImGui::PopStyleColor();
        }

        // --- input line --------------------------------------------------------------
        ImGui::SetNextItemWidth(std::max(1.0f, ImGui::GetContentRegionAvail().x));
        const bool justFocused = m_focusInput;
        if (m_focusInput)
        {
            ImGui::SetKeyboardFocusHere();
            m_focusInput = false;
        }
        const bool submitted =
            ImGui::InputText("##TFChatWindowInput", m_input, sizeof(m_input), ImGuiInputTextFlags_EnterReturnsTrue);
        if (submitted && !justFocused)
            Submit();

        ImGui::End();
    }

#else // !SPARK_HAS_IMGUI

    void TFChatWindow::RenderUI() {}

#endif // SPARK_HAS_IMGUI

    void TFChatWindow::RenderDebugUI()
    {
#ifdef SPARK_HAS_IMGUI
        if (!ImGui::CollapsingHeader("TF Chat Window"))
            return;
        ImGui::Text("open: %d  channel: %s  notices: %zu", m_open ? 1 : 0, ChatChannelName(m_channel),
                    m_notices.size());
#endif
    }

} // namespace Terrafront
