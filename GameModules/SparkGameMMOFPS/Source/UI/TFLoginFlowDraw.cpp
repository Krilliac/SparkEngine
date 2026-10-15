/**
 * @file TFLoginFlowDraw.cpp
 * @brief TFLoginFlow rendering half: the full-viewport onboarding modal
 *        (RenderUI), login/register form with the W11 LAN SERVERS list,
 *        character select/create screens, entering-world splash, and the
 *        debug window.
 *
 * Split from TFLoginFlow.cpp per the repo file-size rule (TFHUDDraw.cpp
 * pattern — same class, feature-owned translation units). TFLoginFlow.cpp
 * keeps the lifecycle, state machine, reply sinks, and sends.
 *
 * Scale: every offset, size and font size is a logical (100 %) value times
 * TFUi::UiScale(), so the screens stay coherent at uiScale 1.0, 1.5 and 2.0.
 * The panel and the sign-in form come from UI/TFLoginLayout.h (headless-tested).
 */
#include "UI/TFLoginFlow.h"

#include "Account/TFCharacterSystem.h" // kTFMaxCharSlots
#include "Data/TFDataTables.h"
#include "Game/TFLanDiscovery.h" // kTFLanBeaconPort, TFLanServerEntry
#include "UI/TFLoginLayout.h"
#include "UI/TFUiCommon.h"
#include "Utils/SecureMemory.h"

