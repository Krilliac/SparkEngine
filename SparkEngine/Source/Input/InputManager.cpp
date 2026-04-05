/**
 * @file InputManager.cpp
 * @brief Input handling implementation — shared logic + platform-specific backends
 *
 * Shared code (key queries, console integration, key mapping) is defined once
 * before the platform-specific sections to avoid duplication.
 */

#include "InputManager.h"
#include "../Core/Platform.h"
#include "Utils/Assert.h"
#include "../Utils/Validate.h"
#include "../Utils/SparkConsole.h"
#include <cstring>
#include <cmath>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <thread>
#include <chrono>
#include <unordered_map>

#ifdef SPARK_PLATFORM_WINDOWS
#include <windows.h>
#include <windowsx.h>
#else
#ifdef SPARK_SDL2_AVAILABLE
#include <SDL.h>
#endif
#endif

// ============================================================================
// SHARED KEY MAPPING TABLES
// ============================================================================

static const std::unordered_map<std::string, int>& GetKeyNameToVKMap()
{
    static const std::unordered_map<std::string, int> keyMapping = {{"A", 'A'},
                                                                    {"B", 'B'},
                                                                    {"C", 'C'},
                                                                    {"D", 'D'},
                                                                    {"E", 'E'},
                                                                    {"F", 'F'},
                                                                    {"G", 'G'},
                                                                    {"H", 'H'},
                                                                    {"I", 'I'},
                                                                    {"J", 'J'},
                                                                    {"K", 'K'},
                                                                    {"L", 'L'},
                                                                    {"M", 'M'},
                                                                    {"N", 'N'},
                                                                    {"O", 'O'},
                                                                    {"P", 'P'},
                                                                    {"Q", 'Q'},
                                                                    {"R", 'R'},
                                                                    {"S", 'S'},
                                                                    {"T", 'T'},
                                                                    {"U", 'U'},
                                                                    {"V", 'V'},
                                                                    {"W", 'W'},
                                                                    {"X", 'X'},
                                                                    {"Y", 'Y'},
                                                                    {"Z", 'Z'},
                                                                    {"0", '0'},
                                                                    {"1", '1'},
                                                                    {"2", '2'},
                                                                    {"3", '3'},
                                                                    {"4", '4'},
                                                                    {"5", '5'},
                                                                    {"6", '6'},
                                                                    {"7", '7'},
                                                                    {"8", '8'},
                                                                    {"9", '9'},
                                                                    {"SPACE", VK_SPACE},
                                                                    {"ENTER", VK_RETURN},
                                                                    {"ESCAPE", VK_ESCAPE},
                                                                    {"TAB", VK_TAB},
                                                                    {"SHIFT", VK_SHIFT},
                                                                    {"CTRL", VK_CONTROL},
                                                                    {"ALT", VK_MENU},
                                                                    {"F1", VK_F1},
                                                                    {"F2", VK_F2},
                                                                    {"F3", VK_F3},
                                                                    {"F4", VK_F4},
                                                                    {"F5", VK_F5},
                                                                    {"F6", VK_F6},
                                                                    {"F7", VK_F7},
                                                                    {"F8", VK_F8},
                                                                    {"F9", VK_F9},
                                                                    {"F10", VK_F10},
                                                                    {"F11", VK_F11},
                                                                    {"F12", VK_F12},
                                                                    {"UP", VK_UP},
                                                                    {"DOWN", VK_DOWN},
                                                                    {"LEFT", VK_LEFT},
                                                                    {"RIGHT", VK_RIGHT},
                                                                    {"BACKSPACE", VK_BACK},
                                                                    {"DELETE", VK_DELETE},
                                                                    {"INSERT", VK_INSERT},
                                                                    {"HOME", VK_HOME},
                                                                    {"END", VK_END},
                                                                    {"PAGEUP", VK_PRIOR},
                                                                    {"PAGEDOWN", VK_NEXT}};
    return keyMapping;
}

