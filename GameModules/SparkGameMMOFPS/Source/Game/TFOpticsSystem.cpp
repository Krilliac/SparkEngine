/**
 * @file TFOpticsSystem.cpp
 * @brief Weapon optics presentation (see TFOpticsSystem.h for the contract).
 *        ADS blend + FOV scale + hold-breath sway are plain float math (safe
 *        headless); the reticle overlays compile only under SPARK_HAS_IMGUI,
 *        drawn straight onto the foreground draw list like TFNameplates /
 *        TFTargetRange (no window, no input capture).
 */
#include "Game/TFOpticsSystem.h"

#include "Game/TFWeaponSystem.h"

#include "Input/InputManager.h"
#include "Spark/IEngineContext.h"
#include "Utils/JsonUtils.h"
#include "Utils/LogMacros.h"

#ifdef SPARK_HAS_IMGUI
#include <imgui.h>
#endif

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

namespace Terrafront
{

    namespace
    {

        // ------------------------------------------------------------- tuning
        // Zoom: multiplier on the projection FOV ANGLE while fully ADS. The 4x
        // scope quarters the angle (the classic small-angle 4x magnification);
        // the red dot is a mild 0.9x — present but never disorienting.
        constexpr float kRedDotFovScale = 0.9f;
        constexpr float kX4FovScale = 0.25f;

        // ADS pacing: blend-in paced by the weapon's own adsSec (x4 pays a
        // 1.5x tax — sniper standard); blend-out is snappier than blend-in so
        // leaving the scope never feels sticky. Floors keep adsSec==0 defs
        // (tools) well-defined even though they never reach this path.
        constexpr float kX4AdsTimeMult = 1.5f;
        constexpr float kAdsOutTimeMult = 0.6f;
        constexpr float kAdsMinSec = 0.06f;

        // Hold-breath sway (x4 only, radians): two incommensurate sines per
        // axis so the drift never settles into a loop. Peak combined amplitude
        // ~0.1 deg — visible at 4x magnification, imperceptible as motion
        // sickness (and visual-only: fire rays never see it).
        constexpr float kSwayAmpRad = 0.0011f;
        constexpr float kSwayYawHzA = 0.22f, kSwayYawHzB = 0.37f;
        constexpr float kSwayPitchHzA = 0.19f, kSwayPitchHzB = 0.31f;
        constexpr float kSwayBlendStart = 0.75f; ///< sway fades in over the last blend quarter

        // Overlay thresholds / toast.
        constexpr float kOverlayBlendStart = 0.55f; ///< reticles fade in past this blend
        constexpr float kHideViewModelBlend = 0.85f;
        constexpr double kToastSec = 1.4;

        constexpr float kTwoPi = 6.28318531f;

        constexpr const char* kWeaponsJsonPath = "Assets/MMOFPS/Data/weapons.json";

        float Clamp01(float v)
        {
            return std::clamp(v, 0.0f, 1.0f);
        }

        /// Smoothstep the raw blend so zoom eases in/out instead of ramping.
        float Smooth(float t)
        {
            t = Clamp01(t);
            return t * t * (3.0f - 2.0f * t);
        }

        bool ParseOpticName(const std::string& name, TFOpticKind& out)
        {
            if (name == "irons")
                out = TFOpticKind::Irons;
            else if (name == "reddot")
                out = TFOpticKind::RedDot;
            else if (name == "x4")
                out = TFOpticKind::X4;
            else
                return false;
            return true;
        }

        int AllowedCount(uint8_t mask)
        {
            int n = 0;
            for (int b = 0; b < 3; ++b)
                if (mask & (1u << b))
                    ++n;
            return n;
        }

    } // namespace

    // ---------------------------------------------------------------------------

    TFOpticsSystem& TFOpticsSystem::Get()
    {
        static TFOpticsSystem s_instance;
        return s_instance;
    }

