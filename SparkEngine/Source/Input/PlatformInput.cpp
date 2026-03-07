#include "../Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS
/**
 * @file PlatformInput.cpp
 * @brief Cross-platform input abstraction implementation
 */

#include "PlatformInput.h"
#include <sstream>
#include <algorithm>
#include <cmath>
#include <cstring>

#ifdef _WIN32
#ifdef SPARK_PLATFORM_WINDOWS
#include <Windows.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <Xinput.h>
#pragma comment(lib, "xinput.lib")
#endif

#ifdef SPARK_SDL2_AVAILABLE
#include <SDL.h>
#endif

namespace Spark::Input {

// ============================================================================
// Key Name Mapping
// ============================================================================

static const std::unordered_map<KeyCode, std::string>& GetKeyNames() {
    static std::unordered_map<KeyCode, std::string> names = {
        {KeyCode::A, "A"}, {KeyCode::B, "B"}, {KeyCode::C, "C"}, {KeyCode::D, "D"},
        {KeyCode::E, "E"}, {KeyCode::F, "F"}, {KeyCode::G, "G"}, {KeyCode::H, "H"},
        {KeyCode::I, "I"}, {KeyCode::J, "J"}, {KeyCode::K, "K"}, {KeyCode::L, "L"},
        {KeyCode::M, "M"}, {KeyCode::N, "N"}, {KeyCode::O, "O"}, {KeyCode::P, "P"},
        {KeyCode::Q, "Q"}, {KeyCode::R, "R"}, {KeyCode::S, "S"}, {KeyCode::T, "T"},
        {KeyCode::U, "U"}, {KeyCode::V, "V"}, {KeyCode::W, "W"}, {KeyCode::X, "X"},
        {KeyCode::Y, "Y"}, {KeyCode::Z, "Z"},
        {KeyCode::Num0, "0"}, {KeyCode::Num1, "1"}, {KeyCode::Num2, "2"},
        {KeyCode::Num3, "3"}, {KeyCode::Num4, "4"}, {KeyCode::Num5, "5"},
        {KeyCode::Num6, "6"}, {KeyCode::Num7, "7"}, {KeyCode::Num8, "8"},
        {KeyCode::Num9, "9"},
        {KeyCode::F1, "F1"}, {KeyCode::F2, "F2"}, {KeyCode::F3, "F3"},
        {KeyCode::F4, "F4"}, {KeyCode::F5, "F5"}, {KeyCode::F6, "F6"},
        {KeyCode::F7, "F7"}, {KeyCode::F8, "F8"}, {KeyCode::F9, "F9"},
        {KeyCode::F10, "F10"}, {KeyCode::F11, "F11"}, {KeyCode::F12, "F12"},
        {KeyCode::Escape, "Escape"}, {KeyCode::Enter, "Enter"}, {KeyCode::Tab, "Tab"},
        {KeyCode::Backspace, "Backspace"}, {KeyCode::Space, "Space"},
        {KeyCode::LeftShift, "LeftShift"}, {KeyCode::RightShift, "RightShift"},
        {KeyCode::LeftCtrl, "LeftCtrl"}, {KeyCode::RightCtrl, "RightCtrl"},
        {KeyCode::LeftAlt, "LeftAlt"}, {KeyCode::RightAlt, "RightAlt"},
        {KeyCode::Up, "Up"}, {KeyCode::Down, "Down"},
        {KeyCode::Left, "Left"}, {KeyCode::Right, "Right"},
        {KeyCode::Insert, "Insert"}, {KeyCode::Delete, "Delete"},
        {KeyCode::Home, "Home"}, {KeyCode::End, "End"},
        {KeyCode::PageUp, "PageUp"}, {KeyCode::PageDown, "PageDown"},
        {KeyCode::MouseLeft, "MouseLeft"}, {KeyCode::MouseRight, "MouseRight"},
        {KeyCode::MouseMiddle, "MouseMiddle"},
    };
    return names;
}

// ============================================================================
// Win32 Input Backend
// ============================================================================

#ifdef _WIN32

bool Win32InputBackend::Initialize(void* windowHandle) {
    m_windowHandle = windowHandle;
    m_keyStates.fill(false);
    m_prevKeyStates.fill(false);
    return true;
}

void Win32InputBackend::Shutdown() {
    if (m_mouseCaptured) {
        SetMouseCapture(false);
    }
}

void Win32InputBackend::PollEvents() {
    // Save previous states
    m_prevKeyStates = m_keyStates;
    for (auto& gp : m_gamepads) {
        gp.prevButtons = gp.buttons;
    }

    // Reset per-frame values
    m_mouseDeltaX = 0;
    m_mouseDeltaY = 0;
    m_mouseScroll = 0.0f;

    // Poll XInput gamepads
    for (int i = 0; i < MAX_GAMEPADS; ++i) {
        XINPUT_STATE state{};
        DWORD result = XInputGetState(i, &state);

        m_gamepads[i].connected = (result == ERROR_SUCCESS);
        if (!m_gamepads[i].connected) continue;

        m_gamepads[i].name = "XInput Controller " + std::to_string(i);

        // Axes (normalized -1 to 1)
        auto normalize = [](short raw) -> float {
            return static_cast<float>(raw) / 32767.0f;
        };
        m_gamepads[i].axes[static_cast<size_t>(GamepadAxis::LeftStickX)] = normalize(state.Gamepad.sThumbLX);
        m_gamepads[i].axes[static_cast<size_t>(GamepadAxis::LeftStickY)] = normalize(state.Gamepad.sThumbLY);
        m_gamepads[i].axes[static_cast<size_t>(GamepadAxis::RightStickX)] = normalize(state.Gamepad.sThumbRX);
        m_gamepads[i].axes[static_cast<size_t>(GamepadAxis::RightStickY)] = normalize(state.Gamepad.sThumbRY);
        m_gamepads[i].axes[static_cast<size_t>(GamepadAxis::LeftTrigger)] = state.Gamepad.bLeftTrigger / 255.0f;
        m_gamepads[i].axes[static_cast<size_t>(GamepadAxis::RightTrigger)] = state.Gamepad.bRightTrigger / 255.0f;

        // Buttons
        auto checkBtn = [&](GamepadBtn btn, WORD xinputFlag) {
            m_gamepads[i].buttons[static_cast<size_t>(btn)] = (state.Gamepad.wButtons & xinputFlag) != 0;
        };
        checkBtn(GamepadBtn::A, XINPUT_GAMEPAD_A);
        checkBtn(GamepadBtn::B, XINPUT_GAMEPAD_B);
        checkBtn(GamepadBtn::X, XINPUT_GAMEPAD_X);
        checkBtn(GamepadBtn::Y, XINPUT_GAMEPAD_Y);
        checkBtn(GamepadBtn::LeftBumper, XINPUT_GAMEPAD_LEFT_SHOULDER);
        checkBtn(GamepadBtn::RightBumper, XINPUT_GAMEPAD_RIGHT_SHOULDER);
        checkBtn(GamepadBtn::Back, XINPUT_GAMEPAD_BACK);
        checkBtn(GamepadBtn::Start, XINPUT_GAMEPAD_START);
        checkBtn(GamepadBtn::LeftStick, XINPUT_GAMEPAD_LEFT_THUMB);
        checkBtn(GamepadBtn::RightStick, XINPUT_GAMEPAD_RIGHT_THUMB);
        checkBtn(GamepadBtn::DPadUp, XINPUT_GAMEPAD_DPAD_UP);
        checkBtn(GamepadBtn::DPadDown, XINPUT_GAMEPAD_DPAD_DOWN);
        checkBtn(GamepadBtn::DPadLeft, XINPUT_GAMEPAD_DPAD_LEFT);
        checkBtn(GamepadBtn::DPadRight, XINPUT_GAMEPAD_DPAD_RIGHT);
    }
}

bool Win32InputBackend::IsKeyDown(KeyCode key) const {
    auto idx = static_cast<size_t>(key);
    if (idx >= m_keyStates.size()) return false;
    return m_keyStates[idx];
}

bool Win32InputBackend::WasKeyPressed(KeyCode key) const {
    auto idx = static_cast<size_t>(key);
    if (idx >= m_keyStates.size()) return false;
    return m_keyStates[idx] && !m_prevKeyStates[idx];
}

bool Win32InputBackend::WasKeyReleased(KeyCode key) const {
    auto idx = static_cast<size_t>(key);
    if (idx >= m_keyStates.size()) return false;
    return !m_keyStates[idx] && m_prevKeyStates[idx];
}

void Win32InputBackend::GetMousePosition(int& x, int& y) const {
    x = m_mouseX;
    y = m_mouseY;
}

void Win32InputBackend::GetMouseDelta(int& dx, int& dy) const {
    dx = m_mouseDeltaX;
    dy = m_mouseDeltaY;
}

float Win32InputBackend::GetMouseScroll() const {
    return m_mouseScroll;
}

void Win32InputBackend::SetMouseCapture(bool capture) {
    m_mouseCaptured = capture;
    HWND hwnd = static_cast<HWND>(m_windowHandle);
    if (capture) {
        SetCapture(hwnd);
        ShowCursor(FALSE);
    } else {
        ReleaseCapture();
        ShowCursor(TRUE);
    }
}

bool Win32InputBackend::IsMouseCaptured() const {
    return m_mouseCaptured;
}

int Win32InputBackend::GetGamepadCount() const {
    int count = 0;
    for (const auto& gp : m_gamepads) {
        if (gp.connected) count++;
    }
    return count;
}

const GamepadState* Win32InputBackend::GetGamepadState(int index) const {
    if (index < 0 || index >= MAX_GAMEPADS) return nullptr;
    return &m_gamepads[index];
}

void Win32InputBackend::SetVibration(int gamepadIndex, float leftMotor, float rightMotor, float /*duration*/) {
    if (gamepadIndex < 0 || gamepadIndex >= MAX_GAMEPADS) return;
    XINPUT_VIBRATION vibration{};
    vibration.wLeftMotorSpeed = static_cast<WORD>(leftMotor * 65535.0f);
    vibration.wRightMotorSpeed = static_cast<WORD>(rightMotor * 65535.0f);
    XInputSetState(gamepadIndex, &vibration);
}

void Win32InputBackend::HandleWindowMessage(uint32_t message, uintptr_t wParam, intptr_t lParam) {
    switch (message) {
        case WM_KEYDOWN: {
            KeyCode kc = VirtualKeyToKeyCode(static_cast<int>(wParam));
            if (kc != KeyCode::Unknown) {
                m_keyStates[static_cast<size_t>(kc)] = true;
            }
            break;
        }
        case WM_KEYUP: {
            KeyCode kc = VirtualKeyToKeyCode(static_cast<int>(wParam));
            if (kc != KeyCode::Unknown) {
                m_keyStates[static_cast<size_t>(kc)] = false;
            }
            break;
        }
        case WM_LBUTTONDOWN:
            m_keyStates[static_cast<size_t>(KeyCode::MouseLeft)] = true;
            break;
        case WM_LBUTTONUP:
            m_keyStates[static_cast<size_t>(KeyCode::MouseLeft)] = false;
            break;
        case WM_RBUTTONDOWN:
            m_keyStates[static_cast<size_t>(KeyCode::MouseRight)] = true;
            break;
        case WM_RBUTTONUP:
            m_keyStates[static_cast<size_t>(KeyCode::MouseRight)] = false;
            break;
        case WM_MBUTTONDOWN:
            m_keyStates[static_cast<size_t>(KeyCode::MouseMiddle)] = true;
            break;
        case WM_MBUTTONUP:
            m_keyStates[static_cast<size_t>(KeyCode::MouseMiddle)] = false;
            break;
        case WM_MOUSEMOVE: {
            int newX = static_cast<int>(lParam & 0xFFFF);
            int newY = static_cast<int>((lParam >> 16) & 0xFFFF);
            m_mouseDeltaX = newX - m_mouseX;
            m_mouseDeltaY = newY - m_mouseY;
            m_mouseX = newX;
            m_mouseY = newY;
            break;
        }
        case WM_MOUSEWHEEL:
            m_mouseScroll = static_cast<float>(static_cast<short>(HIWORD(wParam))) / 120.0f;
            break;
    }
}

KeyCode Win32InputBackend::VirtualKeyToKeyCode(int vk) const {
    // Letters and numbers map directly
    if (vk >= 'A' && vk <= 'Z') return static_cast<KeyCode>(vk);
    if (vk >= '0' && vk <= '9') return static_cast<KeyCode>(vk);

    switch (vk) {
        case VK_F1: return KeyCode::F1;     case VK_F2: return KeyCode::F2;
        case VK_F3: return KeyCode::F3;     case VK_F4: return KeyCode::F4;
        case VK_F5: return KeyCode::F5;     case VK_F6: return KeyCode::F6;
        case VK_F7: return KeyCode::F7;     case VK_F8: return KeyCode::F8;
        case VK_F9: return KeyCode::F9;     case VK_F10: return KeyCode::F10;
        case VK_F11: return KeyCode::F11;   case VK_F12: return KeyCode::F12;
        case VK_ESCAPE: return KeyCode::Escape;
        case VK_RETURN: return KeyCode::Enter;
        case VK_TAB: return KeyCode::Tab;
        case VK_BACK: return KeyCode::Backspace;
        case VK_INSERT: return KeyCode::Insert;
        case VK_DELETE: return KeyCode::Delete;
        case VK_RIGHT: return KeyCode::Right;
        case VK_LEFT: return KeyCode::Left;
        case VK_DOWN: return KeyCode::Down;
        case VK_UP: return KeyCode::Up;
        case VK_PRIOR: return KeyCode::PageUp;
        case VK_NEXT: return KeyCode::PageDown;
        case VK_HOME: return KeyCode::Home;
        case VK_END: return KeyCode::End;
        case VK_LSHIFT: return KeyCode::LeftShift;
        case VK_RSHIFT: return KeyCode::RightShift;
        case VK_LCONTROL: return KeyCode::LeftCtrl;
        case VK_RCONTROL: return KeyCode::RightCtrl;
        case VK_LMENU: return KeyCode::LeftAlt;
        case VK_RMENU: return KeyCode::RightAlt;
        case VK_SPACE: return KeyCode::Space;
        case VK_NUMPAD0: return KeyCode::Numpad0;
        case VK_NUMPAD1: return KeyCode::Numpad1;
        case VK_NUMPAD2: return KeyCode::Numpad2;
        case VK_NUMPAD3: return KeyCode::Numpad3;
        case VK_NUMPAD4: return KeyCode::Numpad4;
        case VK_NUMPAD5: return KeyCode::Numpad5;
        case VK_NUMPAD6: return KeyCode::Numpad6;
        case VK_NUMPAD7: return KeyCode::Numpad7;
        case VK_NUMPAD8: return KeyCode::Numpad8;
        case VK_NUMPAD9: return KeyCode::Numpad9;
        case VK_DECIMAL: return KeyCode::NumpadDecimal;
        case VK_DIVIDE: return KeyCode::NumpadDivide;
        case VK_MULTIPLY: return KeyCode::NumpadMultiply;
        case VK_SUBTRACT: return KeyCode::NumpadSubtract;
        case VK_ADD: return KeyCode::NumpadAdd;
        default: return KeyCode::Unknown;
    }
}

#endif // _WIN32

// ============================================================================
// SDL2 Input Backend
// ============================================================================

#ifdef SPARK_SDL2_AVAILABLE

bool SDL2InputBackend::Initialize(void* windowHandle) {
    if (SDL_Init(SDL_INIT_GAMECONTROLLER | SDL_INIT_HAPTIC) < 0) {
        return false;
    }
    m_window = windowHandle;
    m_keyStates.fill(false);
    m_prevKeyStates.fill(false);
    m_sdlControllers.fill(nullptr);

    // Open any connected game controllers
    for (int i = 0; i < SDL_NumJoysticks() && i < MAX_GAMEPADS; ++i) {
        if (SDL_IsGameController(i)) {
            m_sdlControllers[i] = SDL_GameControllerOpen(i);
            m_gamepads[i].connected = (m_sdlControllers[i] != nullptr);
            if (m_gamepads[i].connected) {
                m_gamepads[i].name = SDL_GameControllerName(
                    static_cast<SDL_GameController*>(m_sdlControllers[i]));
            }
        }
    }
    return true;
}

void SDL2InputBackend::Shutdown() {
    for (int i = 0; i < MAX_GAMEPADS; ++i) {
        if (m_sdlControllers[i]) {
            SDL_GameControllerClose(static_cast<SDL_GameController*>(m_sdlControllers[i]));
            m_sdlControllers[i] = nullptr;
        }
    }
    SDL_Quit();
}

void SDL2InputBackend::PollEvents() {
    m_prevKeyStates = m_keyStates;
    for (auto& gp : m_gamepads) gp.prevButtons = gp.buttons;

    m_mouseDeltaX = 0;
    m_mouseDeltaY = 0;
    m_mouseScroll = 0.0f;

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_KEYDOWN: {
                KeyCode kc = SDLKeyToKeyCode(event.key.keysym.sym);
                if (kc != KeyCode::Unknown)
                    m_keyStates[static_cast<size_t>(kc)] = true;
                break;
            }
            case SDL_KEYUP: {
                KeyCode kc = SDLKeyToKeyCode(event.key.keysym.sym);
                if (kc != KeyCode::Unknown)
                    m_keyStates[static_cast<size_t>(kc)] = false;
                break;
            }
            case SDL_MOUSEMOTION:
                m_mouseX = event.motion.x;
                m_mouseY = event.motion.y;
                m_mouseDeltaX = event.motion.xrel;
                m_mouseDeltaY = event.motion.yrel;
                break;
            case SDL_MOUSEBUTTONDOWN:
                if (event.button.button == SDL_BUTTON_LEFT)
                    m_keyStates[static_cast<size_t>(KeyCode::MouseLeft)] = true;
                else if (event.button.button == SDL_BUTTON_RIGHT)
                    m_keyStates[static_cast<size_t>(KeyCode::MouseRight)] = true;
                else if (event.button.button == SDL_BUTTON_MIDDLE)
                    m_keyStates[static_cast<size_t>(KeyCode::MouseMiddle)] = true;
                break;
            case SDL_MOUSEBUTTONUP:
                if (event.button.button == SDL_BUTTON_LEFT)
                    m_keyStates[static_cast<size_t>(KeyCode::MouseLeft)] = false;
                else if (event.button.button == SDL_BUTTON_RIGHT)
                    m_keyStates[static_cast<size_t>(KeyCode::MouseRight)] = false;
                else if (event.button.button == SDL_BUTTON_MIDDLE)
                    m_keyStates[static_cast<size_t>(KeyCode::MouseMiddle)] = false;
                break;
            case SDL_MOUSEWHEEL:
                m_mouseScroll = static_cast<float>(event.wheel.y);
                break;
            case SDL_CONTROLLERDEVICEADDED: {
                int idx = event.cdevice.which;
                if (idx < MAX_GAMEPADS) {
                    m_sdlControllers[idx] = SDL_GameControllerOpen(idx);
                    m_gamepads[idx].connected = (m_sdlControllers[idx] != nullptr);
                }
                break;
            }
            case SDL_CONTROLLERDEVICEREMOVED: {
                for (int i = 0; i < MAX_GAMEPADS; ++i) {
                    if (m_sdlControllers[i] && !SDL_GameControllerGetAttached(
                            static_cast<SDL_GameController*>(m_sdlControllers[i]))) {
                        SDL_GameControllerClose(static_cast<SDL_GameController*>(m_sdlControllers[i]));
                        m_sdlControllers[i] = nullptr;
                        m_gamepads[i].connected = false;
                    }
                }
                break;
            }
        }
    }

    // Update gamepad axes and buttons
    for (int i = 0; i < MAX_GAMEPADS; ++i) {
        if (!m_sdlControllers[i]) continue;
        auto* ctrl = static_cast<SDL_GameController*>(m_sdlControllers[i]);

        auto readAxis = [&](SDL_GameControllerAxis sdlAxis) -> float {
            return static_cast<float>(SDL_GameControllerGetAxis(ctrl, sdlAxis)) / 32767.0f;
        };

        m_gamepads[i].axes[static_cast<size_t>(GamepadAxis::LeftStickX)] = readAxis(SDL_CONTROLLER_AXIS_LEFTX);
        m_gamepads[i].axes[static_cast<size_t>(GamepadAxis::LeftStickY)] = readAxis(SDL_CONTROLLER_AXIS_LEFTY);
        m_gamepads[i].axes[static_cast<size_t>(GamepadAxis::RightStickX)] = readAxis(SDL_CONTROLLER_AXIS_RIGHTX);
        m_gamepads[i].axes[static_cast<size_t>(GamepadAxis::RightStickY)] = readAxis(SDL_CONTROLLER_AXIS_RIGHTY);
        m_gamepads[i].axes[static_cast<size_t>(GamepadAxis::LeftTrigger)] = readAxis(SDL_CONTROLLER_AXIS_TRIGGERLEFT);
        m_gamepads[i].axes[static_cast<size_t>(GamepadAxis::RightTrigger)] = readAxis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT);

        auto readBtn = [&](SDL_GameControllerButton sdlBtn) -> bool {
            return SDL_GameControllerGetButton(ctrl, sdlBtn) != 0;
        };

        m_gamepads[i].buttons[static_cast<size_t>(GamepadBtn::A)] = readBtn(SDL_CONTROLLER_BUTTON_A);
        m_gamepads[i].buttons[static_cast<size_t>(GamepadBtn::B)] = readBtn(SDL_CONTROLLER_BUTTON_B);
        m_gamepads[i].buttons[static_cast<size_t>(GamepadBtn::X)] = readBtn(SDL_CONTROLLER_BUTTON_X);
        m_gamepads[i].buttons[static_cast<size_t>(GamepadBtn::Y)] = readBtn(SDL_CONTROLLER_BUTTON_Y);
        m_gamepads[i].buttons[static_cast<size_t>(GamepadBtn::LeftBumper)] = readBtn(SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
        m_gamepads[i].buttons[static_cast<size_t>(GamepadBtn::RightBumper)] = readBtn(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
        m_gamepads[i].buttons[static_cast<size_t>(GamepadBtn::Back)] = readBtn(SDL_CONTROLLER_BUTTON_BACK);
        m_gamepads[i].buttons[static_cast<size_t>(GamepadBtn::Start)] = readBtn(SDL_CONTROLLER_BUTTON_START);
        m_gamepads[i].buttons[static_cast<size_t>(GamepadBtn::Guide)] = readBtn(SDL_CONTROLLER_BUTTON_GUIDE);
        m_gamepads[i].buttons[static_cast<size_t>(GamepadBtn::LeftStick)] = readBtn(SDL_CONTROLLER_BUTTON_LEFTSTICK);
        m_gamepads[i].buttons[static_cast<size_t>(GamepadBtn::RightStick)] = readBtn(SDL_CONTROLLER_BUTTON_RIGHTSTICK);
        m_gamepads[i].buttons[static_cast<size_t>(GamepadBtn::DPadUp)] = readBtn(SDL_CONTROLLER_BUTTON_DPAD_UP);
        m_gamepads[i].buttons[static_cast<size_t>(GamepadBtn::DPadDown)] = readBtn(SDL_CONTROLLER_BUTTON_DPAD_DOWN);
        m_gamepads[i].buttons[static_cast<size_t>(GamepadBtn::DPadLeft)] = readBtn(SDL_CONTROLLER_BUTTON_DPAD_LEFT);
        m_gamepads[i].buttons[static_cast<size_t>(GamepadBtn::DPadRight)] = readBtn(SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
    }
}

bool SDL2InputBackend::IsKeyDown(KeyCode key) const {
    auto idx = static_cast<size_t>(key);
    return (idx < m_keyStates.size()) ? m_keyStates[idx] : false;
}

bool SDL2InputBackend::WasKeyPressed(KeyCode key) const {
    auto idx = static_cast<size_t>(key);
    return (idx < m_keyStates.size()) ? (m_keyStates[idx] && !m_prevKeyStates[idx]) : false;
}

bool SDL2InputBackend::WasKeyReleased(KeyCode key) const {
    auto idx = static_cast<size_t>(key);
    return (idx < m_keyStates.size()) ? (!m_keyStates[idx] && m_prevKeyStates[idx]) : false;
}

void SDL2InputBackend::GetMousePosition(int& x, int& y) const {
    x = m_mouseX; y = m_mouseY;
}

void SDL2InputBackend::GetMouseDelta(int& dx, int& dy) const {
    dx = m_mouseDeltaX; dy = m_mouseDeltaY;
}

float SDL2InputBackend::GetMouseScroll() const { return m_mouseScroll; }

void SDL2InputBackend::SetMouseCapture(bool capture) {
    m_mouseCaptured = capture;
    SDL_SetRelativeMouseMode(capture ? SDL_TRUE : SDL_FALSE);
}

bool SDL2InputBackend::IsMouseCaptured() const { return m_mouseCaptured; }

int SDL2InputBackend::GetGamepadCount() const {
    int count = 0;
    for (const auto& gp : m_gamepads) if (gp.connected) count++;
    return count;
}

const GamepadState* SDL2InputBackend::GetGamepadState(int index) const {
    if (index < 0 || index >= MAX_GAMEPADS) return nullptr;
    return &m_gamepads[index];
}

void SDL2InputBackend::SetVibration(int gamepadIndex, float leftMotor, float rightMotor, float /*duration*/) {
    if (gamepadIndex < 0 || gamepadIndex >= MAX_GAMEPADS || !m_sdlControllers[gamepadIndex]) return;
    SDL_GameControllerRumble(
        static_cast<SDL_GameController*>(m_sdlControllers[gamepadIndex]),
        static_cast<Uint16>(leftMotor * 65535.0f),
        static_cast<Uint16>(rightMotor * 65535.0f),
        100  // duration in ms
    );
}

KeyCode SDL2InputBackend::SDLKeyToKeyCode(int sdlKey) const {
    if (sdlKey >= SDLK_a && sdlKey <= SDLK_z) return static_cast<KeyCode>('A' + (sdlKey - SDLK_a));
    if (sdlKey >= SDLK_0 && sdlKey <= SDLK_9) return static_cast<KeyCode>('0' + (sdlKey - SDLK_0));

    switch (sdlKey) {
        case SDLK_F1: return KeyCode::F1;      case SDLK_F2: return KeyCode::F2;
        case SDLK_F3: return KeyCode::F3;      case SDLK_F4: return KeyCode::F4;
        case SDLK_F5: return KeyCode::F5;      case SDLK_F6: return KeyCode::F6;
        case SDLK_F7: return KeyCode::F7;      case SDLK_F8: return KeyCode::F8;
        case SDLK_F9: return KeyCode::F9;      case SDLK_F10: return KeyCode::F10;
        case SDLK_F11: return KeyCode::F11;    case SDLK_F12: return KeyCode::F12;
        case SDLK_ESCAPE: return KeyCode::Escape;
        case SDLK_RETURN: return KeyCode::Enter;
        case SDLK_TAB: return KeyCode::Tab;
        case SDLK_BACKSPACE: return KeyCode::Backspace;
        case SDLK_INSERT: return KeyCode::Insert;
        case SDLK_DELETE: return KeyCode::Delete;
        case SDLK_RIGHT: return KeyCode::Right;
        case SDLK_LEFT: return KeyCode::Left;
        case SDLK_DOWN: return KeyCode::Down;
        case SDLK_UP: return KeyCode::Up;
        case SDLK_PAGEUP: return KeyCode::PageUp;
        case SDLK_PAGEDOWN: return KeyCode::PageDown;
        case SDLK_HOME: return KeyCode::Home;
        case SDLK_END: return KeyCode::End;
        case SDLK_LSHIFT: return KeyCode::LeftShift;
        case SDLK_RSHIFT: return KeyCode::RightShift;
        case SDLK_LCTRL: return KeyCode::LeftCtrl;
        case SDLK_RCTRL: return KeyCode::RightCtrl;
        case SDLK_LALT: return KeyCode::LeftAlt;
        case SDLK_RALT: return KeyCode::RightAlt;
        case SDLK_SPACE: return KeyCode::Space;
        default: return KeyCode::Unknown;
    }
}

#endif // SPARK_SDL2_AVAILABLE

// ============================================================================
// PlatformInputManager
// ============================================================================

PlatformInputManager& PlatformInputManager::GetInstance() {
    static PlatformInputManager instance;
    return instance;
}

bool PlatformInputManager::Initialize(void* windowHandle, const std::string& preferredBackend) {
#ifdef SPARK_SDL2_AVAILABLE
    if (preferredBackend == "sdl2" || preferredBackend == "SDL2") {
        m_backend = std::make_unique<SDL2InputBackend>();
        return m_backend->Initialize(windowHandle);
    }
#endif

#ifdef _WIN32
    if (preferredBackend == "auto" || preferredBackend == "win32") {
        m_backend = std::make_unique<Win32InputBackend>();
        return m_backend->Initialize(windowHandle);
    }
#endif

    // Fallback: try platform default
#ifdef _WIN32
    m_backend = std::make_unique<Win32InputBackend>();
#elif defined(SPARK_SDL2_AVAILABLE)
    m_backend = std::make_unique<SDL2InputBackend>();
#endif

    return m_backend ? m_backend->Initialize(windowHandle) : false;
}

void PlatformInputManager::Shutdown() {
    if (m_backend) m_backend->Shutdown();
    m_backend.reset();
}

void PlatformInputManager::Update() {
    if (!m_backend) return;
    m_backend->PollEvents();
}

bool PlatformInputManager::IsKeyDown(KeyCode key) const {
    return m_backend ? m_backend->IsKeyDown(key) : false;
}

bool PlatformInputManager::WasKeyPressed(KeyCode key) const {
    return m_backend ? m_backend->WasKeyPressed(key) : false;
}

bool PlatformInputManager::WasKeyReleased(KeyCode key) const {
    return m_backend ? m_backend->WasKeyReleased(key) : false;
}

void PlatformInputManager::GetMousePosition(int& x, int& y) const {
    if (m_backend) m_backend->GetMousePosition(x, y);
    else { x = 0; y = 0; }
}

void PlatformInputManager::GetMouseDelta(int& dx, int& dy) const {
    if (m_backend) {
        m_backend->GetMouseDelta(dx, dy);
        dx = static_cast<int>(dx * m_globalSensitivity);
        dy = static_cast<int>(dy * m_globalSensitivity);
    } else {
        dx = 0; dy = 0;
    }
}

float PlatformInputManager::GetMouseScroll() const {
    return m_backend ? m_backend->GetMouseScroll() : 0.0f;
}

void PlatformInputManager::SetMouseCapture(bool capture) {
    if (m_backend) m_backend->SetMouseCapture(capture);
}

bool PlatformInputManager::IsMouseCaptured() const {
    return m_backend ? m_backend->IsMouseCaptured() : false;
}

int PlatformInputManager::GetGamepadCount() const {
    return m_backend ? m_backend->GetGamepadCount() : 0;
}

bool PlatformInputManager::IsGamepadConnected(int index) const {
    if (!m_backend) return false;
    auto* state = m_backend->GetGamepadState(index);
    return state && state->connected;
}

float PlatformInputManager::GetGamepadAxis(GamepadAxis axis, int index) const {
    if (!m_backend) return 0.0f;
    auto* state = m_backend->GetGamepadState(index);
    if (!state || !state->connected) return 0.0f;
    float val = state->axes[static_cast<size_t>(axis)];
    // Apply dead zone
    if (std::abs(val) < m_globalDeadZone) return 0.0f;
    return val;
}

bool PlatformInputManager::IsGamepadButtonDown(GamepadBtn button, int index) const {
    if (!m_backend) return false;
    auto* state = m_backend->GetGamepadState(index);
    if (!state || !state->connected) return false;
    return state->buttons[static_cast<size_t>(button)];
}

bool PlatformInputManager::WasGamepadButtonPressed(GamepadBtn button, int index) const {
    if (!m_backend) return false;
    auto* state = m_backend->GetGamepadState(index);
    if (!state || !state->connected) return false;
    return state->buttons[static_cast<size_t>(button)] && !state->prevButtons[static_cast<size_t>(button)];
}

bool PlatformInputManager::WasGamepadButtonReleased(GamepadBtn button, int index) const {
    if (!m_backend) return false;
    auto* state = m_backend->GetGamepadState(index);
    if (!state || !state->connected) return false;
    return !state->buttons[static_cast<size_t>(button)] && state->prevButtons[static_cast<size_t>(button)];
}

void PlatformInputManager::SetVibration(int index, float leftMotor, float rightMotor, float duration) {
    if (m_backend) m_backend->SetVibration(index, leftMotor, rightMotor, duration);
}

// ===== Action Mapping =====

void PlatformInputManager::BindAction(const std::string& name, KeyCode key, ActionTrigger trigger) {
    ActionBinding binding;
    binding.actionName = name;
    binding.key = key;
    binding.useGamepad = false;
    binding.trigger = trigger;
    m_actionBindings.push_back(binding);
}

void PlatformInputManager::BindAction(const std::string& name, GamepadBtn button, ActionTrigger trigger) {
    ActionBinding binding;
    binding.actionName = name;
    binding.gamepadButton = button;
    binding.useGamepad = true;
    binding.trigger = trigger;
    m_actionBindings.push_back(binding);
}

void PlatformInputManager::BindAxis(const std::string& name, KeyCode positiveKey, KeyCode negativeKey, float scale) {
    AxisBinding binding;
    binding.axisName = name;
    binding.positiveKey = positiveKey;
    binding.negativeKey = negativeKey;
    binding.useGamepad = false;
    binding.scale = scale;
    m_axisBindings.push_back(binding);
}

void PlatformInputManager::BindAxis(const std::string& name, GamepadAxis axis, float scale, float deadZone) {
    AxisBinding binding;
    binding.axisName = name;
    binding.gamepadAxis = axis;
    binding.useGamepad = true;
    binding.scale = scale;
    binding.deadZone = deadZone;
    m_axisBindings.push_back(binding);
}

bool PlatformInputManager::IsActionActive(const std::string& name) const {
    for (const auto& binding : m_actionBindings) {
        if (binding.actionName != name) continue;

        if (!binding.useGamepad) {
            switch (binding.trigger) {
                case ActionTrigger::Pressed: if (WasKeyPressed(binding.key)) return true; break;
                case ActionTrigger::Released: if (WasKeyReleased(binding.key)) return true; break;
                case ActionTrigger::Held: if (IsKeyDown(binding.key)) return true; break;
            }
        } else {
            switch (binding.trigger) {
                case ActionTrigger::Pressed: if (WasGamepadButtonPressed(binding.gamepadButton)) return true; break;
                case ActionTrigger::Released: if (WasGamepadButtonReleased(binding.gamepadButton)) return true; break;
                case ActionTrigger::Held: if (IsGamepadButtonDown(binding.gamepadButton)) return true; break;
            }
        }
    }
    return false;
}

float PlatformInputManager::GetAxisValue(const std::string& name) const {
    float result = 0.0f;

    for (const auto& binding : m_axisBindings) {
        if (binding.axisName != name) continue;

        if (!binding.useGamepad) {
            float val = 0.0f;
            if (IsKeyDown(binding.positiveKey)) val += 1.0f;
            if (IsKeyDown(binding.negativeKey)) val -= 1.0f;
            result += val * binding.scale;
        } else {
            float val = GetGamepadAxis(binding.gamepadAxis);
            if (std::abs(val) < binding.deadZone) val = 0.0f;
            result += val * binding.scale;
        }
    }

    return result;
}

void PlatformInputManager::ClearBindings() {
    m_actionBindings.clear();
    m_axisBindings.clear();
}

void PlatformInputManager::RemoveAction(const std::string& name) {
    m_actionBindings.erase(
        std::remove_if(m_actionBindings.begin(), m_actionBindings.end(),
                        [&](const ActionBinding& b) { return b.actionName == name; }),
        m_actionBindings.end());
    m_axisBindings.erase(
        std::remove_if(m_axisBindings.begin(), m_axisBindings.end(),
                        [&](const AxisBinding& b) { return b.axisName == name; }),
        m_axisBindings.end());
}

void PlatformInputManager::RegisterEventCallback(InputEventCallback callback) {
    m_eventCallbacks.push_back(std::move(callback));
}

const char* PlatformInputManager::GetBackendName() const {
    return m_backend ? m_backend->GetBackendName() : "None";
}

std::string PlatformInputManager::GetKeyName(KeyCode key) const {
    const auto& names = GetKeyNames();
    auto it = names.find(key);
    return (it != names.end()) ? it->second : "Unknown";
}

KeyCode PlatformInputManager::GetKeyFromName(const std::string& name) const {
    const auto& names = GetKeyNames();
    for (const auto& [code, keyName] : names) {
        if (keyName == name) return code;
    }
    return KeyCode::Unknown;
}

void PlatformInputManager::HandleWindowMessage(uint32_t message, uintptr_t wParam, intptr_t lParam) {
#ifdef _WIN32
    auto* win32Backend = dynamic_cast<Win32InputBackend*>(m_backend.get());
    if (win32Backend) {
        win32Backend->HandleWindowMessage(message, wParam, lParam);
    }
#endif
}

// ===== Console Integration =====

std::string PlatformInputManager::Console_GetStatus() const {
    std::ostringstream ss;
    ss << "=== Platform Input ===\n";
    ss << "Backend: " << GetBackendName() << "\n";
    ss << "Mouse Captured: " << (IsMouseCaptured() ? "Yes" : "No") << "\n";
    ss << "Gamepads Connected: " << GetGamepadCount() << "\n";
    ss << "Action Bindings: " << m_actionBindings.size() << "\n";
    ss << "Axis Bindings: " << m_axisBindings.size() << "\n";
    ss << "Sensitivity: " << m_globalSensitivity << "\n";
    ss << "Dead Zone: " << m_globalDeadZone << "\n";
    return ss.str();
}

std::string PlatformInputManager::Console_ListBindings() const {
    std::ostringstream ss;
    ss << "=== Action Bindings ===\n";
    for (const auto& b : m_actionBindings) {
        ss << "  " << b.actionName << " -> ";
        if (!b.useGamepad) {
            ss << GetKeyName(b.key);
        } else {
            ss << "Gamepad:" << static_cast<int>(b.gamepadButton);
        }
        ss << "\n";
    }
    ss << "\n=== Axis Bindings ===\n";
    for (const auto& b : m_axisBindings) {
        ss << "  " << b.axisName << " -> ";
        if (!b.useGamepad) {
            ss << GetKeyName(b.positiveKey) << "/" << GetKeyName(b.negativeKey);
        } else {
            ss << "Gamepad Axis:" << static_cast<int>(b.gamepadAxis);
        }
        ss << " (scale:" << b.scale << ")\n";
    }
    return ss.str();
}

void PlatformInputManager::Console_BindAction(const std::string& action, const std::string& keyName) {
    KeyCode key = GetKeyFromName(keyName);
    if (key != KeyCode::Unknown) {
        BindAction(action, key);
    }
}

void PlatformInputManager::Console_UnbindAction(const std::string& action) {
    RemoveAction(action);
}

void PlatformInputManager::Console_SetDeadZone(float deadZone) {
    m_globalDeadZone = (std::max)(0.0f, (std::min)(deadZone, 1.0f));
}

void PlatformInputManager::Console_SetSensitivity(float sensitivity) {
    m_globalSensitivity = (std::max)(0.1f, (std::min)(sensitivity, 10.0f));
}

} // namespace Spark::Input

#endif // SPARK_PLATFORM_WINDOWS