static const std::unordered_map<int, std::string>& GetVKToKeyNameMap()
{
    static const std::unordered_map<int, std::string> reverseMapping = {{'A', "A"},
                                                                        {'B', "B"},
                                                                        {'C', "C"},
                                                                        {'D', "D"},
                                                                        {'E', "E"},
                                                                        {'F', "F"},
                                                                        {'G', "G"},
                                                                        {'H', "H"},
                                                                        {'I', "I"},
                                                                        {'J', "J"},
                                                                        {'K', "K"},
                                                                        {'L', "L"},
                                                                        {'M', "M"},
                                                                        {'N', "N"},
                                                                        {'O', "O"},
                                                                        {'P', "P"},
                                                                        {'Q', "Q"},
                                                                        {'R', "R"},
                                                                        {'S', "S"},
                                                                        {'T', "T"},
                                                                        {'U', "U"},
                                                                        {'V', "V"},
                                                                        {'W', "W"},
                                                                        {'X', "X"},
                                                                        {'Y', "Y"},
                                                                        {'Z', "Z"},
                                                                        {'0', "0"},
                                                                        {'1', "1"},
                                                                        {'2', "2"},
                                                                        {'3', "3"},
                                                                        {'4', "4"},
                                                                        {'5', "5"},
                                                                        {'6', "6"},
                                                                        {'7', "7"},
                                                                        {'8', "8"},
                                                                        {'9', "9"},
                                                                        {VK_SPACE, "Space"},
                                                                        {VK_RETURN, "Enter"},
                                                                        {VK_ESCAPE, "Escape"},
                                                                        {VK_TAB, "Tab"},
                                                                        {VK_SHIFT, "Shift"},
                                                                        {VK_CONTROL, "Ctrl"},
                                                                        {VK_MENU, "Alt"},
                                                                        {VK_F1, "F1"},
                                                                        {VK_F2, "F2"},
                                                                        {VK_F3, "F3"},
                                                                        {VK_F4, "F4"},
                                                                        {VK_F5, "F5"},
                                                                        {VK_F6, "F6"},
                                                                        {VK_F7, "F7"},
                                                                        {VK_F8, "F8"},
                                                                        {VK_F9, "F9"},
                                                                        {VK_F10, "F10"},
                                                                        {VK_F11, "F11"},
                                                                        {VK_F12, "F12"},
                                                                        {VK_UP, "Up"},
                                                                        {VK_DOWN, "Down"},
                                                                        {VK_LEFT, "Left"},
                                                                        {VK_RIGHT, "Right"},
                                                                        {VK_BACK, "Backspace"},
                                                                        {VK_DELETE, "Delete"},
                                                                        {VK_INSERT, "Insert"},
                                                                        {VK_HOME, "Home"},
                                                                        {VK_END, "End"},
                                                                        {VK_PRIOR, "PageUp"},
                                                                        {VK_NEXT, "PageDown"}};
    return reverseMapping;
}

// ============================================================================
// SHARED IMPLEMENTATIONS (platform-independent)
// ============================================================================

int InputManager::KeyNameToVirtualKey(const std::string& keyName) const
{
    std::string upperKeyName = keyName;
    std::transform(upperKeyName.begin(), upperKeyName.end(), upperKeyName.begin(), ::toupper);
    auto& map = GetKeyNameToVKMap();
    auto it = map.find(upperKeyName);
    return (it != map.end()) ? it->second : 0;
}

std::string InputManager::VirtualKeyToKeyName(int virtualKey) const
{
    auto& map = GetVKToKeyNameMap();
    auto it = map.find(virtualKey);
    return (it != map.end()) ? it->second : "Unknown(" + std::to_string(virtualKey) + ")";
}

bool InputManager::IsKeyDown(int key) const
{
    SPARK_REQUIRE_MSG(Spark::LogCategory::Input, key >= 0, "IsKeyDown - invalid key code");
    auto it = m_keyStates.find(key);
    return it != m_keyStates.end() && it->second;
}

bool InputManager::IsKeyUp(int key) const
{
    return !IsKeyDown(key);
}

bool InputManager::WasKeyPressed(int key) const
{
    SPARK_REQUIRE_MSG(Spark::LogCategory::Input, key >= 0, "WasKeyPressed - invalid key code");
    bool curr = IsKeyDown(key);
    bool prev = m_prevKeyStates.count(key) ? m_prevKeyStates.at(key) : false;
    return curr && !prev;
}

