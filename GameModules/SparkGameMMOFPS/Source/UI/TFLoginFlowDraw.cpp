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
 */
#include "UI/TFLoginFlow.h"

#include "Account/TFCharacterSystem.h" // kTFMaxCharSlots
#include "Data/TFDataTables.h"
#include "Game/TFLanDiscovery.h" // kTFLanBeaconPort, TFLanServerEntry
#include "UI/TFUiCommon.h"

#ifdef SPARK_HAS_IMGUI
#include <imgui.h>
#endif

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace Terrafront
{

#ifdef SPARK_HAS_IMGUI

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

        const float panelW = std::min(600.0f, vp->Size.x * 0.92f);
        const float panelH = std::min(560.0f, vp->Size.y * 0.88f);
        const float panelX = vp->Pos.x + (vp->Size.x - panelW) * 0.5f;
        const float panelY = vp->Pos.y + (vp->Size.y - panelH) * 0.5f;
        dl->AddRectFilled(ImVec2(panelX, panelY), ImVec2(panelX + panelW, panelY + panelH), IM_COL32(16, 19, 25, 235),
                          6.0f);
        dl->AddRect(ImVec2(panelX, panelY), ImVec2(panelX + panelW, panelY + panelH), IM_COL32(120, 124, 130, 150),
                    6.0f, 0, 2.0f);

        switch (m_state)
        {
        case TFFlowState::Login:
        case TFFlowState::Register:
            RenderLoginScreen(panelX, panelY, panelW, panelH);
            break;
        case TFFlowState::CharSelect:
            RenderCharacterSelectScreen(panelX, panelY, panelW, panelH);
            break;
        case TFFlowState::CharCreate:
            RenderCharacterCreateScreen(panelX, panelY, panelW, panelH);
            break;
        case TFFlowState::EnteringWorld:
            RenderEnteringWorldScreen(panelX, panelY, panelW, panelH);
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

        AddTextCentered(dl, 28.0f, ImVec2(panelX + panelW * 0.5f, panelY + 44.0f), IM_COL32(235, 235, 235, 235),
                        registering ? "CREATE ACCOUNT" : "TERRAFRONT");
        AddTextCentered(dl, 14.0f, ImVec2(panelX + panelW * 0.5f, panelY + 74.0f), IM_COL32(170, 174, 180, 210),
                        registering ? "Choose a callsign and passphrase." : "Sign in to deploy.");

        const float fieldW = panelW - 120.0f;
        const float fieldX = panelX + 60.0f;

        dl->AddText(ImVec2(fieldX, panelY + 108.0f), IM_COL32(150, 154, 160, 200), "CALLSIGN");
        ImGui::SetCursorScreenPos(ImVec2(fieldX, panelY + 126.0f));
        ImGui::SetNextItemWidth(fieldW);
        ImGui::InputText("##tf_user", m_username, sizeof(m_username));

        dl->AddText(ImVec2(fieldX, panelY + 178.0f), IM_COL32(150, 154, 160, 200), "PASSPHRASE");
        ImGui::SetCursorScreenPos(ImVec2(fieldX, panelY + 196.0f));
        ImGui::SetNextItemWidth(fieldW);
        ImGui::InputText("##tf_pass", m_password, sizeof(m_password), ImGuiInputTextFlags_Password);

        if (!m_error.empty())
            AddTextCentered(dl, 15.0f, ImVec2(panelX + panelW * 0.5f, panelY + 244.0f), IM_COL32(235, 110, 105, 235),
                            m_error.c_str());

        const bool blocked = m_pending != PendingOp::None;
        const float btnY = panelY + panelH - 80.0f;
        const float btnW = (fieldW - 20.0f) * 0.5f;

        ImGui::SetCursorScreenPos(ImVec2(fieldX, btnY));
        ImGui::BeginDisabled(blocked);
        if (ImGui::Button(registering ? "Register##tf_submit" : "Login##tf_submit", ImVec2(btnW, 44.0f)))
        {
            if (registering)
                SendRegister();
            else
                SendLogin();
        }
        ImGui::EndDisabled();

        ImGui::SameLine(0.0f, 20.0f);
        ImGui::BeginDisabled(blocked);
        if (ImGui::Button(registering ? "Back to Login##tf_toggle" : "Create Account##tf_toggle", ImVec2(btnW, 44.0f)))
        {
            m_state = registering ? TFFlowState::Login : TFFlowState::Register;
            m_error.clear();
        }
        ImGui::EndDisabled();

        // --- W11 server-browser lane: LAN SERVERS list between the form and the
        // footer buttons. Arms the scanner (idempotent; latched off on bind fail).
        const float lanHeaderY = panelY + 272.0f;
        dl->AddText(ImVec2(fieldX, lanHeaderY), IM_COL32(150, 154, 160, 200), "LAN SERVERS");
        const float listY = lanHeaderY + 20.0f;
        RenderLanServerList(fieldX, listY, fieldW, (btnY - 12.0f) - listY, blocked);
    }

    void TFLoginFlow::RenderLanServerList(float x, float y, float w, float h, bool blocked)
    {
        if (h < 24.0f)
            return;

        m_lan.StartScanning();

        ImGui::SetCursorScreenPos(ImVec2(x, y));
        ImGui::BeginChild("##tf_lanlist", ImVec2(w, h), true);

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
                if (ImGui::Button("Join", ImVec2(56.0f, 0.0f)))
                    JoinLanServer(s.ip, s.gamePort);
                ImGui::EndDisabled();
                ImGui::SameLine(0.0f, 10.0f);
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

        AddTextCentered(dl, 26.0f, ImVec2(panelX + panelW * 0.5f, panelY + 38.0f), IM_COL32(235, 235, 235, 235),
                        "SELECT CHARACTER");

        const float pad = 20.0f;
        const float rowH = 52.0f;
        float y = panelY + 76.0f;

        for (size_t i = 0; i < m_chars.size(); ++i)
        {
            const TF_CharBrief& c = m_chars[i];
            const FactionId f = static_cast<FactionId>(c.faction);
            const bool selected = (m_selectedIdx == static_cast<int>(i));

            ImGui::SetCursorScreenPos(ImVec2(panelX + pad, y));
            char label[96];
            std::snprintf(label, sizeof(label), "%s##char%zu", c.name, i);
            if (ImGui::Selectable(label, selected, 0, ImVec2(panelW - pad * 2.0f, rowH)))
                m_selectedIdx = static_cast<int>(i);

            char tag[64];
            std::snprintf(tag, sizeof(tag), "%s   -   Rank %u", FactionTag(f), static_cast<unsigned>(c.rank));
            dl->AddText(ImVec2(panelX + pad + 8.0f, y + rowH * 0.5f - 8.0f), FactionCol(f, 0.9f), tag);

            y += rowH + 8.0f;
        }

        if (m_chars.empty())
            AddTextCentered(dl, 15.0f, ImVec2(panelX + panelW * 0.5f, y + 24.0f), IM_COL32(170, 174, 180, 210),
                            "No characters yet. Create one to deploy.");

        if (!m_error.empty())
            AddTextCentered(dl, 14.0f, ImVec2(panelX + panelW * 0.5f, panelY + panelH - 120.0f),
                            IM_COL32(235, 110, 105, 235), m_error.c_str());

        const bool hasSel = m_selectedIdx >= 0 && m_selectedIdx < static_cast<int>(m_chars.size());
        const bool blocked = m_pending != PendingOp::None;
        const float footY = panelY + panelH - 64.0f;
        const float pad2 = 20.0f;

        ImGui::SetCursorScreenPos(ImVec2(panelX + pad2, footY));
        ImGui::BeginDisabled(blocked || !hasSel);
        if (ImGui::Button("Enter World##tf_enter", ImVec2(140.0f, 40.0f)))
            SendEnterWorld(m_chars[static_cast<size_t>(m_selectedIdx)].id);
        ImGui::EndDisabled();

        ImGui::SameLine(0.0f, 12.0f);
        ImGui::BeginDisabled(blocked || m_chars.size() >= static_cast<size_t>(kTFMaxCharSlots));
        if (ImGui::Button("Create New##tf_createbtn", ImVec2(120.0f, 40.0f)))
        {
            std::memset(m_createName, 0, sizeof(m_createName));
            m_createFaction = FactionId::MRA;
            m_error.clear();
            m_state = TFFlowState::CharCreate;
        }
        ImGui::EndDisabled();

        ImGui::SameLine(0.0f, 12.0f);
        ImGui::BeginDisabled(blocked || !hasSel);
        if (ImGui::Button("Delete##tf_delete", ImVec2(90.0f, 40.0f)))
            SendCharDelete(m_chars[static_cast<size_t>(m_selectedIdx)].id);
        ImGui::EndDisabled();

        ImGui::SetCursorScreenPos(ImVec2(panelX + panelW - 100.0f, footY));
        if (ImGui::Button("Logout##tf_logout", ImVec2(80.0f, 40.0f)))
        {
            m_accountId = 0;
            m_chars.clear();
            m_selectedIdx = -1;
            std::memset(m_password, 0, sizeof(m_password));
            m_error.clear();
            m_pending = PendingOp::None;
            m_state = TFFlowState::Login;
        }
    }

    void TFLoginFlow::RenderCharacterCreateScreen(float panelX, float panelY, float panelW, float panelH)
    {
        using namespace TFUi;
        ImDrawList* dl = ImGui::GetWindowDrawList();

        AddTextCentered(dl, 26.0f, ImVec2(panelX + panelW * 0.5f, panelY + 38.0f), IM_COL32(235, 235, 235, 235),
                        "NEW CHARACTER");

        const float fieldW = panelW - 120.0f;
        const float fieldX = panelX + 60.0f;
        dl->AddText(ImVec2(fieldX, panelY + 82.0f), IM_COL32(150, 154, 160, 200), "NAME");
        ImGui::SetCursorScreenPos(ImVec2(fieldX, panelY + 100.0f));
        ImGui::SetNextItemWidth(fieldW);
        ImGui::InputText("##tf_charname", m_createName, sizeof(m_createName));

        const float pad = 16.0f;
        const float cardW = (panelW - pad * 4.0f) / 3.0f;
        const float cardH = 150.0f;
        const float cardsY = panelY + 156.0f;
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
            if (ImGui::Button(label, ImVec2(cardW, cardH * 0.5f)))
                m_createFaction = f;
            ImGui::PopStyleColor(3);

            const FactionDef* fd = m_ctx->data ? m_ctx->data->GetFaction(f) : nullptr;
            ImGui::SetCursorScreenPos(ImVec2(x + 4.0f, cardsY + cardH * 0.5f + 8.0f));
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + cardW - 8.0f);
            ImGui::TextWrapped("%s", fd && !fd->blurb.empty() ? fd->blurb.c_str() : "");
            ImGui::PopTextWrapPos();
            ++i;
        }

        if (!m_error.empty())
            AddTextCentered(dl, 14.0f, ImVec2(panelX + panelW * 0.5f, panelY + panelH - 100.0f),
                            IM_COL32(235, 110, 105, 235), m_error.c_str());

        const bool blocked = m_pending != PendingOp::None;
        const float footY = panelY + panelH - 64.0f;

        ImGui::SetCursorScreenPos(ImVec2(fieldX, footY));
        ImGui::BeginDisabled(blocked);
        if (ImGui::Button("Create##tf_createsubmit", ImVec2(140.0f, 40.0f)))
            SendCharCreate();
        ImGui::EndDisabled();

        ImGui::SameLine(0.0f, 16.0f);
        ImGui::BeginDisabled(blocked);
        if (ImGui::Button("Back##tf_createback", ImVec2(100.0f, 40.0f)))
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

        AddTextCentered(dl, 24.0f, ImVec2(panelX + panelW * 0.5f, panelY + panelH * 0.45f),
                        IM_COL32(235, 235, 235, 235), "Entering the Cindral Wastes...");

        char dots[8];
        const int n = 1 + (static_cast<int>(m_enterTimer * 2.0f) % 3);
        std::snprintf(dots, sizeof(dots), "%.*s", n, "...");
        AddTextCentered(dl, 16.0f, ImVec2(panelX + panelW * 0.5f, panelY + panelH * 0.45f + 34.0f),
                        IM_COL32(170, 174, 180, 210), dots);
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
