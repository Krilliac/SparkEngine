/**
 * @file Runtime.cpp
 * @brief Per-ImGuiContext Spark UI state, stored behind a Shutdown context hook.
 */

#include "SparkUI/Runtime.h"

#include "RuntimeState.h"
#include "SparkUI/Glyphs.h"
#include "SparkUI/Style.h"

#include <Spark/UiTheme.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include <imgui_internal.h>

namespace Spark::Ui
{
    namespace
    {
        ImGuiID StateOwnerId()
        {
            return ImHashStr("SparkUi.Runtime");
        }

        void DestroyState(ImGuiContext*, ImGuiContextHook* hook)
        {
            delete static_cast<Detail::RuntimeState*>(hook->UserData);
            hook->UserData = nullptr;
        }

        Detail::RuntimeState* FindState(ImGuiContext* ctx)
        {
            if (ctx == nullptr)
            {
                return nullptr;
            }
            const ImGuiID owner = StateOwnerId();
            for (ImGuiContextHook& hook : ctx->Hooks)
            {
                if (hook.Owner == owner && hook.Type == ImGuiContextHookType_Shutdown && hook.UserData != nullptr)
                {
                    return static_cast<Detail::RuntimeState*>(hook.UserData);
                }
            }
            return nullptr;
        }

        Detail::RuntimeState& EnsureState(ImGuiContext* ctx)
        {
            if (Detail::RuntimeState* existing = FindState(ctx))
            {
                return *existing;
            }
            auto* state = new Detail::RuntimeState();
            state->tokens = GenerateTokens(BuiltinSeed(PaletteId::Ember), Appearance::Dark);
            ImGuiContextHook hook;
            hook.Type = ImGuiContextHookType_Shutdown;
            hook.Owner = StateOwnerId();
            hook.Callback = &DestroyState;
            hook.UserData = state;
            ImGui::AddContextHook(ctx, &hook);
            return *state;
        }

        const Tokens& DefaultTokens()
        {
            // Immutable fallback answered before Install; it holds no per-context state.
            static const Tokens tokens = GenerateTokens(BuiltinSeed(PaletteId::Ember), Appearance::Dark);
            return tokens;
        }

        float SaneFactor(float value)
        {
            return std::isfinite(value) && value > 0.0f ? value : 1.0f;
        }

        void ApplyScaled(ImGuiContext& ctx, const Detail::RuntimeState& state)
        {
            const float effective = ClampScale(state.dpiScale, state.userScale);
            ImGuiStyle& style = ctx.Style;
            style.FontScaleDpi = state.dpiScale;
            style.FontScaleMain = effective / state.dpiScale;
            ApplyStyle(state.tokens, effective, style);
            ctx.IO.ConfigDpiScaleFonts = false;
        }

        Detail::RuntimeState* Current()
        {
            return FindState(ImGui::GetCurrentContext());
        }
    } // namespace

    namespace Detail
    {
        RuntimeState* CurrentState()
        {
            ImGuiContext* ctx = ImGui::GetCurrentContext();
            return ctx == nullptr ? nullptr : &EnsureState(ctx);
        }
    } // namespace Detail

    void Install(ImGuiContext* ctx, const Tokens& tokens, float dpiScale, float userScale, bool reduceMotion)
    {
        if (ctx == nullptr)
        {
            return;
        }
        Detail::RuntimeState& state = EnsureState(ctx);
        state.tokens = tokens;
        state.dpiScale = SaneFactor(dpiScale);
        state.userScale = SaneFactor(userScale);
        state.hasPendingScale = false;
        state.reduceMotion = reduceMotion;
        ApplyScaled(*ctx, state);
    }

    void RequestScale(float dpiScale, float userScale)
    {
        if (Detail::RuntimeState* state = Detail::CurrentState())
        {
            state->pendingDpiScale = SaneFactor(dpiScale);
            state->pendingUserScale = SaneFactor(userScale);
            state->hasPendingScale = true;
        }
    }

    bool BeginFrameScale()
    {
        ImGuiContext* ctx = ImGui::GetCurrentContext();
        Detail::RuntimeState* state = FindState(ctx);
        if (state == nullptr || !state->hasPendingScale)
        {
            return false;
        }
        state->dpiScale = state->pendingDpiScale;
        state->userScale = state->pendingUserScale;
        state->hasPendingScale = false;
        ApplyScaled(*ctx, *state);
        return true;
    }

    void SetTokens(const Tokens& tokens)
    {
        ImGuiContext* ctx = ImGui::GetCurrentContext();
        if (ctx == nullptr)
        {
            return;
        }
        Detail::RuntimeState& state = EnsureState(ctx);
        state.tokens = tokens;
        ApplyScaled(*ctx, state);
    }

    const Tokens& ActiveTokens()
    {
        const Detail::RuntimeState* state = Current();
        return state != nullptr ? state->tokens : DefaultTokens();
    }

    float Scale()
    {
        const Detail::RuntimeState* state = Current();
        return state != nullptr ? ClampScale(state->dpiScale, state->userScale) : 1.0f;
    }