bool InputManager::WasKeyReleased(int key) const
{
    SPARK_REQUIRE_MSG(Spark::LogCategory::Input, key >= 0, "WasKeyReleased - invalid key code");
    bool curr = IsKeyDown(key);
    bool prev = m_prevKeyStates.count(key) ? m_prevKeyStates.at(key) : false;
    return !curr && prev;
}

bool InputManager::IsMouseButtonDown(int button) const
{
    SPARK_REQUIRE_MSG(Spark::LogCategory::Input, button >= 0 && button < 3, "IsMouseButtonDown - invalid button");
    return m_mouseButtons[button];
}

bool InputManager::WasMouseButtonPressed(int button) const
{
    SPARK_REQUIRE_MSG(Spark::LogCategory::Input, button >= 0 && button < 3, "WasMouseButtonPressed - invalid button");
    return m_mouseButtons[button] && !m_prevMouseButtons[button];
}

bool InputManager::WasMouseButtonReleased(int button) const
{
    SPARK_REQUIRE_MSG(Spark::LogCategory::Input, button >= 0 && button < 3, "WasMouseButtonReleased - invalid button");
    return !m_mouseButtons[button] && m_prevMouseButtons[button];
}

MousePoint InputManager::GetMouseDelta() const
{
    return {m_mouseDeltaX, m_mouseDeltaY};
}

MousePoint InputManager::GetMousePosition() const
{
    return {m_mouseX, m_mouseY};
}

void InputManager::UpdateMouseButton(int button, bool isDown)
{
    SPARK_REQUIRE_MSG(Spark::LogCategory::Input, button >= 0 && button < 3, "UpdateMouseButton - invalid button");
    m_mouseButtons[button] = isDown;
}

void InputManager::UpdateMousePosition(int x, int y)
{
    m_mouseX = x;
    m_mouseY = y;
}

void InputManager::ProcessMouseDelta(int& deltaX, int& deltaY) const
{
    float distance = sqrtf(static_cast<float>(deltaX * deltaX + deltaY * deltaY));
    if (distance < m_mouseDeadZone)
    {
        deltaX = deltaY = 0;
        return;
    }

    deltaX = static_cast<int>(deltaX * m_mouseSensitivity);
    deltaY = static_cast<int>(deltaY * m_mouseSensitivity);

    if (m_invertMouseY)
        deltaY = -deltaY;

    if (m_mouseAcceleration && distance > 5.0f)
    {
        float accelFactor = 1.0f + (distance - 5.0f) * 0.01f;
        deltaX = static_cast<int>(deltaX * accelFactor);
        deltaY = static_cast<int>(deltaY * accelFactor);
    }
}

void InputManager::LogInputEvent(int key, bool isPressed)
{
    if (m_recentInputEvents.size() >= 100)
    {
        // O(n) erase on small bounded buffer (max 100 entries) — acceptable for input event rate
        m_recentInputEvents.erase(m_recentInputEvents.begin());
    }
    m_recentInputEvents.emplace_back(key, isPressed);
}

void InputManager::NotifyStateChange()
{
    if (m_stateCallback)
        m_stateCallback();
}

// ============================================================================
// SHARED CONSOLE INTEGRATION
// ============================================================================

void InputManager::Console_SetMouseSensitivity(float sensitivity)
{
    std::lock_guard<std::mutex> lock(m_inputMutex);
    if (sensitivity >= 0.1f && sensitivity <= 10.0f)
    {
        m_mouseSensitivity = sensitivity;
        NotifyStateChange();
        Spark::SimpleConsole::GetInstance().Log(
            "Mouse sensitivity set to " + std::to_string(sensitivity) + " via console", "SUCCESS");
    }
    else
    {
        Spark::SimpleConsole::GetInstance().Log("Invalid mouse sensitivity. Must be between 0.1 and 10.0", "ERROR");
    }
}

void InputManager::Console_SetMouseDeadZone(float deadZone)
{
    std::lock_guard<std::mutex> lock(m_inputMutex);
    if (deadZone >= 0.0f && deadZone <= 10.0f)
    {
        m_mouseDeadZone = deadZone;
        NotifyStateChange();
        Spark::SimpleConsole::GetInstance().Log("Mouse dead zone set to " + std::to_string(deadZone) + " via console",
                                                "SUCCESS");
    }
    else
    {
        Spark::SimpleConsole::GetInstance().Log("Invalid mouse dead zone. Must be between 0.0 and 10.0", "ERROR");
    }
}