#ifdef SPARK_HAS_IMGUI
#include <imgui.h>
#endif

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace Terrafront
{

#ifdef SPARK_HAS_IMGUI

    namespace
    {
        constexpr ImU32 kTitleInk = IM_COL32(235, 235, 235, 235);
        constexpr ImU32 kSubtitleInk = IM_COL32(170, 174, 180, 210);
        constexpr ImU32 kLabelInk = IM_COL32(150, 154, 160, 200);
        constexpr ImU32 kErrorInk = IM_COL32(235, 110, 105, 235);

        /// Field label in the HUD face at an explicit (already scaled) size, top-left at @p pos.
        void AddLabel(ImDrawList* dl, float fontSize, ImVec2 pos, const char* text)
        {
            dl->AddText(Spark::Ui::Sdk::HudFont(), fontSize, pos, kLabelInk, text);
        }
    } // namespace

    void TFLoginFlow::RenderUI()
    {
        if (!m_initialized || !m_ctx || m_state == TFFlowState::InWorld)
            return;

        ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->Pos);
        ImGui::SetNextWindowSize(vp->Size);
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
                                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
        if (!ImGui::Begin("##TFLoginFlow", nullptr, flags))
        {
            ImGui::End();
            return;
        }

        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y), IM_COL32(6, 8, 12, 235));

        const TFLoginLayout::PanelRect panel =
            TFLoginLayout::ComputePanel(vp->Pos.x, vp->Pos.y, vp->Size.x, vp->Size.y, TFUi::UiScale());
        const ImVec2 panelMin(panel.x, panel.y);
        const ImVec2 panelMax(panel.x + panel.w, panel.y + panel.h);
        const float rounding = TFUi::Px(6.0f);
        dl->AddRectFilled(panelMin, panelMax, IM_COL32(16, 19, 25, 235), rounding);
        dl->AddRect(panelMin, panelMax, IM_COL32(120, 124, 130, 150), rounding, 0, std::max(1.0f, TFUi::Px(2.0f)));

        switch (m_state)
        {
        case TFFlowState::Login:
        case TFFlowState::Register:
            RenderLoginScreen(panel.x, panel.y, panel.w, panel.h);
            break;
        case TFFlowState::CharSelect:
            RenderCharacterSelectScreen(panel.x, panel.y, panel.w, panel.h);
            break;
        case TFFlowState::CharCreate:
            RenderCharacterCreateScreen(panel.x, panel.y, panel.w, panel.h);
            break;
        case TFFlowState::EnteringWorld:
            RenderEnteringWorldScreen(panel.x, panel.y, panel.w, panel.h);
            break;
        case TFFlowState::InWorld:
            break;
        }

        ImGui::End();
    }

    void TFLoginFlow::RenderLoginScreen(float panelX, float panelY, float panelW, float panelH)
    {
        using namespace TFUi;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const bool registering = (m_state == TFFlowState::Register);
        const TFLoginLayout::SignInForm form =
            TFLoginLayout::ComputeSignInForm({panelX, panelY, panelW, panelH}, UiScale(), ImGui::GetFrameHeight());

        AddTextCentered(dl, form.titleSize, ImVec2(form.centerX, form.titleCenterY), kTitleInk,
                        registering ? "CREATE ACCOUNT" : "TERRAFRONT");
        AddTextCentered(dl, form.subtitleSize, ImVec2(form.centerX, form.subtitleCenterY), kSubtitleInk,
                        registering ? "Choose a callsign and passphrase." : "Sign in to deploy.");

        AddLabel(dl, form.labelSize, ImVec2(form.fieldX, form.callsignLabelY), "CALLSIGN");
        ImGui::SetCursorScreenPos(ImVec2(form.fieldX, form.callsignInputY));
        ImGui::SetNextItemWidth(form.fieldW);
        ImGui::InputText("##tf_user", m_username, sizeof(m_username));

        AddLabel(dl, form.labelSize, ImVec2(form.fieldX, form.passphraseLabelY), "PASSPHRASE");
        ImGui::SetCursorScreenPos(ImVec2(form.fieldX, form.passphraseInputY));
        ImGui::SetNextItemWidth(form.fieldW);
        ImGui::InputText("##tf_pass", m_password, sizeof(m_password), ImGuiInputTextFlags_Password);

        if (!m_error.empty())
            AddTextCentered(dl, form.errorSize, ImVec2(form.centerX, form.errorCenterY), kErrorInk, m_error.c_str());

        const bool blocked = m_pending != PendingOp::None;
        const ImVec2 buttonSize(form.buttonW, form.buttonH);

        ImGui::SetCursorScreenPos(ImVec2(form.fieldX, form.buttonY));
        ImGui::BeginDisabled(blocked);
        if (ImGui::Button(registering ? "Register##tf_submit" : "Login##tf_submit", buttonSize))
        {
            if (registering)
                SendRegister();
            else
                SendLogin();
        }
        ImGui::EndDisabled();

        ImGui::SameLine(0.0f, form.buttonGap);
        ImGui::BeginDisabled(blocked);
        if (ImGui::Button(registering ? "Back to Login##tf_toggle" : "Create Account##tf_toggle", buttonSize))
        {
            m_state = registering ? TFFlowState::Login : TFFlowState::Register;
            m_error.clear();
        }
        ImGui::EndDisabled();

        // --- W11 server-browser lane: LAN SERVERS list between the form and the
        // footer buttons. Arms the scanner (idempotent; latched off on bind fail).
        // A panel too short for the list (high UI scale in a small window) hides
        // the whole section rather than drawing it over the buttons.
        if (form.showLan)
        {
            AddLabel(dl, form.labelSize, ImVec2(form.fieldX, form.lanHeaderY), "LAN SERVERS");
            RenderLanServerList(form.fieldX, form.lanListY, form.fieldW, form.lanListH, blocked);
        }
    }

    void TFLoginFlow::RenderLanServerList(float x, float y, float w, float h, bool blocked)
    {
        using namespace TFUi;
        if (h < Px(TFLoginLayout::Metric::kMinListHeight))
            return;

        m_lan.StartScanning();

        ImGui::SetCursorScreenPos(ImVec2(x, y));
        ImGui::BeginChild("##tf_lanlist", ImVec2(w, h), ImGuiChildFlags_Borders);

        const std::vector<TFLanServerEntry>& servers = m_lan.Servers();
        if (!m_lan.IsScanAvailable())
        {
            ImGui::TextDisabled("LAN discovery unavailable (UDP %u could not be opened - see log).",
                                static_cast<unsigned>(kTFLanBeaconPort));
        }
        else if (servers.empty())
        {
            ImGui::TextDisabled("Scanning for LAN servers on UDP %u...", static_cast<unsigned>(kTFLanBeaconPort));
        }
        else
        {
            // Join only makes sense before any net boot; once connected (or
            // hosting) the button stays visible but disabled.
            const bool canJoin = (m_ctx->role == NetRole::Standalone) && !blocked;
            int row = 0;
            for (const TFLanServerEntry& s : servers)
            {
                ImGui::PushID(row++);
                ImGui::BeginDisabled(!canJoin);
                if (ImGui::Button("Join", ImVec2(Px(56.0f), 0.0f)))
                    JoinLanServer(s.ip, s.gamePort);
                ImGui::EndDisabled();
                ImGui::SameLine(0.0f, Px(10.0f));
                char line[160];
                std::snprintf(line, sizeof(line), "%s   %u/%u   %s   %s:%u", s.name.c_str(),
                              static_cast<unsigned>(s.players), static_cast<unsigned>(s.maxPlayers), s.map.c_str(),
                              s.ip.c_str(), static_cast<unsigned>(s.gamePort));
                ImGui::TextUnformatted(line);
                ImGui::PopID();
            }
        }

        ImGui::EndChild();
    }

    void TFLoginFlow::RenderCharacterSelectScreen(float panelX, float panelY, float panelW, float panelH)
    {
        using namespace TFUi;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float centerX = panelX + panelW * 0.5f;

        AddTextCentered(dl, Px(26.0f), ImVec2(centerX, panelY + Px(38.0f)), kTitleInk, "SELECT CHARACTER");

        const float pad = Px(20.0f);
        const float footY = panelY + panelH - Px(64.0f);
        const float errorCenterY = footY - Px(22.0f);
        const float listTop = panelY + Px(76.0f);
        const float listBottom = errorCenterY - Px(18.0f);

        // The slots scroll inside their own region so a high UI scale never pushes them under the footer.
        ImGui::SetCursorScreenPos(ImVec2(panelX + pad, listTop));
        ImGui::BeginChild("##tf_chars", ImVec2(panelW - pad * 2.0f, std::max(Px(52.0f), listBottom - listTop)),
                          ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
        ImDrawList* listDl = ImGui::GetWindowDrawList();
        const float lineHeight = ImGui::GetTextLineHeight();
        const float rowH = std::max(Px(52.0f), lineHeight * 2.0f + Px(12.0f));
        for (size_t i = 0; i < m_chars.size(); ++i)
        {
            const TF_CharBrief& c = m_chars[i];
            const FactionId f = static_cast<FactionId>(c.faction);
            const bool selected = (m_selectedIdx == static_cast<int>(i));
            const ImVec2 rowPos = ImGui::GetCursorScreenPos();

            char label[96];
            // Bounded read: the name array is fixed-width (validated at receipt too).
            std::snprintf(label, sizeof(label), "%.*s##char%zu", static_cast<int>(strnlen(c.name, sizeof(c.name))),
                          c.name, i);
            if (ImGui::Selectable(label, selected, 0, ImVec2(ImGui::GetContentRegionAvail().x, rowH)))
                m_selectedIdx = static_cast<int>(i);

            char tag[64];
            std::snprintf(tag, sizeof(tag), "%s   -   Rank %u", FactionTag(f), static_cast<unsigned>(c.rank));
            listDl->AddText(ImVec2(rowPos.x + Px(8.0f), rowPos.y + lineHeight + Px(6.0f)), FactionCol(f, 0.9f), tag);
        }
        if (m_chars.empty())
        {
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            AddTextCentered(listDl, Px(15.0f), ImVec2(centerX, origin.y + Px(24.0f)), kSubtitleInk,
                            "No characters yet. Create one to deploy.");
        }
        ImGui::EndChild();

        if (!m_error.empty())
            AddTextCentered(dl, Px(14.0f), ImVec2(centerX, errorCenterY), kErrorInk, m_error.c_str());

        const bool hasSel = m_selectedIdx >= 0 && m_selectedIdx < static_cast<int>(m_chars.size());
        const bool blocked = m_pending != PendingOp::None;
        const float buttonH = Px(40.0f);
        const float gap = Px(12.0f);

        ImGui::SetCursorScreenPos(ImVec2(panelX + pad, footY));
        ImGui::BeginDisabled(blocked || !hasSel);
        if (ImGui::Button("Enter World##tf_enter", ImVec2(Px(140.0f), buttonH)))
            SendEnterWorld(m_chars[static_cast<size_t>(m_selectedIdx)].id);
        ImGui::EndDisabled();

        ImGui::SameLine(0.0f, gap);
        ImGui::BeginDisabled(blocked || m_chars.size() >= static_cast<size_t>(kTFMaxCharSlots));
        if (ImGui::Button("Create New##tf_createbtn", ImVec2(Px(120.0f), buttonH)))
        {
            std::memset(m_createName, 0, sizeof(m_createName));
            m_createFaction = FactionId::MRA;
            m_error.clear();
            m_state = TFFlowState::CharCreate;
        }
        ImGui::EndDisabled();

        ImGui::SameLine(0.0f, gap);
        ImGui::BeginDisabled(blocked || !hasSel);
        if (ImGui::Button("Delete##tf_delete", ImVec2(Px(90.0f), buttonH)))
            SendCharDelete(m_chars[static_cast<size_t>(m_selectedIdx)].id);
        ImGui::EndDisabled();

        const float logoutW = Px(80.0f);
        ImGui::SetCursorScreenPos(ImVec2(panelX + panelW - pad - logoutW, footY));
        if (ImGui::Button("Logout##tf_logout", ImVec2(logoutW, buttonH)))
            Logout();
    }

    void TFLoginFlow::RenderCharacterCreateScreen(float panelX, float panelY, float panelW, float panelH)
    {
        using namespace TFUi;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float centerX = panelX + panelW * 0.5f;

        AddTextCentered(dl, Px(26.0f), ImVec2(centerX, panelY + Px(38.0f)), kTitleInk, "NEW CHARACTER");

        // NAME label over its input, then the faction cards, laid out top-down.
        const float fieldW = panelW - Px(120.0f);
        const float fieldX = panelX + Px(60.0f);
        const float labelSize = Px(TFLoginLayout::Metric::kLabelSize);
        float y = panelY + Px(70.0f);
        AddLabel(dl, labelSize, ImVec2(fieldX, y), "NAME");
        y += labelSize + Px(TFLoginLayout::Metric::kLabelGap);
        ImGui::SetCursorScreenPos(ImVec2(fieldX, y));
        ImGui::SetNextItemWidth(fieldW);
        ImGui::InputText("##tf_charname", m_createName, sizeof(m_createName));
        y += ImGui::GetFrameHeight() + Px(20.0f);

        const float footY = panelY + panelH - Px(64.0f);
        const float errorCenterY = footY - Px(22.0f);
        const float pad = Px(16.0f);
        const float cardW = (panelW - pad * 4.0f) / 3.0f;
        const float cardsY = y;
        const float cardH = std::clamp(errorCenterY - Px(18.0f) - cardsY, 0.0f, Px(150.0f));
        // The faction button holds two lines (name, tag); the blurb gets what is left of the card.
        const ImGuiStyle& style = ImGui::GetStyle();
        const float twoLineButtonH = ImGui::GetTextLineHeight() * 2.0f + style.FramePadding.y * 2.0f;
        const float buttonH = std::min(cardH, std::max(cardH * 0.5f, twoLineButtonH));
        const float blurbGap = Px(8.0f);
        const float blurbH = cardH - buttonH - blurbGap;
        int i = 0;
        for (FactionId f : {FactionId::MRA, FactionId::AUC, FactionId::HLX})
        {
            const float x = panelX + pad + static_cast<float>(i) * (cardW + pad);
            ImGui::SetCursorScreenPos(ImVec2(x, cardsY));

            float c[4];
            FactionColor(f, c);
            const bool selected = (m_createFaction == f);
            const float shade = selected ? 0.85f : 0.45f;
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(c[0] * shade, c[1] * shade, c[2] * shade, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(c[0] * 0.75f, c[1] * 0.75f, c[2] * 0.75f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(c[0], c[1], c[2], 1.0f));

            char label[96];
            std::snprintf(label, sizeof(label), "%s\n[%s]##facc%d", FactionName(f), FactionTag(f), i);
            if (ImGui::Button(label, ImVec2(cardW, buttonH)))
                m_createFaction = f;
            ImGui::PopStyleColor(3);

            // The blurb wraps and clips inside the card, so it never runs into the footer.
            if (blurbH >= ImGui::GetTextLineHeight())
            {
                const FactionDef* fd = m_ctx->data ? m_ctx->data->GetFaction(f) : nullptr;
                ImGui::SetCursorScreenPos(ImVec2(x + Px(4.0f), cardsY + buttonH + blurbGap));
                ImGui::PushID(i);
                ImGui::BeginChild("##tf_factionblurb", ImVec2(cardW - Px(8.0f), blurbH), ImGuiChildFlags_None,
                                  ImGuiWindowFlags_NoBackground);
                ImGui::TextWrapped("%s", fd && !fd->blurb.empty() ? fd->blurb.c_str() : "");
                ImGui::EndChild();
                ImGui::PopID();
            }
            ++i;
        }

        if (!m_error.empty())
            AddTextCentered(dl, Px(14.0f), ImVec2(centerX, errorCenterY), kErrorInk, m_error.c_str());

        const bool blocked = m_pending != PendingOp::None;
        const float buttonRowH = Px(40.0f);

        ImGui::SetCursorScreenPos(ImVec2(fieldX, footY));
        ImGui::BeginDisabled(blocked);
        if (ImGui::Button("Create##tf_createsubmit", ImVec2(Px(140.0f), buttonRowH)))
            SendCharCreate();
        ImGui::EndDisabled();

        ImGui::SameLine(0.0f, Px(16.0f));
        ImGui::BeginDisabled(blocked);
        if (ImGui::Button("Back##tf_createback", ImVec2(Px(100.0f), buttonRowH)))
        {
            m_error.clear();
            m_state = TFFlowState::CharSelect;
        }
        ImGui::EndDisabled();
    }

    void TFLoginFlow::RenderEnteringWorldScreen(float panelX, float panelY, float panelW, float panelH)
    {
        using namespace TFUi;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 center(panelX + panelW * 0.5f, panelY + panelH * 0.45f);

        AddTextCentered(dl, Px(24.0f), center, kTitleInk, "Entering the Cindral Wastes...");

        char dots[8];
        const int n = 1 + (static_cast<int>(m_enterTimer * 2.0f) % 3);
        std::snprintf(dots, sizeof(dots), "%.*s", n, "...");
        AddTextCentered(dl, Px(16.0f), ImVec2(center.x, center.y + Px(34.0f)), kSubtitleInk, dots);
    }

    void TFLoginFlow::RenderDebugUI()
    {
        if (!m_showDebug)
            return;
        if (ImGui::Begin("TF Login Flow", &m_showDebug))
        {
            static const char* kStateNames[] = {"Login",      "Register",      "CharSelect",
                                                "CharCreate", "EnteringWorld", "InWorld"};
            ImGui::Text("state   : %s", kStateNames[static_cast<int>(m_state)]);
            ImGui::Text("account : %llu", static_cast<unsigned long long>(m_accountId));
            ImGui::Text("chars   : %zu", m_chars.size());
            ImGui::Text("pending : %s", m_pending == PendingOp::None ? "-" : "awaiting reply");
            if (!m_error.empty())
                ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.4f, 1.0f), "error   : %s", m_error.c_str());
            // W11 server-browser lane.
            ImGui::Text("lan     : beacon %s  scan %s  servers %zu", m_lan.IsBeaconActive() ? "on" : "off",
                        m_lan.IsScanning() ? "on" : (m_lan.IsScanAvailable() ? "off" : "unavailable"),
                        m_lan.Servers().size());
        }
        ImGui::End();
    }

#else // !SPARK_HAS_IMGUI — headless: screen is state-only

    void TFLoginFlow::RenderUI() {}
    void TFLoginFlow::RenderLoginScreen(float, float, float, float) {}
    void TFLoginFlow::RenderLanServerList(float, float, float, float, bool) {} // W11: scanner never arms headless
    void TFLoginFlow::RenderCharacterSelectScreen(float, float, float, float) {}
    void TFLoginFlow::RenderCharacterCreateScreen(float, float, float, float) {}
    void TFLoginFlow::RenderEnteringWorldScreen(float, float, float, float) {}
    void TFLoginFlow::RenderDebugUI() {}

#endif // SPARK_HAS_IMGUI

} // namespace Terrafront
