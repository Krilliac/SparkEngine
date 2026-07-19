/**
 * @file TFHUDChat.cpp
 * @brief TFHUD built-in chat: the input-receiving chat window (history,
 *        channel selector, input line) and the open/close mouse-capture
 *        handoff.
 *
 * Split from TFHUD.cpp per the repo file-size rule (TFPlayerSystemClient
 * pattern — same class, feature-owned translation units). Superseded at
 * runtime by TFChatWindow when the chat-social lane wires it in — the
 * m_ctx->chatWindow gate in RenderUI (TFHUDDraw.cpp) keeps this chat from
 * ever opening or rendering then.
 */
#include "UI/TFHUD.h"

#include "Net/TFClientNet.h"
#include "Net/TFChatRules.h"
#include "Net/TFNetProtocol.h"
#include "UI/TFHUDInternal.h"
#include "Input/InputManager.h"
#include "Spark/IEngineContext.h"

#ifdef SPARK_HAS_IMGUI
#include <imgui.h>
#endif

#include <algorithm>
#include <cstdio>
#include <vector>

namespace Terrafront
{

    using namespace HudDetail; // TFHUDInternal.h: FactionCol shared with TFHUDDraw.cpp

    void TFHUD::CloseChat()
    {
        m_chatOpen = false;
        m_focusChatInput = false;
        if (m_chatReleasedMouse && m_ctx && m_ctx->engine && m_ctx->engine->GetInput() && m_view.alive)
            m_ctx->engine->GetInput()->CaptureMouse(true);
        m_chatReleasedMouse = false;
    }

#ifdef SPARK_HAS_IMGUI

    void TFHUD::DrawChat()
    {
        if (!m_ctx || !m_ctx->clientNet)
            return;
        ImGuiViewport* vp = ImGui::GetMainViewport();
        const float margin = std::clamp(vp->Size.x * 0.03f, 4.0f, 16.0f);
        const float width = std::max(1.0f, std::min(560.0f, vp->Size.x - margin * 2.0f));
        const float lineH = ImGui::GetTextLineHeightWithSpacing();
        const bool compact = width < 300.0f;
        const float controlsH = m_chatOpen ? ImGui::GetFrameHeight() * (compact ? 2.0f : 1.0f) +
                                                 ImGui::GetStyle().ItemSpacing.y * (compact ? 2.0f : 1.0f)
                                           : 0.0f;
        const float maxHeight = std::max(1.0f, vp->Size.y - margin * 2.0f);

        const auto& history = m_ctx->clientNet->ChatHistory();
        std::vector<const TFClientNet::ChatLine*> visible;
        visible.reserve(8);
        for (auto it = history.rbegin(); it != history.rend() && visible.size() < 8; ++it)
        {
            if (m_chatOpen || it->visibleFor > 0.0f)
                visible.push_back(&*it);
        }
        if (visible.empty() && !m_chatOpen)
            return;

        const float desiredHeight = lineH * static_cast<float>(std::max<size_t>(visible.size(), 1)) + controlsH +
                                    ImGui::GetStyle().WindowPadding.y * 2.0f;
        const float height = std::min(maxHeight, std::max(52.0f, desiredHeight));
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + margin, vp->Pos.y + vp->Size.y - height - margin));
        ImGui::SetNextWindowSize(ImVec2(width, height));
        ImGui::SetNextWindowBgAlpha(m_chatOpen ? 0.86f : 0.59f);
        if (m_focusChatInput)
            ImGui::SetNextWindowFocus();

        ImGuiWindowFlags chatFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
                                     ImGuiWindowFlags_NoBringToFrontOnFocus;
        if (!m_chatOpen)
            chatFlags |= ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing;
        if (!ImGui::Begin("##TFChat", nullptr, chatFlags))
        {
            ImGui::End();
            return;
        }

        const float historyH = std::max(1.0f, ImGui::GetContentRegionAvail().y - controlsH);
        ImGui::BeginChild("##TFChatHistory", ImVec2(0.0f, historyH), false,
                          ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBackground);
        for (auto it = visible.rbegin(); it != visible.rend(); ++it)
        {
            const TFClientNet::ChatLine& line = **it;
            char prefix[48]{};
            std::snprintf(prefix, sizeof(prefix), "[%s] p%u: ", ChatChannelName(line.channel), line.from);
            const ImU32 channelColor = line.channel == ChatChannel::Squad     ? IM_COL32(110, 225, 150, 255)
                                       : line.channel == ChatChannel::Faction ? FactionCol(m_ctx->localFaction)
                                       : line.channel == ChatChannel::Yell    ? IM_COL32(255, 180, 90, 255)
                                                                              : IM_COL32(130, 205, 255, 255);
            ImGui::PushStyleColor(ImGuiCol_Text, channelColor);
            ImGui::TextUnformatted(prefix);
            ImGui::PopStyleColor();
            ImGui::SameLine(0.0f, 0.0f);
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(line.text.data(), line.text.data() + line.text.size());
            ImGui::PopTextWrapPos();
        }
        if (!visible.empty())
            ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();

        if (!m_chatOpen)
        {
            ImGui::End();
            return;
        }

        static const char* channels[] = {"Region", "Faction", "Squad", "Yell"};
        int selected = static_cast<int>(m_chatChannel);
        const float channelWidth = compact ? ImGui::GetContentRegionAvail().x : 92.0f;
        ImGui::SetNextItemWidth(std::max(1.0f, channelWidth));
        if (ImGui::Combo("##TFChatChannel", &selected, channels, 4))
            m_chatChannel = static_cast<ChatChannel>(selected);
        if (!compact)
            ImGui::SameLine();
        ImGui::SetNextItemWidth(std::max(1.0f, ImGui::GetContentRegionAvail().x));
        const bool justFocused = m_focusChatInput;
        if (m_focusChatInput)
        {
            ImGui::SetKeyboardFocusHere();
            m_focusChatInput = false;
        }
        const bool submitted =
            ImGui::InputText("##TFChatInput", m_chatInput, sizeof(m_chatInput), ImGuiInputTextFlags_EnterReturnsTrue);
        if (submitted && !justFocused)
        {
            if (m_ctx->clientNet->SendChat(m_chatChannel, m_chatInput))
            {
                m_chatInput[0] = '\0';
                CloseChat();
            }
            else
            {
                m_focusChatInput = true;
            }
        }
        ImGui::End();
    }

#else // !SPARK_HAS_IMGUI — headless / no ImGui: chat is state-only.

    void TFHUD::DrawChat() {}

#endif // SPARK_HAS_IMGUI

} // namespace Terrafront