void InputManager::Console_SetMouseAcceleration(bool enabled)
{
    std::lock_guard<std::mutex> lock(m_inputMutex);
    m_mouseAcceleration = enabled;
    NotifyStateChange();
    Spark::SimpleConsole::GetInstance().Log(
        "Mouse acceleration " + std::string(enabled ? "enabled" : "disabled") + " via console", "SUCCESS");
}

void InputManager::Console_SetInvertMouseY(bool enabled)
{
    std::lock_guard<std::mutex> lock(m_inputMutex);
    m_invertMouseY = enabled;
    NotifyStateChange();
    Spark::SimpleConsole::GetInstance().Log(
        "Mouse Y inversion " + std::string(enabled ? "enabled" : "disabled") + " via console", "SUCCESS");
}

void InputManager::Console_SetRawMouseInput(bool enabled)
{
    std::lock_guard<std::mutex> lock(m_inputMutex);
    m_rawMouseInput = enabled;
    NotifyStateChange();
    Spark::SimpleConsole::GetInstance().Log(
        "Raw mouse input " + std::string(enabled ? "enabled" : "disabled") + " via console", "SUCCESS");
}

void InputManager::Console_SetInputLogging(bool enabled)
{
    std::lock_guard<std::mutex> lock(m_inputMutex);
    m_inputLogging = enabled;
    if (enabled)
        m_recentInputEvents.clear();
    NotifyStateChange();
    Spark::SimpleConsole::GetInstance().Log(
        "Input logging " + std::string(enabled ? "enabled" : "disabled") + " via console", "SUCCESS");
}

bool InputManager::Console_BindKey(const std::string& action, const std::string& keyName)
{
    std::lock_guard<std::mutex> lock(m_inputMutex);

    int virtualKey = KeyNameToVirtualKey(keyName);
    if (virtualKey == 0)
    {
        Spark::SimpleConsole::GetInstance().Log("Invalid key name: " + keyName, "ERROR");
        return false;
    }

    auto existingIt = m_keyBindings.find(action);
    if (existingIt != m_keyBindings.end())
    {
        m_reverseBindings.erase(existingIt->second);
        existingIt->second = virtualKey;
    }
    else
    {
        m_keyBindings.emplace(action, virtualKey);
    }
    m_reverseBindings[virtualKey] = action;

    NotifyStateChange();
    Spark::SimpleConsole::GetInstance().Log("Key '" + keyName + "' bound to action '" + action + "' via console",
                                            "SUCCESS");
    return true;
}

void InputManager::Console_UnbindKey(const std::string& action)
{
    std::lock_guard<std::mutex> lock(m_inputMutex);

    auto it = m_keyBindings.find(action);
    if (it != m_keyBindings.end())
    {
        m_reverseBindings.erase(it->second);
        m_keyBindings.erase(it);
        NotifyStateChange();
        Spark::SimpleConsole::GetInstance().Log("Action '" + action + "' unbound via console", "SUCCESS");
    }
    else
    {
        Spark::SimpleConsole::GetInstance().Log("Action '" + action + "' not found", "ERROR");
    }
}

std::string InputManager::Console_ListKeyBindings() const
{
    std::lock_guard<std::mutex> lock(m_inputMutex);

    std::ostringstream ss;
    ss << "Key Bindings (" << m_keyBindings.size() << " total):\n";
    ss << "==========================================\n";

    for (const auto& [name, key] : m_keyBindings)
        ss << "  " << std::left << std::setw(20) << name << " -> " << VirtualKeyToKeyName(key) << " (" << key << ")\n";

    if (m_keyBindings.empty())
        ss << "  No key bindings configured\n";

    return ss.str();
}

