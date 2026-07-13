/**
 * @file TFLoginFlow.cpp
 * @brief W5 onboarding client UI state machine (see TFLoginFlow.h).
 */
#include "UI/TFLoginFlow.h"

#include "Account/TFAccountSystem.h"   // TFAuthErr (error text only; no TFDatabase coupling used)
#include "Account/TFCharacterSystem.h" // TFCharErr, kTFMaxCharSlots
#include "Data/TFDataTables.h"
#include "Net/TFClientNet.h"
#include "UI/TFUiCommon.h"
#include "World/TFWorldSetup.h" // W11 server-browser: Connect() = the tf_connect path

#include "Utils/LogMacros.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace Terrafront
{

    namespace
    {

        const char* AuthErrText(uint8_t errByte)
        {
            switch (static_cast<TFAuthErr>(errByte))
            {
            case TFAuthErr::Ok:
                return "";
            case TFAuthErr::BadCredentials:
                return "Incorrect callsign or passphrase.";
            case TFAuthErr::UsernameTaken:
                return "That callsign is already taken.";
            case TFAuthErr::UsernameTooShort:
                return "Callsign must be at least 3 characters.";
            case TFAuthErr::PasswordTooShort:
                return "Passphrase is too short.";
            case TFAuthErr::ServerError:
                return "Server error - try again.";
            case TFAuthErr::NotLoggedIn:
                return "You are not logged in.";
            default:
                return "Unknown error.";
            }
        }

        const char* CharErrText(uint8_t errByte)
        {
            switch (static_cast<TFCharErr>(errByte))
            {
            case TFCharErr::Ok:
                return "";
            case TFCharErr::SlotsFull:
                return "All character slots are full.";
            case TFCharErr::NameTaken:
                return "That name is already taken.";
            case TFCharErr::NameInvalid:
                return "Invalid name (3-23 characters).";
            case TFCharErr::NoSuchCharacter:
                return "No such character.";
            case TFCharErr::NotYourCharacter:
                return "That character does not belong to you.";
            case TFCharErr::ServerError:
                return "Server error - try again.";
            case TFCharErr::NotLoggedIn:
                return "You are not logged in.";
            default:
                return "Unknown error.";
            }
        }

    } // namespace

    TFLoginFlow::TFLoginFlow() = default;
    TFLoginFlow::~TFLoginFlow()
    {
        if (m_initialized)
            Shutdown();
    }

    bool TFLoginFlow::Initialize(TFGameContext& ctx, TFEventBus& events)
    {
        m_ctx = &ctx;
        m_events = &events;

        m_state = TFFlowState::Login;
        std::memset(m_username, 0, sizeof(m_username));
        std::memset(m_password, 0, sizeof(m_password));
        std::memset(m_createName, 0, sizeof(m_createName));
        m_error.clear();
        m_chars.clear();
        m_selectedIdx = -1;
        m_pending = PendingOp::None;

        // W11 server-browser lane: LAN discovery is owned here because this
        // Update runs on every role (see TFLoginFlow.h). Never boot-fatal.
        if (!m_lan.Initialize(ctx, events))
            SPARK_LOG_WARN(Spark::LogCategory::Game, "[TF] TFLanDiscovery init failed - LAN browser off");

        m_initialized = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] TFLoginFlow initialized");
        return true;
    }

    void TFLoginFlow::Shutdown()
    {
        m_lan.Shutdown(); // W11 server-browser lane
        m_initialized = false;
    }

    void TFLoginFlow::ResetToLogin()
    {
        // See the header comment: called on client disconnect (manual
        // tf_disconnect or a continent hop) so the login screen reappears
        // instead of leaving the game UI believing it is still InWorld.
        m_state = TFFlowState::Login;
        std::memset(m_password, 0, sizeof(m_password));
        m_error.clear();
        m_chars.clear();
        m_selectedIdx = -1;
        m_pending = PendingOp::None;
        m_accountId = 0;
        m_enterTimer = 0.0f;
        // m_username intentionally preserved (same server/account is the
        // common case — re-typing it every hop is pure friction). m_lan is
        // untouched: it self-arms off ctx.role and the login screen render
        // condition, not this flow's m_state.
    }

    void TFLoginFlow::Update(float deltaTime)
    {
        if (!m_initialized || !m_ctx)
            return;

        if (m_state == TFFlowState::EnteringWorld)
            m_enterTimer += deltaTime;

        // W11 server-browser lane: ticks the server beacon (self-arming off
        // ctx.role) and drains/expires scanner results. The scanner itself is only
        // ARMED from RenderLoginScreen, so it stops once the login screen is left
        // (and never starts at all in headless runs, where RenderUI is a stub).
        //
        // W13 multimap follow-up (docs/TERRAFRONT_MULTIMAP.md section 5.1):
        // once InWorld, re-arm it ourselves (RenderLoginScreen no longer
        // renders — TFLoginFlow::RenderUI early-outs on InWorld) so
        // TFTravelSystem::RenderUI's continent-hop terminal has a live feed
        // to prefer over the static continents.json host/port. Gated to
        // NetRole::Client: that's the only role that can ever continent-hop
        // (see TFTravelSystem::ClientRequestContinentHop), so a listen host /
        // dedicated server / standalone loopback never binds the extra
        // socket for no reason. Every other state (CharSelect/CharCreate/
        // EnteringWorld, or InWorld on a non-Client role) stops it exactly as
        // before.
        m_lan.Update(deltaTime);
        if (m_state == TFFlowState::Login || m_state == TFFlowState::Register)
        {
            // armed by RenderLanServerList while this state actually renders
        }
        else if (m_state == TFFlowState::InWorld && m_ctx->role == NetRole::Client)
        {
            m_lan.StartScanning();
        }
        else
        {
            m_lan.StopScanning();
        }
    }

    // ---------------------------------------------------------------------------
    // Reply sinks — called directly by TFClientNet's onboarding handlers
    // (TFClientNetHandlers.cpp) via m_ctx->loginFlow (Task 6).
    // ---------------------------------------------------------------------------

    void TFLoginFlow::OnLoginReply(bool ok, uint8_t err, uint64_t accountId)
    {
        m_pending = PendingOp::None;
        if (ok)
        {
            m_accountId = accountId;
            m_error.clear();
            m_selectedIdx = -1;
            SendCharList();
            m_state = TFFlowState::CharSelect;
            SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] login ok, account %llu",
                           static_cast<unsigned long long>(accountId));
        }
        else
        {
            m_error = AuthErrText(err);
        }
    }

    void TFLoginFlow::OnRegisterReply(bool ok, uint8_t err)
    {
        m_pending = PendingOp::None;
        if (ok)
        {
            m_error = "Account created - sign in below.";
            m_state = TFFlowState::Login;
        }
        else
        {
            m_error = AuthErrText(err);
        }
    }

    void TFLoginFlow::OnCharList(const TF_CharListReply& reply)
    {
        m_pending = PendingOp::None;
        m_chars.assign(reply.chars, reply.chars + std::min<uint8_t>(reply.count, 5));
        if (m_selectedIdx >= static_cast<int>(m_chars.size()))
            m_selectedIdx = -1;
    }

    void TFLoginFlow::OnCharOpReply(bool ok, uint8_t err, uint64_t charId)
    {
        (void)charId;
        m_pending = PendingOp::None;
        if (ok)
        {
            m_error.clear();
            SendCharList();
            m_state = TFFlowState::CharSelect;
        }
        else
        {
            m_error = CharErrText(err);
        }
    }

    void TFLoginFlow::OnEnteredWorld()
    {
        // m_ctx->inWorld is set by the caller (TFClientNet::OnWorldWelcome, the
        // gated enter-world reply) right before/after this call — see
        // TFClientNetHandlers.cpp and TFTypes.h TFGameContext::inWorld.
        m_state = TFFlowState::InWorld;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] entered world");
    }

    // ---------------------------------------------------------------------------
    // Sends
    // ---------------------------------------------------------------------------

    void TFLoginFlow::SendLogin()
    {
        if (!m_ctx || !m_ctx->clientNet)
            return;
        TF_AuthRequest req{};
        std::strncpy(req.user, m_username, sizeof(req.user) - 1);
        std::strncpy(req.pass, m_password, sizeof(req.pass) - 1);
        m_ctx->clientNet->SendMsg(TFMsg::LoginRequest, &req, sizeof(req));
        m_pending = PendingOp::Login;
        m_error.clear();
    }

    void TFLoginFlow::SendRegister()
    {
        if (!m_ctx || !m_ctx->clientNet)
            return;
        TF_AuthRequest req{};
        std::strncpy(req.user, m_username, sizeof(req.user) - 1);
        std::strncpy(req.pass, m_password, sizeof(req.pass) - 1);
        m_ctx->clientNet->SendMsg(TFMsg::RegisterRequest, &req, sizeof(req));
        m_pending = PendingOp::Register;
        m_error.clear();
    }

    void TFLoginFlow::SendCharList()
    {
        if (!m_ctx || !m_ctx->clientNet)
            return;
        m_ctx->clientNet->SendMsg(TFMsg::CharListRequest, nullptr, 0);
        m_pending = PendingOp::CharList;
    }

    void TFLoginFlow::SendCharCreate()
    {
        if (!m_ctx || !m_ctx->clientNet)
            return;
        TF_CharCreateRequest req{};
        std::strncpy(req.name, m_createName, sizeof(req.name) - 1);
        req.faction = static_cast<uint8_t>(m_createFaction);
        m_ctx->clientNet->SendMsg(TFMsg::CharCreateReq, &req, sizeof(req));
        m_pending = PendingOp::CharCreate;
        m_error.clear();
    }

    void TFLoginFlow::SendCharDelete(uint64_t charId)
    {
        if (!m_ctx || !m_ctx->clientNet)
            return;
        TF_CharDeleteRequest req{};
        req.charId = charId;
        m_ctx->clientNet->SendMsg(TFMsg::CharDeleteReq, &req, sizeof(req));
        m_pending = PendingOp::CharDelete;
        m_error.clear();
    }

    void TFLoginFlow::SendEnterWorld(uint64_t charId)
    {
        if (!m_ctx || !m_ctx->clientNet)
            return;
        TF_EnterWorldRequest req{};
        req.charId = charId;
        m_ctx->clientNet->SendMsg(TFMsg::EnterWorldReq, &req, sizeof(req));
        m_enterTimer = 0.0f;
        m_state = TFFlowState::EnteringWorld;
    }

    // ---------------------------------------------------------------------------
    // W11 server-browser lane: join a discovered LAN server via the exact path
    // the tf_connect console command uses (TFWorldSetup::Connect).
    // ---------------------------------------------------------------------------

    void TFLoginFlow::JoinLanServer(const std::string& ip, uint16_t port)
    {
        if (!m_ctx || !m_ctx->world)
            return;
        if (m_ctx->role != NetRole::Standalone)
        {
            m_error = "Already connected - restart to join a different server.";
            return;
        }
        if (m_ctx->world->Connect(ip, port))
            m_error = "Connecting to " + ip + ":" + std::to_string(port) + " - sign in to deploy.";
        else
            m_error = "Connect to " + ip + ":" + std::to_string(port) + " failed (see log).";
    }

    // ---------------------------------------------------------------------------
    // Rendering
    // ---------------------------------------------------------------------------

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