    float DpiScale()
    {
        const Detail::RuntimeState* state = Current();
        return state != nullptr ? state->dpiScale : 1.0f;
    }

    float UserScale()
    {
        const Detail::RuntimeState* state = Current();
        return state != nullptr ? state->userScale : 1.0f;
    }

    float Px(float logical)
    {
        const float device = std::floor(logical * Scale());
        return logical > 0.0f ? std::max(1.0f, device) : device;
    }

    ImVec2 Px2(float x, float y)
    {
        return ImVec2(Px(x), Px(y));
    }

    float Em(float ems)
    {
        return ImGui::GetCurrentContext() != nullptr ? ems * ImGui::GetFontSize() : ems * ActiveTokens().type.body;
    }

    bool ReduceMotion()
    {
        const Detail::RuntimeState* state = Current();
        return state != nullptr && state->reduceMotion;
    }

    void SetReduceMotion(bool reduceMotion)
    {
        if (Detail::RuntimeState* state = Detail::CurrentState())
        {
            state->reduceMotion = reduceMotion;
        }
    }

    ImVec4 ColV(Role r, float alphaMul)
    {
        ImVec4 v = ToImVec4(Get(ActiveTokens(), r));
        v.w *= alphaMul;
        return v;
    }

    ImU32 Col(Role r, float alphaMul)
    {
        const ImVec4 v = ColV(r, alphaMul);
        return ImGui::GetCurrentContext() != nullptr ? ImGui::GetColorU32(v) : ImGui::ColorConvertFloat4ToU32(v);
    }

    Role StatusTextRole(Status s) noexcept
    {
        switch (s)
        {
        case Status::Info:
            return Role::InfoText;
        case Status::Success:
            return Role::SuccessText;
        case Status::Warning:
            return Role::WarningText;
        case Status::Error:
            return Role::ErrorText;
        case Status::Neutral:
            break;
        }
        return Role::TextSecondary;
    }

    Role StatusFillRole(Status s) noexcept
    {
        switch (s)
        {
        case Status::Info:
            return Role::InfoFill;
        case Status::Success:
            return Role::SuccessFill;
        case Status::Warning:
            return Role::WarningFill;
        case Status::Error:
            return Role::ErrorFill;
        case Status::Neutral:
            break;
        }
        return Role::SurfaceRaised;
    }

    const char* StatusGlyph(Status s) noexcept
    {
        switch (s)
        {
        case Status::Info:
            return Glyph::kCircleInfo;
        case Status::Success:
            return Glyph::kCircleCheck;
        case Status::Warning:
            return Glyph::kTriangleExclamation;
        case Status::Error:
            return Glyph::kCircleXmark;
        case Status::Neutral:
            break;
        }
        return Glyph::kCircleQuestion;
    }

    const char* StatusWord(Status s) noexcept
    {
        switch (s)
        {
        case Status::Info:
            return "Info";
        case Status::Success:
            return "Success";
        case Status::Warning:
            return "Warning";
        case Status::Error:
            return "Error";
        case Status::Neutral:
            break;
        }
        return "Note";
    }

    ImU32 CategoryColor(int index)
    {
        constexpr int kCategories = 8;
        const int wrapped = ((index % kCategories) + kCategories) % kCategories;
        return Col(static_cast<Role>(static_cast<int>(Role::Category1) + wrapped));
    }

    void FillSdkRuntimeBlock(Sdk::SparkUiRuntimeBlock& out)
    {
        const Tokens& t = ActiveTokens();
        const auto pack = [&](Role r) { return ImGui::ColorConvertFloat4ToU32(ToImVec4(Get(t, r))); };
        out = Sdk::SparkUiRuntimeBlock{};
        out.magic = Sdk::kRuntimeBlockMagic;
        out.version = Sdk::kRuntimeBlockVersion;
        out.size = static_cast<std::uint16_t>(sizeof(Sdk::SparkUiRuntimeBlock));
        const auto set = [&](Sdk::UiRole role, Role source)
        { out.colors[static_cast<std::size_t>(role)] = pack(source); };
        set(Sdk::UiRole::Text, Role::TextPrimary);
        set(Sdk::UiRole::TextSecondary, Role::TextSecondary);
        set(Sdk::UiRole::Accent, Role::AccentFill);
        set(Sdk::UiRole::TextOnAccent, Role::TextOnAccent);
        set(Sdk::UiRole::Success, Role::SuccessText);
        set(Sdk::UiRole::Warning, Role::WarningText);
        set(Sdk::UiRole::Error, Role::ErrorText);
        set(Sdk::UiRole::Info, Role::InfoText);
        set(Sdk::UiRole::SurfaceOverlay, Role::SurfaceOverlay);
        out.scale = Scale();
        out.fontBody = t.type.body;
        out.fontSmall = t.type.small;
        out.fontH1 = t.type.h1;
        out.reduceMotion = ReduceMotion() ? 1 : 0;
        out.highContrast = t.appearance == Appearance::HighContrast ? 1 : 0;
    }
} // namespace Spark::Ui