void InputManager::Console_SimulateKeyPress(const std::string& keyName, int duration)
{
    int virtualKey = KeyNameToVirtualKey(keyName);
    if (virtualKey == 0)
    {
        Spark::SimpleConsole::GetInstance().Log("Invalid key name: " + keyName, "ERROR");
        return;
    }

    UpdateKeyState(virtualKey, true);
    if (m_inputLogging)
        LogInputEvent(virtualKey, true);

    if (duration == 0)
    {
        UpdateKeyState(virtualKey, false);
        if (m_inputLogging)
            LogInputEvent(virtualKey, false);
    }
    else
    {
        m_pendingTimedThreads.erase(std::remove_if(m_pendingTimedThreads.begin(), m_pendingTimedThreads.end(),
                                                   [](std::thread& t) { return !t.joinable(); }),
                                    m_pendingTimedThreads.end());

        m_pendingTimedThreads.emplace_back(
            [this, virtualKey, duration]()
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(duration));
                {
                    std::lock_guard<std::mutex> lock(m_inputMutex);
                    UpdateKeyState(virtualKey, false);
                    if (m_inputLogging)
                        LogInputEvent(virtualKey, false);
                }
            });
    }
}

void InputManager::Console_ClearInputStates()
{
    std::lock_guard<std::mutex> lock(m_inputMutex);
    m_keyStates.clear();
    m_prevKeyStates.clear();
    memset(m_mouseButtons, 0, sizeof(m_mouseButtons));
    memset(m_prevMouseButtons, 0, sizeof(m_prevMouseButtons));
    m_recentInputEvents.clear();
    Spark::SimpleConsole::GetInstance().Log("All input states cleared via console", "SUCCESS");
}

std::string InputManager::Console_GetRecentEvents(int count) const
{
    std::lock_guard<std::mutex> lock(m_inputMutex);

    std::ostringstream ss;
    ss << "Recent Input Events (last " << std::min(count, static_cast<int>(m_recentInputEvents.size())) << "):\n";

    int startIndex = std::max(0, static_cast<int>(m_recentInputEvents.size()) - count);
    for (int i = startIndex; i < static_cast<int>(m_recentInputEvents.size()); ++i)
    {
        const auto& event = m_recentInputEvents[i];
        std::string name =
            (event.first >= 1000) ? "Mouse" + std::to_string(event.first - 1000) : VirtualKeyToKeyName(event.first);
        ss << "  " << name << " " << (event.second ? "PRESSED" : "RELEASED") << "\n";
    }

    if (m_recentInputEvents.empty())
        ss << "  No input events recorded\n";

    return ss.str();
}

bool InputManager::Console_IsActionActive(const std::string& action) const
{
    std::lock_guard<std::mutex> lock(m_inputMutex);
    auto it = m_keyBindings.find(action);
    if (it == m_keyBindings.end())
        return false;
    return IsKeyDown(it->second);
}

InputManager::InputMetrics InputManager::GetMetricsThreadSafe() const
{
    std::lock_guard<std::mutex> lock(m_inputMutex);

    InputMetrics metrics;
    metrics.keyPressCount = m_keyPressCount;
    metrics.mousePressCount = m_mousePressCount;
    metrics.totalMouseDistance = m_totalMouseDistance;

    metrics.activeKeys = 0;
    for (const auto& pair : m_keyStates)
    {
        if (pair.second)
            metrics.activeKeys++;
    }

    metrics.activeMouseButtons = 0;
    for (int i = 0; i < 3; ++i)
    {
        if (m_mouseButtons[i])
            metrics.activeMouseButtons++;
    }

    metrics.mouseCaptured = m_mouseCaptured;
    metrics.mouseSensitivity = m_mouseSensitivity;
    metrics.mouseDeadZone = m_mouseDeadZone;
    metrics.mouseAcceleration = m_mouseAcceleration;
    metrics.invertMouseY = m_invertMouseY;
    metrics.rawMouseInput = m_rawMouseInput;
    metrics.inputLogging = m_inputLogging;
    metrics.totalKeyBindings = m_keyBindings.size();

    return metrics;
}

InputManager::InputMetrics InputManager::Console_GetMetrics() const
{
    return GetMetricsThreadSafe();
}

InputManager::InputSettings InputManager::Console_GetSettings() const
{
    std::lock_guard<std::mutex> lock(m_inputMutex);

    InputSettings settings;
    settings.mouseSensitivity = m_mouseSensitivity;
    settings.mouseDeadZone = m_mouseDeadZone;
    settings.mouseAcceleration = m_mouseAcceleration;
    settings.invertMouseY = m_invertMouseY;
    settings.rawMouseInput = m_rawMouseInput;
    settings.inputLogging = m_inputLogging;
    settings.keyBindings = m_keyBindings;

    return settings;
}

