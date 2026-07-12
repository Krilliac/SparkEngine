/**
 * @file TFTutorial.cpp
 * @brief First-join sanctuary tutorial (see the header for the read-only
 *        detection + session-local persistence contract).
 *
 * PURE CLIENT PRESENTATION: no TFMsg, no authority state, no mutation of any
 * other system — every signal below is a read that already exists.
 */
#include "Game/TFTutorial.h"

#include "Game/TFAbilitySystem.h"
#include "Game/TFOpticsSystem.h"
#include "Game/TFPingSystem.h"
#include "Game/TFPlayerSystem.h"
#include "Game/TFTargetRange.h"
#include "Game/TFUiSounds.h"
#include "UI/TFChatWindow.h"
#include "UI/TFHUD.h"
#include "UI/TFKeybinds.h" // kVkEscape
#include "UI/TFLoginFlow.h"
#include "UI/TFMapScreen.h"
#include "UI/TFSocialPanel.h"
#include "UI/TFSpawnScreen.h"
#include "World/TFSanctuaryZone.h"
#include "World/TFWorldSetup.h"

#include "Camera/SparkEngineCamera.h"
#include "Input/InputManager.h"
#include "Spark/IEngineContext.h"
#include "Utils/LogMacros.h"
#include "Utils/SparkConsole.h"