    const char* TFOpticsSystem::OpticDisplayName(TFOpticKind kind)
    {
        switch (kind)
        {
        case TFOpticKind::RedDot:
            return "Red Dot";
        case TFOpticKind::X4:
            return "4x Scope";
        case TFOpticKind::Irons:
        default:
            return "Iron Sights";
        }
    }

    void TFOpticsSystem::EnsureTable()
    {
        if (m_tableLoaded)
            return;
        m_tableLoaded = true; // one attempt; missing file == all irons (feature off)

        std::ifstream f(kWeaponsJsonPath, std::ios::binary);
        if (!f.is_open())
        {
            SPARK_LOG_WARN(Spark::LogCategory::Game, "[TF] optics: cannot open %s (irons only)", kWeaponsJsonPath);
            return;
        }
        std::ostringstream ss;
        ss << f.rdbuf();

        const Spark::Json::Value root = Spark::Json::Parse(ss.str());
        if (!root.IsObject() || !root.HasKey("weapons") || !root["weapons"].IsArray())
        {
            SPARK_LOG_WARN(Spark::LogCategory::Game, "[TF] optics: %s malformed (irons only)", kWeaponsJsonPath);
            return;
        }

        const Spark::Json::Value& weapons = root["weapons"];
        for (size_t i = 0; i < weapons.Size(); ++i)
        {
            const Spark::Json::Value& w = weapons[i];
            if (!w.IsObject() || !w.HasKey("key") || !w.HasKey("optics") || !w["optics"].IsArray())
                continue;
            const std::string key = w["key"].AsString({});
            if (key.empty())
                continue;

            OpticList list{};
            list.mask = 0;
            bool haveDefault = false;
            const Spark::Json::Value& optics = w["optics"];
            for (size_t j = 0; j < optics.Size(); ++j)
            {
                TFOpticKind kind{};
                const std::string name = optics[j].AsString({});
                if (!ParseOpticName(name, kind))
                {
                    SPARK_LOG_WARN(Spark::LogCategory::Game, "[TF] optics: '%s': unknown optic '%s' (skipped)",
                                   key.c_str(), name.c_str());
                    continue;
                }
                list.mask |= static_cast<uint8_t>(1u << static_cast<uint8_t>(kind));
                if (!haveDefault)
                {
                    list.defaultSel = static_cast<uint8_t>(kind); // first entry == default sight
                    haveDefault = true;
                }
            }
            if (list.mask == 0) // authored-but-empty/bogus list: keep irons
                list = OpticList{};
            m_allowed.emplace(key, list);
        }
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] optics: %zu weapons with sight lists", m_allowed.size());
    }

    uint8_t TFOpticsSystem::AllowedMask(std::string_view weaponKey)
    {
        EnsureTable();
        const auto it = m_allowed.find(std::string(weaponKey));
        return it != m_allowed.end() ? it->second.mask : OpticList{}.mask;
    }

    void TFOpticsSystem::SetSelected(std::string_view weaponKey, TFOpticKind kind)
    {
        EnsureTable();
        const std::string key(weaponKey);
        const auto it = m_allowed.find(key);
        const uint8_t mask = it != m_allowed.end() ? it->second.mask : OpticList{}.mask;
        if ((mask & (1u << static_cast<uint8_t>(kind))) == 0)
            return; // not an allowed sight for this weapon
        m_selected[key] = static_cast<uint8_t>(kind);
    }

    void TFOpticsSystem::NotifyInactive()
    {
        m_blend = 0.0f;
        m_optic = TFOpticKind::Irons;
        m_activeKey.clear();
        m_toastUntil = -1.0; // m_clock freezes while inactive; never strand a toast
    }

    float TFOpticsSystem::CameraFovScale() const
    {
        float scale = 1.0f;
        if (m_optic == TFOpticKind::RedDot)
            scale = kRedDotFovScale;
        else if (m_optic == TFOpticKind::X4)
            scale = kX4FovScale;
        return 1.0f + (scale - 1.0f) * Smooth(m_blend);
    }

    void TFOpticsSystem::CameraSwayRad(float& outYawRad, float& outPitchRad) const
    {
        outYawRad = 0.0f;
        outPitchRad = 0.0f;
        if (m_optic != TFOpticKind::X4 || m_blend <= kSwayBlendStart)
            return;
        const float gain = Clamp01((m_blend - kSwayBlendStart) / (1.0f - kSwayBlendStart));
        const float t = static_cast<float>(m_clock);
        outYawRad = kSwayAmpRad * gain *
                    (std::sin(kTwoPi * kSwayYawHzA * t) + 0.5f * std::sin(kTwoPi * kSwayYawHzB * t + 1.3f));
        outPitchRad = kSwayAmpRad * gain *
                      (std::sin(kTwoPi * kSwayPitchHzA * t + 0.7f) + 0.5f * std::sin(kTwoPi * kSwayPitchHzB * t));
    }

    bool TFOpticsSystem::HideViewModel() const
    {
        return m_optic == TFOpticKind::X4 && m_blend >= kHideViewModelBlend;
    }

    // ---------------------------------------------------------------------------

    void TFOpticsSystem::Update(TFGameContext& ctx, float dt)
    {
        m_clock += dt;
        EnsureTable();

        const WeaponDef* def = ctx.weapons ? ctx.weapons->ActiveWeaponDefLocal() : nullptr;
        if (!def || def->key.empty())
        {
            NotifyInactive();
            return;
        }
        if (def->key != m_activeKey)
        {
            m_activeKey = def->key;
            m_blend = 0.0f; // fresh raise — a weapon swap never arrives pre-zoomed
        }

        OpticList list{};
        if (auto it = m_allowed.find(m_activeKey); it != m_allowed.end())
            list = it->second;

        uint8_t sel = list.defaultSel;
        if (auto it = m_selected.find(m_activeKey); it != m_selected.end() && (list.mask & (1u << it->second)) != 0)
            sel = it->second;

        // B cycles the sight while holding the weapon (on-foot only here; the
        // seated Aegis deploy 'B' is unreachable from this call site).
        InputManager* input = ctx.engine ? ctx.engine->GetInput() : nullptr;
        if (input && AllowedCount(list.mask) > 1 && input->WasKeyPressed('B'))
        {
            do
            {
                sel = static_cast<uint8_t>((sel + 1u) % 3u);
            } while ((list.mask & (1u << sel)) == 0);
            m_selected[m_activeKey] = sel;
            m_toastText = std::string("Optic: ") + OpticDisplayName(static_cast<TFOpticKind>(sel));
            m_toastUntil = m_clock + kToastSec;
            m_blend = std::min(m_blend, 0.2f); // re-raise the new sight
        }
        m_optic = static_cast<TFOpticKind>(sel);

        // ADS blend — irons keep the exact pre-W11 behavior (blend pinned 0).
        const bool wantAds = ctx.weapons->IsAdsLocal() && m_optic != TFOpticKind::Irons;
        const float adsInSec = std::max(kAdsMinSec, def->adsSec * (m_optic == TFOpticKind::X4 ? kX4AdsTimeMult : 1.0f));
        const float adsOutSec = std::max(kAdsMinSec, def->adsSec * kAdsOutTimeMult);
        m_blend = Clamp01(m_blend + (wantAds ? dt / adsInSec : -dt / adsOutSec));
    }

    void TFOpticsSystem::RenderOverlay()
    {
#ifdef SPARK_HAS_IMGUI
        if (!ImGui::GetCurrentContext())
            return;
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        if (!vp || vp->Size.x <= 0.0f || vp->Size.y <= 0.0f)
            return;
        ImDrawList* fg = ImGui::GetForegroundDrawList();
        const float w = vp->Size.x;
        const float h = vp->Size.y;
        const ImVec2 c(vp->Pos.x + w * 0.5f, vp->Pos.y + h * 0.5f);

        const float overlayA = Clamp01((m_blend - kOverlayBlendStart) / (1.0f - kOverlayBlendStart)); // reticle fade-in

        if (m_optic == TFOpticKind::RedDot && overlayA > 0.0f)
        {
            // Dot reticle: dark sight ring + a bright dot with a faint glow.
            const float ringR = std::max(14.0f, h * 0.032f);
            const float dotR = std::max(2.0f, h * 0.0035f);
            fg->AddCircle(c, ringR, IM_COL32(15, 15, 18, static_cast<int>(150.0f * overlayA)), 48, 2.0f);
            fg->AddCircleFilled(c, dotR * 2.2f, IM_COL32(255, 70, 45, static_cast<int>(60.0f * overlayA)));
            fg->AddCircleFilled(c, dotR, IM_COL32(255, 62, 38, static_cast<int>(235.0f * overlayA)));
        }
        else if (m_optic == TFOpticKind::X4 && overlayA > 0.0f)
        {
            // Scope: dark vignette annulus (a huge-thickness circle stroke
            // whose inner edge is the scope hole — covers the corners), a
            // double scope ring, and a thin crosshair with range ticks.
            const float holeR = std::min(w, h) * 0.40f;
            const float vigT = std::max(w, h) * 1.2f; // inner edge holeR, outer edge past the corners
            const int vigA = static_cast<int>(248.0f * overlayA);
            fg->AddCircle(c, holeR + vigT * 0.5f, IM_COL32(4, 4, 6, vigA), 96, vigT);
            fg->AddCircle(c, holeR, IM_COL32(0, 0, 0, static_cast<int>(255.0f * overlayA)), 96, 6.0f);
            fg->AddCircle(c, holeR - 5.0f, IM_COL32(85, 85, 95, static_cast<int>(110.0f * overlayA)), 96, 1.5f);

            const ImU32 lineCol = IM_COL32(10, 10, 12, static_cast<int>(220.0f * overlayA));
            fg->AddLine(ImVec2(c.x - holeR, c.y), ImVec2(c.x + holeR, c.y), lineCol, 1.5f);
            fg->AddLine(ImVec2(c.x, c.y - holeR), ImVec2(c.x, c.y + holeR), lineCol, 1.5f);
            for (int i = 1; i <= 3; ++i) // mil ticks down the lower post + across
            {
                const float d = holeR * 0.25f * static_cast<float>(i);
                const float tick = 7.0f;
                fg->AddLine(ImVec2(c.x - tick, c.y + d), ImVec2(c.x + tick, c.y + d), lineCol, 1.5f);
                fg->AddLine(ImVec2(c.x - d, c.y - tick), ImVec2(c.x - d, c.y + tick), lineCol, 1.5f);
                fg->AddLine(ImVec2(c.x + d, c.y - tick), ImVec2(c.x + d, c.y + tick), lineCol, 1.5f);
            }
        }

        // Sight-cycle toast (drawn from the hip too, so cycling gives feedback).
        if (m_clock < m_toastUntil && !m_toastText.empty())
        {
            const float ta = Clamp01(static_cast<float>((m_toastUntil - m_clock) / (0.5 * kToastSec)));
            const ImVec2 sz = ImGui::CalcTextSize(m_toastText.c_str());
            const ImVec2 at(c.x - sz.x * 0.5f, c.y + h * 0.12f);
            fg->AddText(ImVec2(at.x + 1.0f, at.y + 1.0f), IM_COL32(0, 0, 0, static_cast<int>(160.0f * ta)),
                        m_toastText.c_str());
            fg->AddText(at, IM_COL32(235, 235, 240, static_cast<int>(230.0f * ta)), m_toastText.c_str());
        }
#endif // SPARK_HAS_IMGUI
    }

} // namespace Terrafront