void InputManager::Console_ApplySettings(const InputSettings& settings)
{
    std::lock_guard<std::mutex> lock(m_inputMutex);

    m_mouseSensitivity = settings.mouseSensitivity;
    m_mouseDeadZone = settings.mouseDeadZone;
    m_mouseAcceleration = settings.mouseAcceleration;
    m_invertMouseY = settings.invertMouseY;
    m_rawMouseInput = settings.rawMouseInput;
    m_inputLogging = settings.inputLogging;
    m_keyBindings = settings.keyBindings;

    m_reverseBindings.clear();
    for (const auto& pair : m_keyBindings)
        m_reverseBindings[pair.second] = pair.first;

    NotifyStateChange();
    Spark::SimpleConsole::GetInstance().Log("Input settings applied via console", "SUCCESS");
}

void InputManager::Console_ResetToDefaults()
{
    Console_SetMouseSensitivity(1.0f);
    Console_SetMouseDeadZone(0.0f);
    Console_SetMouseAcceleration(false);
    Console_SetInvertMouseY(false);
    Console_SetRawMouseInput(false);
    Console_SetInputLogging(false);

    {
        std::lock_guard<std::mutex> lock(m_inputMutex);
        m_keyBindings.clear();
        m_reverseBindings.clear();
    }

    Spark::SimpleConsole::GetInstance().Log("Input settings reset to defaults via console", "SUCCESS");
}

void InputManager::Console_RegisterStateCallback(std::function<void()> callback)
{
    std::lock_guard<std::mutex> lock(m_inputMutex);
    m_stateCallback = std::move(callback);
    Spark::SimpleConsole::GetInstance().Log("Input state callback registered", "INFO");
}

void InputManager::Console_RefreshInput()
{
    Spark::SimpleConsole::GetInstance().Log("Input system refresh requested via console", "INFO");
    Update();
    Spark::SimpleConsole::GetInstance().Log("Input system refresh complete", "SUCCESS");
}

// ============================================================================
// WINDOWS PLATFORM-SPECIFIC
// ============================================================================
#ifdef SPARK_PLATFORM_WINDOWS

InputManager::InputManager()
    : m_mouseX(0), m_mouseY(0), m_prevMouseX(0), m_prevMouseY(0), m_mouseDeltaX(0), m_mouseDeltaY(0), m_hwnd(nullptr),
      m_mouseCaptured(false), m_mouseSensitivity(1.0f), m_mouseDeadZone(0.0f), m_mouseAcceleration(false),
      m_invertMouseY(false), m_rawMouseInput(false), m_inputLogging(false), m_keyPressCount(0), m_mousePressCount(0),
      m_totalMouseDistance(0.0f)
{
    ZeroMemory(m_mouseButtons, sizeof(m_mouseButtons));
    ZeroMemory(m_prevMouseButtons, sizeof(m_prevMouseButtons));
    m_recentInputEvents.reserve(100);
    SPARK_LOG_INFO(Spark::LogCategory::Input, "InputManager constructed (Windows)");
    Spark::SimpleConsole::GetInstance().Log("InputManager constructed with console integration.", "INFO");
}

InputManager::~InputManager()
{
    SPARK_LOG_INFO(Spark::LogCategory::Input, "InputManager shutting down");
    Spark::SimpleConsole::GetInstance().Log("InputManager destructor called.", "INFO");
    if (m_mouseCaptured)
        CaptureMouse(false);

    for (auto& t : m_pendingTimedThreads)
    {
        if (t.joinable())
            t.join();
    }
    m_pendingTimedThreads.clear();
}

void InputManager::Initialize(HWND hwnd)
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Input);
    SPARK_REQUIRE_NOT_NULL(Spark::LogCategory::Input, hwnd);
    m_hwnd = hwnd;

    RECT rect;
    GetClientRect(hwnd, &rect);
    POINT center = {(rect.right - rect.left) / 2, (rect.bottom - rect.top) / 2};
    ClientToScreen(hwnd, &center);
    SetCursorPos(center.x, center.y);

    m_mouseX = m_prevMouseX = center.x;
    m_mouseY = m_prevMouseY = center.y;

    SPARK_LOG_INFO(Spark::LogCategory::Input, "InputManager initialized (Windows)");
    Spark::SimpleConsole::GetInstance().Log("InputManager initialized with console integration.", "SUCCESS");
}