#ifdef SPARK_HAS_IMGUI
#include "UI/TFUiCommon.h"
#include <imgui.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace Terrafront
{

    namespace
    {

        // ---- input (TFClientNet::SampleAndSendInput's VK codes) ---------------
        constexpr int kVkShift = 0x10;
        constexpr int kVkSpace = 0x20;

        // ---- step tuning --------------------------------------------------------
        constexpr float kMoveHoldSec = 1.5f;   ///< cumulative WASD time for Move
        constexpr float kSprintHoldSec = 1.0f; ///< cumulative Shift+move time
        constexpr float kToastDefaultSec = 4.0f;

        // ---- world anchors (own copies — cosmetic markers only) ----------------
        // MUST match TFTargetRange.cpp kRangeCenterX / kLineZ (firing line).
        constexpr float kRangeMarkX = 404.0f;
        constexpr float kRangeMarkZ = 3878.0f;
        // MUST match TFSanctuaryDecor.cpp kClassTermX / kClassTermZ.
        constexpr float kClassTermX = 320.0f;
        constexpr float kClassTermZ = 3826.0f;
        /// Visit radius: slightly wider than the decor's 4.5 m [E] radius so
        /// walking up to the prompt always completes the step.
        constexpr float kClassTermVisitM = 5.5f;

        // Projection recipe copied from TFTargetRange::RenderUI — must match
        // TFWorldSetup::ComputeViewProj's first-person branch (XM_PIDIV4 * 1.6,
        // near 0.3); cosmetic drift tolerance only.
        constexpr float kFovY = 0.78539816f * 1.6f;
        constexpr float kNearZ = 0.3f;

        float DistSqXZ(const float p[3], float x, float z)
        {
            const float dx = p[0] - x;
            const float dz = p[2] - z;
            return dx * dx + dz * dz;
        }

        const char* StepLabel(TFTutorial::Step s)
        {
            switch (s)
            {
            case TFTutorial::Step::Move:
                return "Move around (W A S D)";
            case TFTutorial::Step::Sprint:
                return "Sprint (hold SHIFT while moving)";
            case TFTutorial::Step::Jump:
                return "Jump (SPACE)";
            case TFTutorial::Step::RangeHits:
                return "Firing range: hit a dummy 5 times";
            case TFTutorial::Step::Optics:
                return "Cycle your weapon sights (B)";
            case TFTutorial::Step::Ability:
                return "Use your class ability (F)";
            case TFTutorial::Step::Map:
                return "Open the continent map (M)";
            case TFTutorial::Step::Ping:
                return "Place a ping (Q)";
            case TFTutorial::Step::ClassTerm:
                return "Visit the class terminal";
            case TFTutorial::Step::Travel:
                return "Travel terminal: deploy when ready (E)";
            default:
                return "?";
            }
        }

    } // namespace

    TFTutorial::TFTutorial() = default;
    TFTutorial::~TFTutorial()
    {
        if (m_initialized)
            Shutdown();
    }

    // ---------------------------------------------------------------------------
    // Lifecycle
    // ---------------------------------------------------------------------------

    bool TFTutorial::Initialize(TFGameContext& ctx)
    {
        m_ctx = &ctx;
        m_initialized = true;

        auto& console = Spark::SimpleConsole::GetInstance();
        if (!console.HasCommand("tf_tutorial"))
        {
            console.RegisterCommand(
                "tf_tutorial",
                [this](const std::vector<std::string>& args) -> std::string
                {
                    const std::string sub = args.empty() ? std::string("status") : args[0];
                    if (sub == "reset")
                    {
                        ResetTutorial();
                        return "[TF] tutorial reset (restarts on the next alive sanctuary frame)";
                    }
                    if (sub == "skip")
                    {
                        SkipTutorial();
                        return "[TF] tutorial skipped";
                    }
                    char buf[160];
                    std::snprintf(buf, sizeof(buf),
                                  "[TF] tutorial: active=%d done=%d step=%u (%s) rangeHits=%u/%u (session-local; "
                                  "tf_tutorial reset|skip)",
                                  m_active ? 1 : 0, m_done ? 1 : 0, static_cast<unsigned>(m_step), StepLabel(m_step),
                                  m_rangeHits, kTFTutorialRangeHits);
                    return std::string(buf);
                },
                "First-join sanctuary tutorial: tf_tutorial [status|reset|skip]", "TERRAFRONT",
                "tf_tutorial [status|reset|skip]");
            m_debugCmd = true;
        }

        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] TFTutorial initialized (session-local, client-only)");
        return true;
    }

    void TFTutorial::AttachRange(TFTargetRange* range)
    {
        m_range = range;
        if (!m_range)
            return;
        m_range->SetHitObserver(
            [this](float /*dmgEstimate*/, float /*distM*/)
            {
                if (m_active && !m_done && m_step == Step::RangeHits)
                    ++m_rangeHits;
            });
    }

    void TFTutorial::Shutdown()
    {
        if (!m_initialized)
            return;
        if (m_debugCmd)
        {
            Spark::SimpleConsole::GetInstance().UnregisterCommand("tf_tutorial");
            m_debugCmd = false;
        }
        // Detach the observer BEFORE this object dies — the lambda captures
        // `this`. TFTravelSystem shuts the tutorial down before the range, so
        // m_range is still alive here in the normal teardown order.
        if (m_range)
        {
            m_range->SetHitObserver({});
            m_range = nullptr;
        }
        m_initialized = false;
    }

    // ---------------------------------------------------------------------------
    // Flow control
    // ---------------------------------------------------------------------------

    void TFTutorial::ResetTutorial()
    {
        m_done = false;
        m_active = false; // restarts on the next alive sanctuary frame
        m_step = Step::Move;
        m_moveSec = 0.0f;
        m_sprintSec = 0.0f;
        m_rangeHits = 0;
        m_opticBaseValid = false;
        m_escHoldSec = 0.0f;
        SetToast("Tutorial reset", kToastDefaultSec);
    }

    void TFTutorial::SkipTutorial()
    {
        if (m_done)
            return;
        m_active = true; // a pre-start skip still marks the session done
        FinishTutorial("Tutorial skipped");
        TFUiSounds_Play(m_ctx, TFUiBleep::Close);
    }

    void TFTutorial::CompleteStep()
    {
        m_step = static_cast<Step>(static_cast<uint8_t>(m_step) + 1);
        m_escHoldSec = 0.0f;
        TFUiSounds_Play(m_ctx, TFUiBleep::Confirm);
    }

    void TFTutorial::FinishTutorial(const char* toast)
    {
        m_done = true;
        SetToast(toast, 5.0f);
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] tutorial finished (%s)", toast);
    }

    void TFTutorial::SetToast(const char* text, float ttlSec)
    {
        std::snprintf(m_toast, sizeof(m_toast), "%s", text);
        m_toastTTL = ttlSec;
    }

    // ---------------------------------------------------------------------------
    // Gates (TFPingUI::FullscreenUiOpen mirror)
    // ---------------------------------------------------------------------------

    bool TFTutorial::FullscreenUiOpen() const
    {
        return (m_ctx->map && m_ctx->map->IsOpen()) || (m_ctx->spawnUI && m_ctx->spawnUI->IsOpen()) ||
               (m_ctx->loginFlow && m_ctx->loginFlow->IsOpen()) || (m_ctx->hud && m_ctx->hud->IsChatOpen()) ||
               (m_ctx->chatWindow && m_ctx->chatWindow->IsOpen()) ||
               (m_ctx->socialPanel && m_ctx->socialPanel->IsOpen());
    }

    bool TFTutorial::LocalPawn(float outPos[3], bool& outAlive) const
    {
        outAlive = false;
        if (!m_ctx->players || m_ctx->localPlayer == kInvalidPlayer)
            return false;
        PawnInfo pawn{};
        if (!m_ctx->players->GetPawnByPlayer(m_ctx->localPlayer, pawn))
            return false;
        outPos[0] = pawn.pos[0];
        outPos[1] = pawn.pos[1];
        outPos[2] = pawn.pos[2];
        outAlive = pawn.alive;
        return true;
    }

    // ---------------------------------------------------------------------------
    // Detection state machine
    // ---------------------------------------------------------------------------

    void TFTutorial::Update(float deltaTime)
    {
        if (!m_initialized || !m_ctx || !m_ctx->HasLocalPlayer())
            return;
        m_clock += deltaTime;
        if (m_toastTTL > 0.0f)
            m_toastTTL -= deltaTime;
        if (m_done || !m_ctx->InWorld())
            return;

        float pos[3];
        bool alive = false;
        if (!LocalPawn(pos, alive) || !alive)
            return; // no pawn / dead: paused

        const bool inSanctuary = TFTravel_IsInSanctuary(pos[0], pos[2]);

        if (!m_active)
        {
            if (!inSanctuary)
                return; // never started and already deployed: stay quiet
            m_active = true;
            SetToast("Tutorial started - follow the checklist", kToastDefaultSec);
            TFUiSounds_Play(m_ctx, TFUiBleep::Open);
        }

        if (!inSanctuary)
        {
            // Continent = combat zone: the tutorial pauses... except the final
            // step, which IS "leave through the travel terminal".
            if (m_step == Step::Travel)
                FinishTutorial("Tutorial complete - good hunting");
            m_escHoldSec = 0.0f;
            return;
        }

        // ---- skip: hold ESC (never while an overlay owns the key) --------------
        InputManager* input = m_ctx->engine ? m_ctx->engine->GetInput() : nullptr;
        if (input && !FullscreenUiOpen() && input->IsKeyDown(TFKeys::kVkEscape))
        {
            m_escHoldSec += deltaTime;
            if (m_escHoldSec >= kTFTutorialSkipHoldSec)
            {
                SkipTutorial();
                return;
            }
        }
        else
        {
            m_escHoldSec = 0.0f;
        }

        DetectCurrentStep(deltaTime, pos);
    }

    void TFTutorial::DetectCurrentStep(float dt, const float pawnPos[3])
    {
        InputManager* input = m_ctx->engine ? m_ctx->engine->GetInput() : nullptr;
        const bool uiOpen = FullscreenUiOpen();

        switch (m_step)
        {
        case Step::Move:
        {
            if (uiOpen || !input)
                break;
            if (input->IsKeyDown('W') || input->IsKeyDown('A') || input->IsKeyDown('S') || input->IsKeyDown('D'))
                m_moveSec += dt;
            if (m_moveSec >= kMoveHoldSec)
                CompleteStep();
            break;
        }
        case Step::Sprint:
        {
            if (uiOpen || !input)
                break;
            const bool moving =
                input->IsKeyDown('W') || input->IsKeyDown('A') || input->IsKeyDown('S') || input->IsKeyDown('D');
            if (moving && input->IsKeyDown(kVkShift))
                m_sprintSec += dt;
            if (m_sprintSec >= kSprintHoldSec)
                CompleteStep();
            break;
        }
        case Step::Jump:
        {
            if (uiOpen || !input)
                break;
            if (input->WasKeyPressed(kVkSpace))
                CompleteStep();
            break;
        }
        case Step::RangeHits:
        {
            // Counted by the TFTargetRange hit observer (AttachRange).
            if (m_rangeHits >= kTFTutorialRangeHits)
                CompleteStep();
            break;
        }
        case Step::Optics:
        {
            const uint8_t cur = static_cast<uint8_t>(TFOpticsSystem::Get().ActiveOptic());
            if (!m_opticBaseValid)
            {
                m_opticBase = cur;
                m_opticBaseValid = true;
            }
            // Real cycle detected — or a B press as the acknowledge fallback
            // (irons-only weapons have nothing to cycle to).
            if (cur != m_opticBase || (!uiOpen && input && input->WasKeyPressed('B')))
                CompleteStep();
            break;
        }
        case Step::Ability:
        {
            bool used = false;
            if (m_ctx->abilities)
            {
                const TFAbilitySystem::HudView view = m_ctx->abilities->GetLocalHudView();
                used = view.valid && view.phase != TFAbilityPhase::Ready;
            }
            // Fallback acknowledge when no ability row resolves for the class.
            if (!used && !uiOpen && input && input->WasKeyPressed('F'))
                used = true;
            if (used)
                CompleteStep();
            break;
        }
        case Step::Map:
        {
            // The map IS a fullscreen UI — deliberately not gated on uiOpen.
            if (m_ctx->map && m_ctx->map->IsOpen())
                CompleteStep();
            break;
        }
        case Step::Ping:
        {
            bool placed = false;
            if (m_ctx->pings)
            {
                m_ctx->pings->ForEachActivePing(
                    [&](const TFPingView& v)
                    {
                        if (v.owner == m_ctx->localPlayer)
                            placed = true;
                    });
            }
            else if (!uiOpen && input && input->WasKeyPressed('Q'))
            {
                placed = true; // ping system absent: acknowledge fallback
            }
            if (placed)
                CompleteStep();
            break;
        }
        case Step::ClassTerm:
        {
            if (DistSqXZ(pawnPos, kClassTermX, kClassTermZ) <= kClassTermVisitM * kClassTermVisitM)
                CompleteStep();
            break;
        }
        case Step::Travel:
        default:
            // Completed by the sanctuary-exit check in Update().
            break;
        }
    }

    // ---------------------------------------------------------------------------
    // Presentation (checklist + world markers + toast)
    // ---------------------------------------------------------------------------

