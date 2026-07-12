/**
 * @file TFKeybinds.h
 * @brief Client-side action -> key table for the TERRAFRONT UI layer.
 *
 * OWNERSHIP: ui-map-keys lane (same agent as TFMapScreen/TFHUD/TFUiCommon).
 *
 * W7 (PlanetSide-spirit UI pass): one small table instead of VK constants
 * scattered per translation unit. Consumers:
 *   - TFMapScreen  — Action::OpenMap (M) toggles the map, Action::CloseOverlay
 *                    (Esc) closes it.
 *   - TFScoreboard — Action::HoldScoreboard (TAB, held).
 *   - TFHUD        — Action::FocusChat (Enter) opens the chat input. The chat
 *                    lane can rebind or hook this via BindKey (the seam below)
 *                    without touching HUD code.
 *
 * BindKey is the cross-lane seam: any system may rebind an action at runtime
 * (e.g. a future settings screen, or the chat lane moving chat focus off
 * Enter). Keys are Windows virtual-key codes, numeric so no <windows.h>
 * dependency here (TFClientNet.cpp convention: 'A'-'Z' work directly).
 *
 * Header-only: the table is an inline variable, one instance module-wide.
 */
#pragma once

#include "Input/InputManager.h"

#include <array>
#include <cstdint>

#ifdef SPARK_HAS_IMGUI
#include <imgui.h>
#endif

namespace Terrafront::TFKeys
{

    enum class Action : uint8_t
    {
        OpenMap = 0,      ///< toggle the fullscreen continent map (TFMapScreen)
        HoldScoreboard,   ///< hold-to-show scoreboard (TFScoreboard)
        FocusChat,        ///< focus the chat input line (TFHUD; chat-lane hook)
        CloseOverlay,     ///< close the topmost fullscreen overlay (map/chat)
        ToggleDirectives, ///< toggle the directives panel (TFDirectivePanel, W8 ui-polish)
        ThrowGrenade,     ///< throw a frag grenade (TFGrenadeSystem, W10)
        CycleMinimapZoom, ///< cycle the corner minimap's zoom span (TFHUDCombat, W13 minimap-v2)
        COUNT
    };

    // Named VK codes used by the defaults (numeric, no <windows.h>).
    inline constexpr int kVkTab = 0x09;
    inline constexpr int kVkEnter = 0x0D;
    inline constexpr int kVkEscape = 0x1B;

    namespace Detail
    {
        inline std::array<int, static_cast<size_t>(Action::COUNT)> g_binds = {
            'M',       // OpenMap
            kVkTab,    // HoldScoreboard
            kVkEnter,  // FocusChat
            kVkEscape, // CloseOverlay
            'J',       // ToggleDirectives (W8 ui-polish)
            'G',       // ThrowGrenade (W10 grenades)
            'N',       // CycleMinimapZoom (W13 minimap-v2)
        };
    } // namespace Detail

    /// Current VK code bound to `action` (0 when out of range — never matches).
    inline int KeyFor(Action action)
    {
        const auto i = static_cast<size_t>(action);
        return i < Detail::g_binds.size() ? Detail::g_binds[i] : 0;
    }

    /// Cross-lane rebind seam. No-op on out-of-range actions.
    inline void BindKey(Action action, int vk)
    {
        const auto i = static_cast<size_t>(action);
        if (i < Detail::g_binds.size())
            Detail::g_binds[i] = vk;
    }

    /// Edge-triggered press of the key bound to `action` (InputManager route).
    inline bool WasActionPressed(InputManager& input, Action action)
    {
        const int vk = KeyFor(action);
        return vk != 0 && input.WasKeyPressed(vk);
    }

    /// Level check of the key bound to `action` (hold-style, e.g. scoreboard).
    inline bool IsActionDown(InputManager& input, Action action)
    {
        const int vk = KeyFor(action);
        return vk != 0 && input.IsKeyDown(vk);
    }

#ifdef SPARK_HAS_IMGUI
    /// The bound key as an ImGuiKey for consumers that must respect ImGui's
    /// text-input gating (TFHUD chat). Covers letters, digits and the named
    /// keys the defaults use; anything else maps to ImGuiKey_None.
    inline ImGuiKey ImGuiKeyFor(Action action)
    {
        const int vk = KeyFor(action);
        if (vk >= 'A' && vk <= 'Z')
            return static_cast<ImGuiKey>(ImGuiKey_A + (vk - 'A'));
        if (vk >= '0' && vk <= '9')
            return static_cast<ImGuiKey>(ImGuiKey_0 + (vk - '0'));
        switch (vk)
        {
        case kVkEnter:
            return ImGuiKey_Enter;
        case kVkTab:
            return ImGuiKey_Tab;
        case kVkEscape:
            return ImGuiKey_Escape;
        case 0x20:
            return ImGuiKey_Space;
        default:
            return ImGuiKey_None;
        }
    }
#endif // SPARK_HAS_IMGUI

} // namespace Terrafront::TFKeys