void InputManager::Update()
{
    static bool firstFrame = true;
    if (firstFrame)
    {
        Spark::SimpleConsole::GetInstance().Log("InputManager::Update - First frame started with console integration",
                                                "INFO");
        firstFrame = false;
    }

    SPARK_REQUIRE_MSG(Spark::LogCategory::Input, m_hwnd != nullptr, "InputManager::Update - hwnd not initialized");

    m_prevKeyStates = m_keyStates;
    memcpy(m_prevMouseButtons, m_mouseButtons, sizeof(m_mouseButtons));

    m_prevMouseX = m_mouseX;
    m_prevMouseY = m_mouseY;

    if (m_mouseCaptured)
    {
        POINT cursor;
        GetCursorPos(&cursor);
        ScreenToClient(m_hwnd, &cursor);

        m_mouseDeltaX = cursor.x - m_prevMouseX;
        m_mouseDeltaY = cursor.y - m_prevMouseY;

        ProcessMouseDelta(m_mouseDeltaX, m_mouseDeltaY);

        RECT rect;
        GetClientRect(m_hwnd, &rect);
        POINT center = {(rect.right - rect.left) / 2, (rect.bottom - rect.top) / 2};
        ClientToScreen(m_hwnd, &center);
        SetCursorPos(center.x, center.y);

        m_mouseX = center.x;
        m_mouseY = center.y;

        float distance = sqrtf(static_cast<float>(m_mouseDeltaX * m_mouseDeltaX + m_mouseDeltaY * m_mouseDeltaY));
        m_totalMouseDistance.fetch_add(distance, std::memory_order_relaxed);
    }
    else
    {
        m_mouseDeltaX = m_mouseX - m_prevMouseX;
        m_mouseDeltaY = m_mouseY - m_prevMouseY;
        ProcessMouseDelta(m_mouseDeltaX, m_mouseDeltaY);
    }
}

void InputManager::HandleMouseButtonMessage(int button, bool isDown)
{
    UpdateMouseButton(button, isDown);
    if (m_inputLogging)
        LogInputEvent(1000 + button, isDown);
    if (isDown)
        m_mousePressCount++;
}

void InputManager::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_KEYDOWN:
        UpdateKeyState(static_cast<int>(wParam), true);
        if (m_inputLogging)
            LogInputEvent(static_cast<int>(wParam), true);
        m_keyPressCount++;
        break;
    case WM_KEYUP:
        UpdateKeyState(static_cast<int>(wParam), false);
        if (m_inputLogging)
            LogInputEvent(static_cast<int>(wParam), false);
        break;
    case WM_LBUTTONDOWN:
        HandleMouseButtonMessage(0, true);
        if (!m_mouseCaptured)
            CaptureMouse(true);
        break;
    case WM_LBUTTONUP:
        HandleMouseButtonMessage(0, false);
        break;
    case WM_RBUTTONDOWN:
        HandleMouseButtonMessage(1, true);
        break;
    case WM_RBUTTONUP:
        HandleMouseButtonMessage(1, false);
        break;
    case WM_MBUTTONDOWN:
        HandleMouseButtonMessage(2, true);
        break;
    case WM_MBUTTONUP:
        HandleMouseButtonMessage(2, false);
        break;
    case WM_MOUSEMOVE:
        if (!m_mouseCaptured)
            UpdateMousePosition(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        break;
    default:
        break;
    }
}

void InputManager::UpdateKeyState(int key, bool isDown)
{
    SPARK_REQUIRE_MSG(Spark::LogCategory::Input, key >= 0, "UpdateKeyState - invalid key code");
    m_keyStates[key] = isDown;
    if (key == VK_ESCAPE && !isDown && m_mouseCaptured)
        CaptureMouse(false);
}

void InputManager::CaptureMouse(bool capture)
{
    if (capture)
    {
        if (!m_mouseCaptured)
        {
            SetCapture(m_hwnd);
            ShowCursor(FALSE);
            m_mouseCaptured = true;
            Spark::SimpleConsole::GetInstance().Log("Mouse captured.", "INFO");
        }
    }
    else
    {
        if (m_mouseCaptured)
        {
            ReleaseCapture();
            ShowCursor(TRUE);
            m_mouseCaptured = false;
            Spark::SimpleConsole::GetInstance().Log("Mouse capture released.", "INFO");
        }
    }
}