#ifdef SPARK_HAS_IMGUI

    void TFTutorial::RenderUI()
    {
        if (!m_initialized || !m_ctx || !m_ctx->HasLocalPlayer())
            return;

        ImGuiViewport* vp = ImGui::GetMainViewport();
        if (!vp || vp->Size.x <= 0.0f || vp->Size.y <= 0.0f)
            return;

        // Toast (start/finish/skip/reset) — drawn even when done so the
        // completion line survives arriving on the continent.
        if (m_toastTTL > 0.0f && m_toast[0] != '\0' && !FullscreenUiOpen())
        {
            const float a = std::clamp(m_toastTTL / 0.6f, 0.0f, 1.0f);
            TFUi::AddTextCentered(ImGui::GetForegroundDrawList(), 20.0f,
                                  ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.18f),
                                  IM_COL32(150, 230, 255, static_cast<int>(235.0f * a)), m_toast);
        }

        if (!m_active || m_done || FullscreenUiOpen())
            return;

        float pos[3];
        bool alive = false;
        if (!LocalPawn(pos, alive) || !alive)
            return;
        if (!TFTravel_IsInSanctuary(pos[0], pos[2]))
            return; // combat zone: auto-hidden (paused)

        // ---- compact checklist (right edge, below the minimap band) ------------
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x - 14.0f, vp->Pos.y + vp->Size.y * 0.30f),
                                ImGuiCond_Always, ImVec2(1.0f, 0.0f));
        ImGui::SetNextWindowBgAlpha(0.55f);
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize |
                                       ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                                       ImGuiWindowFlags_NoInputs;
        if (ImGui::Begin("##tf_tutorial", nullptr, flags))
        {
            ImGui::TextColored(ImVec4(0.55f, 0.86f, 1.0f, 1.0f), "TUTORIAL");
            ImGui::SameLine();
            if (m_escHoldSec > 0.05f)
                ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.4f, 1.0f), " skipping %d%%",
                                   static_cast<int>(100.0f * m_escHoldSec / kTFTutorialSkipHoldSec));
            else
                ImGui::TextDisabled(" hold ESC to skip");
            ImGui::Separator();

            char line[128];
            for (uint8_t i = 0; i < static_cast<uint8_t>(Step::COUNT); ++i)
            {
                const Step s = static_cast<Step>(i);
                const bool doneStep = i < static_cast<uint8_t>(m_step);
                const bool current = s == m_step;
                if (doneStep)
                {
                    std::snprintf(line, sizeof(line), "[x] %s", StepLabel(s));
                    ImGui::TextColored(ImVec4(0.45f, 0.75f, 0.5f, 0.85f), "%s", line);
                }
                else if (current)
                {
                    std::snprintf(line, sizeof(line), " >  %s", StepLabel(s));
                    ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.5f, 1.0f), "%s", line);
                    if (s == Step::Move)
                        ImGui::TextDisabled("      %d%%", static_cast<int>(100.0f * m_moveSec / kMoveHoldSec));
                    else if (s == Step::Sprint)
                        ImGui::TextDisabled("      %d%%", static_cast<int>(100.0f * m_sprintSec / kSprintHoldSec));
                    else if (s == Step::RangeHits)
                        ImGui::TextDisabled("      hits %u/%u  (follow the marker)", m_rangeHits, kTFTutorialRangeHits);
                    else if (s == Step::ClassTerm || s == Step::Travel)
                        ImGui::TextDisabled("      follow the marker");
                }
                else
                {
                    std::snprintf(line, sizeof(line), "[ ] %s", StepLabel(s));
                    ImGui::TextDisabled("%s", line);
                }
            }
        }
        ImGui::End();

        // ---- world-anchored hint marker for the current step -------------------
        float anchorX = 0.0f, anchorZ = 0.0f;
        const char* anchorLabel = nullptr;
        if (m_step == Step::RangeHits)
        {
            anchorX = kRangeMarkX;
            anchorZ = kRangeMarkZ;
            anchorLabel = "FIRING RANGE";
        }
        else if (m_step == Step::ClassTerm)
        {
            anchorX = kClassTermX;
            anchorZ = kClassTermZ;
            anchorLabel = "CLASS TERMINAL";
        }
        else if (m_step == Step::Travel)
        {
            anchorX = kTFSanctuaryTerminalX;
            anchorZ = kTFSanctuaryTerminalZ;
            anchorLabel = "TRAVEL TERMINAL";
        }
        if (!anchorLabel || !m_ctx->world)
            return;

        const SparkEngineCamera* cam = m_ctx->world->GetCamera();
        if (!cam)
            return;

        // Camera basis + projection: own copy of the TFTargetRange::RenderUI
        // recipe (must match TFWorldSetup::ComputeViewProj's first-person
        // branch; cosmetic drift tolerance only).
        const auto cp = cam->GetPosition();
        const auto cf = cam->GetForward();
        float fwd[3] = {cf.x, cf.y, cf.z};
        const float fl = std::sqrt(fwd[0] * fwd[0] + fwd[1] * fwd[1] + fwd[2] * fwd[2]);
        if (fl < 1.0e-4f)
            return;
        fwd[0] /= fl;
        fwd[1] /= fl;
        fwd[2] /= fl;
        float right[3] = {fwd[2], 0.0f, -fwd[0]};
        const float rl = std::sqrt(right[0] * right[0] + right[2] * right[2]);
        if (rl < 1.0e-4f)
            return; // looking straight up/down: skip this frame
        right[0] /= rl;
        right[2] /= rl;
        const float up[3] = {fwd[1] * right[2] - fwd[2] * right[1], fwd[2] * right[0] - fwd[0] * right[2],
                             fwd[0] * right[1] - fwd[1] * right[0]};

        const float w = vp->Size.x;
        const float h = vp->Size.y;
        const float tanHalfY = std::tan(kFovY * 0.5f);
        const float aspect = w / h;
        const auto project = [&](const float p[3], ImVec2& out) -> bool
        {
            const float vx = p[0] - cp.x;
            const float vy = p[1] - cp.y;
            const float vz = p[2] - cp.z;
            const float cxv = vx * right[0] + vy * right[1] + vz * right[2];
            const float cyv = vx * up[0] + vy * up[1] + vz * up[2];
            const float czv = vx * fwd[0] + vy * fwd[1] + vz * fwd[2];
            if (czv <= kNearZ)
                return false;
            const float ndcX = cxv / (czv * tanHalfY * aspect);
            const float ndcY = cyv / (czv * tanHalfY);
            out.x = vp->Pos.x + (ndcX * 0.5f + 0.5f) * w;
            out.y = vp->Pos.y + (0.5f - ndcY * 0.5f) * h;
            return true;
        };

        // Bobbing diamond + label + distance (ping-marker spirit, own copy).
        const float bob = 0.25f * std::sin(static_cast<float>(m_clock) * 2.4f);
        const float markerPos[3] = {anchorX, m_ctx->world->TerrainHeightAt(anchorX, anchorZ) + 3.0f + bob, anchorZ};
        ImVec2 at;
        if (!project(markerPos, at))
            return;
        ImDrawList* fg = ImGui::GetForegroundDrawList();
        const float pulse = 0.75f + 0.25f * std::sin(static_cast<float>(m_clock) * 3.1f);
        const int alpha = static_cast<int>(210.0f * pulse);
        const float s = 9.0f;
        const ImVec2 pts[4] = {ImVec2(at.x, at.y - s), ImVec2(at.x + s, at.y), ImVec2(at.x, at.y + s),
                               ImVec2(at.x - s, at.y)};
        fg->AddQuadFilled(pts[0], pts[1], pts[2], pts[3], IM_COL32(140, 220, 255, alpha / 2));
        fg->AddQuad(pts[0], pts[1], pts[2], pts[3], IM_COL32(170, 235, 255, alpha), 1.5f);
        TFUi::AddTextCentered(fg, 14.0f, ImVec2(at.x, at.y + s + 12.0f), IM_COL32(170, 235, 255, alpha), anchorLabel);
        const float dx = pos[0] - anchorX;
        const float dz = pos[2] - anchorZ;
        char dist[24];
        std::snprintf(dist, sizeof(dist), "%.0f m", std::sqrt(dx * dx + dz * dz));
        TFUi::AddTextCentered(fg, 12.0f, ImVec2(at.x, at.y + s + 26.0f), IM_COL32(150, 190, 215, alpha), dist);
    }

#else // !SPARK_HAS_IMGUI

    void TFTutorial::RenderUI() {}

#endif // SPARK_HAS_IMGUI

} // namespace Terrafront
