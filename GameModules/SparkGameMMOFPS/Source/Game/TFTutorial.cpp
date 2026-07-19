/**
 * @file TFTutorial.cpp
 * @brief First-join sanctuary tutorial: lifecycle, console command, flow
 *        control and the read-only detection state machine (see the header
 *        for the detection + session-local persistence contract). The ImGui
 *        checklist / marker presentation lives in TFTutorialUi.cpp.
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
#include "Game/TFTutorialInternal.h"
#include "Game/TFUiSounds.h"
#include "UI/TFChatWindow.h"
#include "UI/TFHUD.h"
#include "UI/TFKeybinds.h" // kVkEscape
#include "UI/TFLoginFlow.h"
#include "UI/TFMapScreen.h"
#include "UI/TFSocialPanel.h"
#include "UI/TFSpawnScreen.h"
#include "World/TFSanctuaryZone.h"

#include "Input/InputManager.h"
#include "Spark/IEngineContext.h"
#include "Utils/LogMacros.h"
#include "Utils/SparkConsole.h"

#include <cstdio>
#include <cstring>

namespace Terrafront
{

    using namespace TutorialDetail;

    namespace
    {

        // ---- input (TFClientNet::SampleAndSendInput's VK codes) ---------------
        constexpr int kVkShift = 0x10;
        constexpr int kVkSpace = 0x20;

        // ---- step tuning --------------------------------------------------------
        constexpr float kToastDefaultSec = 4.0f;

        /// Visit radius: slightly wider than the decor's 4.5 m [E] radius so
        /// walking up to the prompt always completes the step.
        constexpr float kClassTermVisitM = 5.5f;

        float DistSqXZ(const float p[3], float x, float z)
        {
            const float dx = p[0] - x;
            const float dz = p[2] - z;
            return dx * dx + dz * dz;
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

} // namespace Terrafront