#endif // SPARK_PLATFORM_WINDOWS

// ============================================================================
// LINUX/macOS PLATFORM-SPECIFIC
// ============================================================================
#ifndef SPARK_PLATFORM_WINDOWS

InputManager::InputManager()
    : m_mouseX(0), m_mouseY(0), m_prevMouseX(0), m_prevMouseY(0), m_mouseDeltaX(0), m_mouseDeltaY(0), m_hwnd(nullptr),
      m_mouseCaptured(false), m_mouseSensitivity(1.0f), m_mouseDeadZone(0.0f), m_mouseAcceleration(false),
      m_invertMouseY(false), m_rawMouseInput(false), m_inputLogging(false), m_keyPressCount(0), m_mousePressCount(0),
      m_totalMouseDistance(0.0f)
{
    memset(m_mouseButtons, 0, sizeof(m_mouseButtons));
    memset(m_prevMouseButtons, 0, sizeof(m_prevMouseButtons));
    m_recentInputEvents.reserve(100);
    SPARK_LOG_INFO(Spark::LogCategory::Input, "InputManager constructed (Linux/macOS)");
}

InputManager::~InputManager()
{
    SPARK_LOG_INFO(Spark::LogCategory::Input, "InputManager shutting down");
    for (auto& t : m_pendingTimedThreads)
    {
        if (t.joinable())
            t.join();
    }
    m_pendingTimedThreads.clear();
}

void InputManager::Initialize(HWND hwnd)
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Input);
    m_hwnd = hwnd;
    SPARK_LOG_INFO(Spark::LogCategory::Input, "InputManager initialized (Linux/SDL2 backend)");
    Spark::SimpleConsole::GetInstance().Log("InputManager initialized (Linux/SDL2 backend).", "INFO");
}

void InputManager::Update()
{
    m_prevKeyStates = m_keyStates;
    memcpy(m_prevMouseButtons, m_mouseButtons, sizeof(m_mouseButtons));
    m_mouseDeltaX = m_mouseX - m_prevMouseX;
    m_mouseDeltaY = m_mouseY - m_prevMouseY;
    m_prevMouseX = m_mouseX;
    m_prevMouseY = m_mouseY;
}

void InputManager::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_KEYDOWN:
        UpdateKeyState(static_cast<int>(wParam), true);
        break;
    case WM_KEYUP:
        UpdateKeyState(static_cast<int>(wParam), false);
        break;
    case WM_LBUTTONDOWN:
        UpdateMouseButton(0, true);
        break;
    case WM_LBUTTONUP:
        UpdateMouseButton(0, false);
        break;
    case WM_RBUTTONDOWN:
        UpdateMouseButton(1, true);
        break;
    case WM_RBUTTONUP:
        UpdateMouseButton(1, false);
        break;
    case WM_MBUTTONDOWN:
        UpdateMouseButton(2, true);
        break;
    case WM_MBUTTONUP:
        UpdateMouseButton(2, false);
        break;
    case WM_MOUSEMOVE:
        UpdateMousePosition(static_cast<int>(lParam & 0xFFFF), static_cast<int>((lParam >> 16) & 0xFFFF));
        break;
    }
}

void InputManager::UpdateKeyState(int key, bool isDown)
{
    SPARK_REQUIRE_MSG(Spark::LogCategory::Input, key >= 0, "UpdateKeyState - invalid key code");
    m_keyStates[key] = isDown;
    if (isDown)
        ++m_keyPressCount;
    if (m_inputLogging)
        LogInputEvent(key, isDown);
    NotifyStateChange();
}

void InputManager::CaptureMouse(bool capture)
{
    if (capture == m_mouseCaptured)
        return;

    m_mouseCaptured = capture;

#ifdef SPARK_SDL2_AVAILABLE
    SDL_SetRelativeMouseMode(capture ? SDL_TRUE : SDL_FALSE);
#endif

    Spark::SimpleConsole::GetInstance().Log(capture ? "Mouse captured." : "Mouse capture released.", "INFO");
}

#endif // !SPARK_PLATFORM_WINDOWS
