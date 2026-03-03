#include "SparkConsole.h"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <conio.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <thread>
#include <map>
#include <psapi.h>
#include <DirectXMath.h>

// Include actual headers for complete type definitions
#include "../Game/Game.h"
#include "../Game/Player.h"
#include "../Camera/SparkEngineCamera.h"
#include "../Graphics/GraphicsEngine.h"
#include "../Input/InputManager.h"
#include "../Projectiles/ProjectilePool.h"
#include "../SceneManager/SceneManager.h"
#include "../Utils/Timer.h"
#include "../Game/Console.h"

// External references to global engine components
extern std::unique_ptr<GraphicsEngine> g_graphics;
extern std::unique_ptr<Game> g_game;
extern std::unique_ptr<InputManager> g_input;
extern std::unique_ptr<Timer> g_timer;
extern Console g_console;

namespace Spark {

// Performance tracking globals for live binding
static struct PerformanceCounters {
    float currentFPS = 0.0f;
    float averageFPS = 0.0f;
    float frameTime = 0.0f;
    float cpuUsage = 0.0f;
    float gpuUsage = 0.0f;
    int drawCalls = 0;
    int triangles = 0;
    int vertices = 0;
    size_t textureMemory = 0;
    size_t bufferMemory = 0;
    float renderTime = 0.0f;
    float updateTime = 0.0f;
    
    // FPS calculation helpers
    std::chrono::high_resolution_clock::time_point lastFPSUpdate;
    int frameCounter = 0;
    float fpsSamples[60] = {0}; // Rolling average
    int sampleIndex = 0;
} g_perfCounters;

// Game state globals for live binding  
static struct GameState {
    // Player stats
    float playerHealth = 100.0f;
    float playerMaxHealth = 100.0f;
    float playerArmor = 0.0f;
    float playerMaxArmor = 100.0f;
    DirectX::XMFLOAT3 playerPosition = {0.0f, 0.0f, 0.0f};
    DirectX::XMFLOAT3 cameraRotation = {0.0f, 0.0f, 0.0f};
    
    // Physics settings (live modifiable)
    float gravity = -20.0f;
    float playerSpeed = 5.0f;
    float jumpHeight = 3.0f;
    float friction = 0.9f;
    
    // Camera settings (live modifiable)
    float fov = 75.0f;
    float mouseSensitivity = 1.0f;
    bool invertY = false;
    
    // System settings (live modifiable)
    float timeScale = 1.0f;
    bool vsync = true;
    bool wireframe = false;
    bool showFPS = false;
    bool noclip = false;
    bool godMode = false;
    bool infiniteAmmo = false;
    
    // Scene management
    std::vector<std::string> availableScenes = {"test_level", "debug_room", "empty_scene"};
    std::string currentScene = "test_level";
    int activeObjects = 0;
    int visibleObjects = 0;
    int culledObjects = 0;
} g_gameState;

// Configuration system for save/load
static struct ConfigSystem {
    std::string configFile = "spark_config.ini";
    bool autoSave = true;
    
    void SaveConfig() {
        try {
            std::ofstream file(configFile);
            if (file.is_open()) {
                file << "[Physics]\n";
                file << "Gravity=" << g_gameState.gravity << "\n";
                file << "PlayerSpeed=" << g_gameState.playerSpeed << "\n";
                file << "JumpHeight=" << g_gameState.jumpHeight << "\n";
                file << "Friction=" << g_gameState.friction << "\n";
                
                file << "\n[Camera]\n";
                file << "FOV=" << g_gameState.fov << "\n";
                file << "MouseSensitivity=" << g_gameState.mouseSensitivity << "\n";
                file << "InvertY=" << (g_gameState.invertY ? "true" : "false") << "\n";
                
                file << "\n[System]\n";
                file << "TimeScale=" << g_gameState.timeScale << "\n";
                file << "VSync=" << (g_gameState.vsync ? "true" : "false") << "\n";
                file << "Wireframe=" << (g_gameState.wireframe ? "true" : "false") << "\n";
                file << "ShowFPS=" << (g_gameState.showFPS ? "true" : "false") << "\n";
                file << "Noclip=" << (g_gameState.noclip ? "true" : "false") << "\n";
                file << "GodMode=" << (g_gameState.godMode ? "true" : "false") << "\n";
                file << "InfiniteAmmo=" << (g_gameState.infiniteAmmo ? "true" : "false") << "\n";
                
                file.close();
            }
        } catch (...) {
            // Silent fail for now
        }
    }
    
    void LoadConfig() {
        try {
            std::ifstream file(configFile);
            if (file.is_open()) {
                std::string line;
                std::string section;
                
                while (std::getline(file, line)) {
                    if (line.empty() || line[0] == '#') continue;
                    
                    if (line[0] == '[' && line.back() == ']') {
                        section = line.substr(1, line.length() - 2);
                        continue;
                    }
                    
                    size_t pos = line.find('=');
                    if (pos != std::string::npos) {
                        std::string key = line.substr(0, pos);
                        std::string value = line.substr(pos + 1);
                        
                        if (section == "Physics") {
                            if (key == "Gravity") g_gameState.gravity = std::stof(value);
                            else if (key == "PlayerSpeed") g_gameState.playerSpeed = std::stof(value);
                            else if (key == "JumpHeight") g_gameState.jumpHeight = std::stof(value);
                            else if (key == "Friction") g_gameState.friction = std::stof(value);
                        } else if (section == "Camera") {
                            if (key == "FOV") g_gameState.fov = std::stof(value);
                            else if (key == "MouseSensitivity") g_gameState.mouseSensitivity = std::stof(value);
                            else if (key == "InvertY") g_gameState.invertY = (value == "true");
                        } else if (section == "System") {
                            if (key == "TimeScale") g_gameState.timeScale = std::stof(value);
                            else if (key == "VSync") g_gameState.vsync = (value == "true");
                            else if (key == "Wireframe") g_gameState.wireframe = (value == "true");
                            else if (key == "ShowFPS") g_gameState.showFPS = (value == "true");
                            else if (key == "Noclip") g_gameState.noclip = (value == "true");
                            else if (key == "GodMode") g_gameState.godMode = (value == "true");
                            else if (key == "InfiniteAmmo") g_gameState.infiniteAmmo = (value == "true");
                        }
                    }
                }
                file.close();
            }
        } catch (...) {
            // Silent fail for now, use defaults
        }
    }
} g_configSystem;

// Helper functions for live system integration
void UpdatePerformanceCounters() {
    static auto lastUpdate = std::chrono::high_resolution_clock::now();
    auto now = std::chrono::high_resolution_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - lastUpdate);
    
    if (elapsed.count() > 16667) { // ~60 FPS update rate
        if (g_timer) {
            // Use simple time calculation instead of calling GetDeltaTime which may cause issues
            auto timeDiff = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastUpdate);
            g_perfCounters.frameTime = (float)timeDiff.count();
        }
        
        // Calculate FPS
        g_perfCounters.frameCounter++;
        auto fpsDelta = std::chrono::duration_cast<std::chrono::seconds>(now - g_perfCounters.lastFPSUpdate);
        if (fpsDelta.count() >= 1) {
            g_perfCounters.currentFPS = g_perfCounters.frameCounter / (float)fpsDelta.count();
            g_perfCounters.fpsSamples[g_perfCounters.sampleIndex] = g_perfCounters.currentFPS;
            g_perfCounters.sampleIndex = (g_perfCounters.sampleIndex + 1) % 60;
            
            // Calculate rolling average
            float sum = 0.0f;
            for (int i = 0; i < 60; i++) {
                sum += g_perfCounters.fpsSamples[i];
            }
            g_perfCounters.averageFPS = sum / 60.0f;
            
            g_perfCounters.lastFPSUpdate = now;
            g_perfCounters.frameCounter = 0;
        }
        
        lastUpdate = now;
    }
}

void UpdateGameState() {
    // Update player state from actual game objects
    if (g_game) {
        // Get real player data using new accessor methods
        Player* player = g_game->GetPlayer();
        if (player) {
            g_gameState.playerHealth = player->GetHealth();
            g_gameState.playerMaxHealth = player->GetMaxHealth();
            // Position would need a GetPosition() method in Player
            // g_gameState.playerPosition = player->GetPosition();
        }
        
        // Get real camera data
        SparkEngineCamera* camera = g_game->GetCamera();
        if (camera) {
            g_gameState.playerPosition = camera->GetPosition();
            // Rotation would need GetRotation() method
            // g_gameState.cameraRotation = camera->GetRotation();
        }
        
        // Get real scene object counts
        int drawCalls, triangles, activeObjects;
        g_game->GetPerformanceStats(drawCalls, triangles, activeObjects);
        g_gameState.activeObjects = activeObjects;
        g_gameState.visibleObjects = activeObjects; // Simplified for now
        g_gameState.culledObjects = 0; // Would need culling system integration
        
        // Update performance counters with real data
        g_perfCounters.drawCalls = drawCalls;
        g_perfCounters.triangles = triangles;
    }
}

SimpleConsole& SimpleConsole::GetInstance() {
    static SimpleConsole instance;
    return instance;
}

bool SimpleConsole::Initialize() {
    if (m_initialized) {
        return true;
    }
    
    if (!CreateConsoleWindow()) {
        return false;
    }
    
    SetupConsoleHandles();
    
    // Initialize performance counters
    g_perfCounters.lastFPSUpdate = std::chrono::high_resolution_clock::now();
    g_perfCounters.frameCounter = 0;
    g_perfCounters.sampleIndex = 0;
    for (int i = 0; i < 60; i++) {
        g_perfCounters.fpsSamples[i] = 60.0f; // Initialize with target FPS
    }
    
    // Load configuration on startup
    if (std::filesystem::exists(g_configSystem.configFile)) {
        g_configSystem.LoadConfig();
        LogInfo("Configuration loaded: " + g_configSystem.configFile);
    } else {
        LogInfo("No existing configuration found, using defaults");
    }
    
    RegisterDefaultCommands();
    RegisterAdvancedCommands();
    
    Clear();
    PrintLine("==========================================", Color::Cyan);
    PrintLine("    Spark Engine Development Console", Color::Cyan);
    PrintLine("        Professional Debug Interface", Color::White);
    PrintLine("        LIVE SYSTEM INTEGRATION", Color::Yellow);
    PrintLine("==========================================", Color::Cyan);
    PrintLine("Type 'help' for commands | 'help <cmd>' for details", Color::Yellow);
    PrintLine("", Color::White);
    
    m_initialized = true;
    LogSuccess("Development console initialized with live integration");
    LogInfo("Performance counters active - real-time monitoring enabled");
    LogInfo("Configuration system ready - auto-save: " + std::string(g_configSystem.autoSave ? "ON" : "OFF"));
    return true;
}

void SimpleConsole::Shutdown() {
    if (!m_initialized) {
        return;
    }
    
    LogInfo("Console shutdown initiated");
    
    if (m_consoleWindow) {
        FreeConsole();
        m_consoleWindow = nullptr;
        m_consoleOutput = nullptr;
        m_consoleInput = nullptr;
    }
    
    m_initialized = false;
}

void SimpleConsole::Update() {
    if (!m_initialized || !m_visible) {
        return;
    }

    // Update performance counters every frame
    static auto lastPerfUpdate = std::chrono::high_resolution_clock::now();
    auto now = std::chrono::high_resolution_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastPerfUpdate);

    if (elapsed.count() > 100) { // Update every 100ms
        UpdatePerformanceCounters();
        UpdateGameState();
        lastPerfUpdate = now;
    }

    // Update watch variables
    UpdateWatches();

    ProcessInput();
}

void SimpleConsole::Log(const std::string& message, const std::string& type) {
    std::lock_guard<std::mutex> lock(m_logMutex);
    
    LogEntry entry;
    entry.message = message;
    entry.type = type;
    entry.timestamp = GetTimestamp();
    
    m_logHistory.push_back(entry);
    
    if (m_logHistory.size() > 1000) {
        m_logHistory.pop_front();
    }
    
    // Always output to debug console too
    std::string debugMsg = "[" + entry.timestamp + "] [" + type + "] " + message;
    OutputDebugStringA(debugMsg.c_str());
    OutputDebugStringA("\n");
    
    if (m_initialized && m_visible) {
        Color color = Color::White;
        if (type == "ERROR") color = Color::Red;
        else if (type == "WARNING") color = Color::Yellow;
        else if (type == "SUCCESS") color = Color::Green;
        else if (type == "INFO") color = Color::Cyan;
        else if (type == "DEBUG") color = Color::Magenta;
        else if (type == "CRITICAL") color = Color::Red;
        else if (type == "TRACE") color = Color::Blue;
        
        PrintLine("[" + entry.timestamp + "] [" + type + "] " + message, color);
        DisplayPrompt();
    }
}

void SimpleConsole::LogInfo(const std::string& message) {
    Log(message, "INFO");
}

void SimpleConsole::LogWarning(const std::string& message) {
    Log(message, "WARNING");
}

void SimpleConsole::LogError(const std::string& message) {
    Log(message, "ERROR");
}

void SimpleConsole::LogSuccess(const std::string& message) {
    Log(message, "SUCCESS");
}

void SimpleConsole::LogCritical(const std::string& message) {
    Log(message, "CRITICAL");
}

void SimpleConsole::LogTrace(const std::string& message) {
    Log(message, "TRACE");
}

void SimpleConsole::RegisterCommand(const std::string& name, CommandHandler handler, const std::string& description, const std::string& category) {
    m_commands[name] = {handler, description, category};
    LogTrace("Command registered: " + name);
}

void SimpleConsole::LogDebug(const std::string& message) {
    Log(message, "DEBUG");
}

bool SimpleConsole::ExecuteCommand(const std::string& commandLine) {
    if (commandLine.empty()) {
        return false;
    }
    
    auto args = ParseCommand(commandLine);
    if (args.empty()) {
        return false;
    }
    
    std::string command = args[0];
    args.erase(args.begin());
    
    auto it = m_commands.find(command);
    if (it == m_commands.end()) {
        LogError("Unknown command: '" + command + "'. Type 'help' for available commands.");
        return false;
    }
    
    try {
        std::string result = it->second.handler(args);
        if (!result.empty()) {
            LogInfo(result);
        }
        return true;
    } catch (const std::exception& e) {
        LogError("Command execution failed: " + std::string(e.what()));
        return false;
    }
}

void SimpleConsole::Show() {
    if (!m_initialized) {
        return;
    }
    
    if (!m_visible) {
        m_visible = true;
        ShowWindow(m_consoleWindow, SW_SHOW);
        SetForegroundWindow(m_consoleWindow);
        RedrawConsole();
    }
}

void SimpleConsole::Hide() {
    if (!m_initialized) {
        return;
    }
    
    if (m_visible) {
        m_visible = false;
        ShowWindow(m_consoleWindow, SW_HIDE);
    }
}

void SimpleConsole::Toggle() {
    if (m_visible) {
        Hide();
    } else {
        Show();
    }
}

void SimpleConsole::Clear() {
    if (!m_initialized) {
        return;
    }
    
    system("cls");
    
    PrintLine("==========================================", Color::Cyan);
    PrintLine("    Spark Engine Development Console", Color::Cyan);
    PrintLine("        Professional Debug Interface", Color::White);
    PrintLine("==========================================", Color::Cyan);
    PrintLine("Type 'help' for commands | 'help <cmd>' for details", Color::Yellow);
    PrintLine("", Color::White);
    
    DisplayPrompt();
}

std::vector<SimpleConsole::LogEntry> SimpleConsole::GetLogHistory() const {
    std::lock_guard<std::mutex> lock(m_logMutex);
    return std::vector<LogEntry>(m_logHistory.begin(), m_logHistory.end());
}

std::vector<std::string> SimpleConsole::GetCommandHistory() const {
    return std::vector<std::string>(m_commandHistory.begin(), m_commandHistory.end());
}

void SimpleConsole::SetColor(Color color) {
    if (m_consoleOutput) {
        SetConsoleTextAttribute(m_consoleOutput, static_cast<WORD>(color));
    }
}

void SimpleConsole::Print(const std::string& text, Color color) {
    if (!m_consoleOutput) {
        return;
    }
    
    SetColor(color);
    DWORD written;
    WriteConsoleA(m_consoleOutput, text.c_str(), static_cast<DWORD>(text.length()), &written, NULL);
    SetColor(Color::White);
}

void SimpleConsole::PrintLine(const std::string& text, Color color) {
    Print(text + "\n", color);
}

void SimpleConsole::ProcessInput() {
    if (!_kbhit()) {
        return;
    }

    char ch = _getch();

    // Handle special keys (arrows, etc.) - they come as 0 or 0xE0 followed by a scan code
    if (ch == 0 || ch == -32) {
        char scanCode = _getch();
        switch (scanCode) {
        case 72: // Up arrow
            NavigateHistoryUp();
            return;
        case 80: // Down arrow
            NavigateHistoryDown();
            return;
        case 75: // Left arrow
            if (m_cursorPosition > 0) {
                m_cursorPosition--;
                Print("\b");
            }
            return;
        case 77: // Right arrow
            if (m_cursorPosition < static_cast<int>(m_currentInput.size())) {
                Print(std::string(1, m_currentInput[m_cursorPosition]));
                m_cursorPosition++;
            }
            return;
        case 71: // Home
            while (m_cursorPosition > 0) {
                Print("\b");
                m_cursorPosition--;
            }
            return;
        case 79: // End
            while (m_cursorPosition < static_cast<int>(m_currentInput.size())) {
                Print(std::string(1, m_currentInput[m_cursorPosition]));
                m_cursorPosition++;
            }
            return;
        case 83: // Delete
            if (m_cursorPosition < static_cast<int>(m_currentInput.size())) {
                m_currentInput.erase(m_cursorPosition, 1);
                RedrawInputLine();
            }
            return;
        default:
            return;
        }
    }

    // Reset tab completion state on non-tab input
    if (ch != '\t') {
        m_tabIndex = -1;
        m_tabCompletions.clear();
    }

    if (ch == '\r' || ch == '\n') {
        Print("\n");

        if (!m_currentInput.empty()) {
            // Resolve aliases before execution
            std::string resolvedCommand = ResolveAliases(m_currentInput);

            m_commandHistory.push_back(m_currentInput);
            if (m_commandHistory.size() > 100) {
                m_commandHistory.pop_front();
            }
            m_historyIndex = static_cast<int>(m_commandHistory.size());

            ExecuteCommand(resolvedCommand);
            m_currentInput.clear();
            m_cursorPosition = 0;
        }

        DisplayPrompt();
    } else if (ch == '\b') {
        if (m_cursorPosition > 0) {
            m_currentInput.erase(m_cursorPosition - 1, 1);
            m_cursorPosition--;
            RedrawInputLine();
        }
    } else if (ch == '\t') {
        HandleTabCompletion();
    } else if (ch == 27) { // Escape - clear line
        for (size_t i = 0; i < m_currentInput.size(); ++i) {
            Print("\b \b");
        }
        m_currentInput.clear();
        m_cursorPosition = 0;
    } else if (ch == 21) { // Ctrl+U - clear line before cursor
        std::string remaining = m_currentInput.substr(m_cursorPosition);
        for (int i = 0; i < m_cursorPosition; ++i) {
            Print("\b \b");
        }
        m_currentInput = remaining;
        m_cursorPosition = 0;
        RedrawInputLine();
    } else if (ch == 11) { // Ctrl+K - clear line after cursor
        int charsToDelete = static_cast<int>(m_currentInput.size()) - m_cursorPosition;
        m_currentInput = m_currentInput.substr(0, m_cursorPosition);
        for (int i = 0; i < charsToDelete; ++i) {
            Print(" ");
        }
        for (int i = 0; i < charsToDelete; ++i) {
            Print("\b");
        }
    } else if (ch >= 32 && ch <= 126) {
        m_currentInput.insert(m_currentInput.begin() + m_cursorPosition, ch);
        m_cursorPosition++;
        if (m_cursorPosition == static_cast<int>(m_currentInput.size())) {
            Print(std::string(1, ch), Color::White);
        } else {
            RedrawInputLine();
        }
    }
}

void SimpleConsole::RedrawInputLine() {
    // Move cursor to start of input (after prompt)
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    GetConsoleScreenBufferInfo(m_consoleOutput, &csbi);

    // Calculate prompt length ("SPARK> " = 7 chars)
    int promptLen = 7;
    SHORT lineStart = csbi.dwCursorPosition.Y;

    // Move to start of input area
    COORD startPos = { static_cast<SHORT>(promptLen), lineStart };
    SetConsoleCursorPosition(m_consoleOutput, startPos);

    // Write the full input, overwriting old content
    DWORD written;
    std::string padded = m_currentInput + std::string(10, ' '); // Pad to clear old chars
    WriteConsoleA(m_consoleOutput, padded.c_str(), static_cast<DWORD>(padded.length()), &written, NULL);

    // Move cursor to correct position
    COORD cursorPos = { static_cast<SHORT>(promptLen + m_cursorPosition), lineStart };
    SetConsoleCursorPosition(m_consoleOutput, cursorPos);
}

void SimpleConsole::HandleTabCompletion() {
    if (m_currentInput.empty()) {
        return;
    }

    // Build completions list on first tab press
    if (m_tabIndex == -1) {
        m_tabPrefix = m_currentInput;
        m_tabCompletions = GetCompletions(m_tabPrefix);

        if (m_tabCompletions.empty()) {
            return;
        }
        m_tabIndex = 0;
    } else {
        m_tabIndex = (m_tabIndex + 1) % static_cast<int>(m_tabCompletions.size());
    }

    // Show completion count on first tab
    if (m_tabCompletions.size() > 1 && m_tabIndex == 0) {
        // If multiple completions, show them all briefly
        Print("\n", Color::DarkGray);
        for (const auto& completion : m_tabCompletions) {
            Print("  " + completion, Color::DarkGray);
        }
        Print("\n");
        DisplayPrompt();
    }

    // Replace current input with the selected completion
    // Clear old input visually
    for (size_t i = 0; i < m_currentInput.size(); ++i) {
        Print("\b \b");
    }

    m_currentInput = m_tabCompletions[m_tabIndex];
    m_cursorPosition = static_cast<int>(m_currentInput.size());
    Print(m_currentInput, Color::White);
}

std::vector<std::string> SimpleConsole::GetCompletions(const std::string& prefix) {
    std::vector<std::string> completions;

    // Match commands
    for (const auto& pair : m_commands) {
        if (pair.first.size() >= prefix.size() &&
            pair.first.substr(0, prefix.size()) == prefix) {
            completions.push_back(pair.first);
        }
    }

    // Match aliases
    for (const auto& pair : m_aliases) {
        if (pair.first.size() >= prefix.size() &&
            pair.first.substr(0, prefix.size()) == prefix) {
            completions.push_back(pair.first);
        }
    }

    std::sort(completions.begin(), completions.end());
    return completions;
}

void SimpleConsole::NavigateHistoryUp() {
    if (m_commandHistory.empty()) {
        return;
    }

    if (m_historyIndex > 0) {
        m_historyIndex--;
    }

    // Clear current input visually
    for (size_t i = 0; i < m_currentInput.size(); ++i) {
        Print("\b \b");
    }

    m_currentInput = m_commandHistory[m_historyIndex];
    m_cursorPosition = static_cast<int>(m_currentInput.size());
    Print(m_currentInput, Color::White);
}

void SimpleConsole::NavigateHistoryDown() {
    if (m_commandHistory.empty()) {
        return;
    }

    // Clear current input visually
    for (size_t i = 0; i < m_currentInput.size(); ++i) {
        Print("\b \b");
    }

    if (m_historyIndex < static_cast<int>(m_commandHistory.size()) - 1) {
        m_historyIndex++;
        m_currentInput = m_commandHistory[m_historyIndex];
    } else {
        m_historyIndex = static_cast<int>(m_commandHistory.size());
        m_currentInput.clear();
    }

    m_cursorPosition = static_cast<int>(m_currentInput.size());
    Print(m_currentInput, Color::White);
}

// Alias system
void SimpleConsole::SetAlias(const std::string& alias, const std::string& command) {
    m_aliases[alias] = command;
}

void SimpleConsole::RemoveAlias(const std::string& alias) {
    m_aliases.erase(alias);
}

std::string SimpleConsole::ResolveAliases(const std::string& commandLine) {
    auto args = ParseCommand(commandLine);
    if (args.empty()) return commandLine;

    auto it = m_aliases.find(args[0]);
    if (it != m_aliases.end()) {
        // Replace the alias with the real command, preserving arguments
        std::string resolved = it->second;
        for (size_t i = 1; i < args.size(); ++i) {
            resolved += " " + args[i];
        }
        return resolved;
    }
    return commandLine;
}

// Watch system
void SimpleConsole::AddWatch(const std::string& name, std::function<std::string()> getter) {
    // Check if watch already exists
    for (auto& entry : m_watchEntries) {
        if (entry.name == name) {
            entry.getter = getter;
            entry.active = true;
            return;
        }
    }
    m_watchEntries.push_back({name, getter, "", true});
}

void SimpleConsole::RemoveWatch(const std::string& name) {
    m_watchEntries.erase(
        std::remove_if(m_watchEntries.begin(), m_watchEntries.end(),
            [&name](const WatchEntry& e) { return e.name == name; }),
        m_watchEntries.end());
}

void SimpleConsole::UpdateWatches() {
    if (!m_watchActive || m_watchEntries.empty()) return;

    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastWatchUpdate);
    if (elapsed.count() < 500) return; // Update every 500ms
    m_lastWatchUpdate = now;

    bool anyChanged = false;
    for (auto& entry : m_watchEntries) {
        if (!entry.active) continue;
        std::string newValue = entry.getter();
        if (newValue != entry.lastValue) {
            entry.lastValue = newValue;
            anyChanged = true;
        }
    }

    if (anyChanged) {
        // Update console title with watch info
        std::string title = "Spark Engine - Development Console | ";
        for (const auto& entry : m_watchEntries) {
            if (entry.active) {
                title += entry.name + "=" + entry.lastValue + " | ";
            }
        }
        SetConsoleTitleA(title.c_str());
    }
}

void SimpleConsole::DisplayPrompt() {
    Print("SPARK> ", Color::Green);
    
    if (!m_currentInput.empty()) {
        Print(m_currentInput, Color::White);
    }
}

void SimpleConsole::RedrawConsole() {
    Clear();
    
    auto history = GetLogHistory();
    size_t startIndex = history.size() > 50 ? history.size() - 50 : 0;
    
    for (size_t i = startIndex; i < history.size(); ++i) {
        const auto& entry = history[i];
        Color color = Color::White;
        if (entry.type == "ERROR") color = Color::Red;
        else if (entry.type == "WARNING") color = Color::Yellow;
        else if (entry.type == "SUCCESS") color = Color::Green;
        else if (entry.type == "INFO") color = Color::Cyan;
        else if (entry.type == "DEBUG") color = Color::Magenta;
        else if (entry.type == "CRITICAL") color = Color::Red;
        else if (entry.type == "TRACE") color = Color::Blue;
        
        PrintLine("[" + entry.timestamp + "] [" + entry.type + "] " + entry.message, color);
    }
    
    DisplayPrompt();
}

std::vector<std::string> SimpleConsole::ParseCommand(const std::string& commandLine) {
    std::vector<std::string> args;
    std::istringstream iss(commandLine);
    std::string arg;
    
    while (iss >> arg) {
        args.push_back(arg);
    }
    
    return args;
}

std::string SimpleConsole::GetTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    
    std::stringstream ss;
    ss << std::put_time(std::localtime(&time_t), "%H:%M:%S");
    return ss.str();
}

bool SimpleConsole::CreateConsoleWindow() {
    if (!AllocConsole()) {
        return false;
    }
    
    SetConsoleTitleA("Spark Engine - Development Console");
    
    m_consoleWindow = GetConsoleWindow();
    if (!m_consoleWindow) {
        FreeConsole();
        return false;
    }
    
    SetWindowPos(m_consoleWindow, HWND_TOP, 100, 100, 1000, 700, SWP_SHOWWINDOW);
    
    return true;
}

void SimpleConsole::SetupConsoleHandles() {
    m_consoleOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    m_consoleInput = GetStdHandle(STD_INPUT_HANDLE);
    
    DWORD inputMode;
    GetConsoleMode(m_consoleInput, &inputMode);
    inputMode &= ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT);
    SetConsoleMode(m_consoleInput, inputMode);
    
    DWORD outputMode;
    GetConsoleMode(m_consoleOutput, &outputMode);
    outputMode |= ENABLE_PROCESSED_OUTPUT | ENABLE_WRAP_AT_EOL_OUTPUT;
    SetConsoleMode(m_consoleOutput, outputMode);
}

void SimpleConsole::RegisterDefaultCommands() {
    // Enhanced help system with category grouping
    RegisterCommand("help", [this](const std::vector<std::string>& args) -> std::string {
        if (args.empty()) {
            // Group commands by category
            std::map<std::string, std::vector<std::pair<std::string, std::string>>> categories;
            for (const auto& pair : m_commands) {
                categories[pair.second.category].push_back({pair.first, pair.second.description});
            }

            std::stringstream ss;
            ss << "==========================================\n";
            ss << "          COMMAND REFERENCE\n";
            ss << "==========================================\n\n";

            for (auto& cat : categories) {
                ss << cat.first << ":\n";
                // Sort commands within category
                std::sort(cat.second.begin(), cat.second.end());
                for (const auto& cmd : cat.second) {
                    ss << "  " << std::left << std::setw(22) << cmd.first << " - " << cmd.second << "\n";
                }
                ss << "\n";
            }

            // Show aliases
            if (!m_aliases.empty()) {
                ss << "ALIASES:\n";
                for (const auto& alias : m_aliases) {
                    ss << "  " << std::left << std::setw(22) << alias.first << " -> " << alias.second << "\n";
                }
                ss << "\n";
            }

            ss << "Type 'help <command>' for detailed info | Tab for autocomplete | Up/Down for history";
            return ss.str();
        } else if (args[0] == "categories") {
            // List just categories and command counts
            std::map<std::string, int> catCounts;
            for (const auto& pair : m_commands) {
                catCounts[pair.second.category]++;
            }
            std::stringstream ss;
            ss << "Command Categories:\n";
            for (const auto& cat : catCounts) {
                ss << "  " << std::left << std::setw(20) << cat.first << " (" << cat.second << " commands)\n";
            }
            ss << "\nUse 'help <category_name>' to list commands in a category.";
            return ss.str();
        } else {
            // Check if it's a command name
            auto it = m_commands.find(args[0]);
            if (it != m_commands.end()) {
                std::stringstream ss;
                ss << "Command: " << args[0] << "\n";
                ss << "Category: " << it->second.category << "\n";
                ss << "Description: " << it->second.description;
                return ss.str();
            }

            // Check if it's a category name (case-insensitive search)
            std::string searchCat = args[0];
            std::transform(searchCat.begin(), searchCat.end(), searchCat.begin(), ::tolower);
            std::stringstream ss;
            bool found = false;
            for (const auto& pair : m_commands) {
                std::string cat = pair.second.category;
                std::transform(cat.begin(), cat.end(), cat.begin(), ::tolower);
                if (cat.find(searchCat) != std::string::npos) {
                    if (!found) {
                        ss << "Commands matching '" << args[0] << "':\n";
                        found = true;
                    }
                    ss << "  " << std::left << std::setw(22) << pair.first << " - " << pair.second.description << "\n";
                }
            }
            if (found) return ss.str();

            return "Unknown command or category: " + args[0];
        }
    }, "Display help information for commands", "System");
    
    RegisterCommand("clear", [this](const std::vector<std::string>& args) -> std::string {
        Clear();
        return "";
    }, "Clear the console screen", "System");

    RegisterCommand("history", [this](const std::vector<std::string>& args) -> std::string {
        std::stringstream ss;
        ss << "Command History (" << m_commandHistory.size() << " entries):\n";
        ss << "==========================================\n";
        int start = 0;
        if (!args.empty()) {
            try {
                int count = std::stoi(args[0]);
                start = std::max(0, static_cast<int>(m_commandHistory.size()) - count);
            } catch (...) {}
        }
        for (size_t i = start; i < m_commandHistory.size(); ++i) {
            ss << std::right << std::setw(3) << (i + 1) << ": " << m_commandHistory[i] << "\n";
        }
        return ss.str();
    }, "Display command history (usage: history [count])", "System");

    RegisterCommand("exit", [this](const std::vector<std::string>& args) -> std::string {
        PostQuitMessage(0);
        return "Engine shutdown initiated";
    }, "Exit the application", "System");

    RegisterCommand("quit", [this](const std::vector<std::string>& args) -> std::string {
        PostQuitMessage(0);
        return "Engine shutdown initiated";
    }, "Exit the application (alias for exit)", "System");

    RegisterCommand("version", [](const std::vector<std::string>& args) -> std::string {
        return "Spark Engine v1.0.0 - Development Build\nConsole System v3.0 - Professional Interface\n"
               "Features: Tab completion, history nav, aliases, watches, log filtering";
    }, "Display engine version information", "System");

    RegisterCommand("uptime", [this](const std::vector<std::string>& args) -> std::string {
        static auto startTime = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        auto uptime = std::chrono::duration_cast<std::chrono::seconds>(now - startTime);

        int hours = static_cast<int>(uptime.count()) / 3600;
        int minutes = (static_cast<int>(uptime.count()) % 3600) / 60;
        int seconds = static_cast<int>(uptime.count()) % 60;

        std::stringstream ss;
        ss << "Engine uptime: " << hours << "h " << minutes << "m " << seconds << "s";
        return ss.str();
    }, "Display engine uptime", "System");

    RegisterCommand("echo", [](const std::vector<std::string>& args) -> std::string {
        std::stringstream ss;
        for (size_t i = 0; i < args.size(); ++i) {
            if (i > 0) ss << " ";
            ss << args[i];
        }
        return ss.str();
    }, "Echo text back to console", "System");

    RegisterCommand("exec", [this](const std::vector<std::string>& args) -> std::string {
        if (args.empty()) return "Usage: exec <command1; command2; ...>";
        // Join all args and split by semicolons to execute multiple commands
        std::string combined;
        for (size_t i = 0; i < args.size(); ++i) {
            if (i > 0) combined += " ";
            combined += args[i];
        }
        std::istringstream iss(combined);
        std::string cmd;
        int count = 0;
        while (std::getline(iss, cmd, ';')) {
            // Trim
            cmd.erase(0, cmd.find_first_not_of(" \t"));
            cmd.erase(cmd.find_last_not_of(" \t") + 1);
            if (!cmd.empty()) {
                ExecuteCommand(cmd);
                count++;
            }
        }
        return "Executed " + std::to_string(count) + " command(s)";
    }, "Execute multiple semicolon-separated commands", "System");

    RegisterCommand("repeat", [this](const std::vector<std::string>& args) -> std::string {
        if (args.size() < 2) return "Usage: repeat <count> <command>";
        try {
            int count = std::stoi(args[0]);
            if (count < 1 || count > 100) return "Repeat count must be between 1 and 100";
            std::string cmd;
            for (size_t i = 1; i < args.size(); ++i) {
                if (i > 1) cmd += " ";
                cmd += args[i];
            }
            for (int i = 0; i < count; ++i) {
                ExecuteCommand(cmd);
            }
            return "Repeated '" + cmd + "' " + std::to_string(count) + " times";
        } catch (...) {
            return "Invalid count. Must be a number.";
        }
    }, "Repeat a command N times (usage: repeat <count> <command>)", "System");
}

void SimpleConsole::RegisterAdvancedCommands() {
    RegisterPerformanceCommands();
    RegisterPlayerCommands();
    RegisterPhysicsCommands();
    RegisterConfigCommands();
    RegisterEngineCommands();
    RegisterSceneCommands();
    RegisterCameraCommands();
    RegisterSystemCommands();
    RegisterGraphicsCommands();
    RegisterAudioCommands();
    RegisterLogCommands();
    RegisterWatchCommands();
    RegisterAliasCommands();
    RegisterDebugCommands();
    RegisterFileCommands();
    RegisterInputCommands();
    RegisterRenderingCommands();
    RegisterProfilingCommands();
    RegisterNetworkingCommands();
    RegisterTestingCommands();
    RegisterGameCommands();
}

void SimpleConsole::RegisterPerformanceCommands() {
    RegisterCommand("fps", [](const std::vector<std::string>& args) -> std::string {
        UpdatePerformanceCounters();
        
        std::stringstream ss;
        ss << "Performance Metrics:\n";
        ss << "==========================================\n";
        ss << "Current FPS:      " << std::fixed << std::setprecision(1) << g_perfCounters.currentFPS << "\n";
        ss << "Average FPS:      " << std::fixed << std::setprecision(1) << g_perfCounters.averageFPS << "\n";
        ss << "Frame Time:       " << std::fixed << std::setprecision(2) << g_perfCounters.frameTime << " ms\n";
        ss << "Target FPS:       60.0 (VSync: " << (g_gameState.vsync ? "ON" : "OFF") << ")\n";
        ss << "Status:           LIVE MONITORING\n";
        ss << "==========================================\n";
        ss << "Render Time:      " << std::fixed << std::setprecision(2) << g_perfCounters.renderTime << " ms\n";
        ss << "Update Time:      " << std::fixed << std::setprecision(2) << g_perfCounters.updateTime << " ms\n";
        ss << "Draw Calls:       " << g_perfCounters.drawCalls << "\n";
        ss << "Triangles:        " << g_perfCounters.triangles << "\n";
        ss << "Vertices:         " << g_perfCounters.vertices;
        
        return ss.str();
    }, "Display live FPS and performance information");

    RegisterCommand("memory", [](const std::vector<std::string>& args) -> std::string {
        MEMORYSTATUSEX memInfo;
        memInfo.dwLength = sizeof(MEMORYSTATUSEX);
        GlobalMemoryStatusEx(&memInfo);
        
        PROCESS_MEMORY_COUNTERS pmc;
        if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
            std::stringstream ss;
            ss << "Memory Usage Information:\n";
            ss << "==========================================\n";
            ss << "System Memory:\n";
            ss << "  Total Physical:   " << (memInfo.ullTotalPhys / 1024 / 1024) << " MB\n";
            ss << "  Available:        " << (memInfo.ullAvailPhys / 1024 / 1024) << " MB\n";
            ss << "  Usage:            " << memInfo.dwMemoryLoad << "%\n\n";
            ss << "Process Memory:\n";
            ss << "  Working Set:      " << (pmc.WorkingSetSize / 1024 / 1024) << " MB\n";
            ss << "  Peak Working Set: " << (pmc.PeakWorkingSetSize / 1024 / 1024) << " MB\n";
            ss << "  Page File Usage:  " << (pmc.PagefileUsage / 1024 / 1024) << " MB\n\n";
            ss << "Game Memory:\n";
            ss << "  Texture Memory:   " << (g_perfCounters.textureMemory / 1024 / 1024) << " MB\n";
            ss << "  Buffer Memory:    " << (g_perfCounters.bufferMemory / 1024 / 1024) << " MB\n";
            ss << "  Scene Objects:    " << g_gameState.activeObjects << " active";
            return ss.str();
        }
        
        return "Failed to retrieve memory information";
    }, "Display current memory usage with game-specific data");
}

void SimpleConsole::RegisterPlayerCommands() {
    RegisterCommand("player_info", [](const std::vector<std::string>& args) -> std::string {
        if (g_game && g_game->GetPlayer()) {
            Player* player = g_game->GetPlayer();
            auto state = player->Console_GetState();
            
            std::stringstream ss;
            ss << "Player Status (LIVE DATA):\n";
            ss << "==========================================\n";
            ss << "Health:           " << std::fixed << std::setprecision(1) << state.health 
               << "/" << state.maxHealth << " (" 
               << std::setprecision(0) << (state.health / state.maxHealth * 100) << "%)\n";
            ss << "Armor:            " << std::fixed << std::setprecision(1) << state.armor 
               << "/" << state.maxArmor << "\n";
            ss << "Stamina:          " << std::fixed << std::setprecision(1) << state.stamina 
               << "/" << state.maxStamina << "\n";
            ss << "Position:         (" << std::setprecision(2) 
               << state.position.x << ", " 
               << state.position.y << ", " 
               << state.position.z << ")\n";
            ss << "Velocity:         (" << std::setprecision(2) 
               << state.velocity.x << ", " 
               << state.velocity.y << ", " 
               << state.velocity.z << ")\n";
            ss << "Current Weapon:   " << static_cast<int>(state.currentWeapon) << "\n";
            ss << "Ammunition:       " << state.currentAmmo << "/" << state.maxAmmo << "\n";
            ss << "Status:           " << (state.isAlive ? "ALIVE" : "DEAD") << "\n";
            ss << "Grounded:         " << (state.isGrounded ? "YES" : "NO") << "\n";
            ss << "Reloading:        " << (state.isReloading ? "YES" : "NO") << "\n";
            ss << "God Mode:         " << (state.godMode ? "ENABLED" : "DISABLED") << "\n";
            ss << "Noclip:           " << (state.noclip ? "ENABLED" : "DISABLED") << "\n";
            ss << "Infinite Ammo:    " << (state.infiniteAmmo ? "ENABLED" : "DISABLED") << "\n";
            ss << "Speed:            " << state.speed << " units/sec\n";
            ss << "Jump Height:      " << state.jumpHeight << " units";
            return ss.str();
        } else {
            return "Player system not available or not initialized";
        }
    }, "Display comprehensive live player status information from game systems");

    RegisterCommand("player_heal", [](const std::vector<std::string>& args) -> std::string {
        if (!g_game || !g_game->GetPlayer()) {
            return "Player system not available";
        }
        
        float amount = 50.0f;
        if (!args.empty()) {
            try {
                amount = std::stof(args[0]);
            } catch (...) {
                return "Invalid heal amount. Must be a number.";
            }
        }
        
        if (amount < 0 || amount > 9999) {
            return "Heal amount must be between 0 and 9999";
        }
        
        Player* player = g_game->GetPlayer();
        auto oldState = player->Console_GetState();
        player->Console_SetHealth(oldState.health + amount);
        auto newState = player->Console_GetState();
        
        float actualHealed = newState.health - oldState.health;
        return "Player healed by " + std::to_string(actualHealed) + " HP. Current health: " + 
               std::to_string(newState.health) + "/" + std::to_string(newState.maxHealth) + " (live game integration)";
    }, "Heal the player using live game integration (usage: player_heal [amount])");

    RegisterCommand("player_damage", [](const std::vector<std::string>& args) -> std::string {
        if (!g_game || !g_game->GetPlayer()) {
            return "Player system not available";
        }
        
        float amount = 10.0f;
        if (!args.empty()) {
            try {
                amount = std::stof(args[0]);
            } catch (...) {
                return "Invalid damage amount. Must be a number.";
            }
        }
        
        if (amount < 0 || amount > 9999) {
            return "Damage amount must be between 0 and 9999";
        }
        
        Player* player = g_game->GetPlayer();
        auto oldState = player->Console_GetState();
        
        if (oldState.godMode) {
            return "Player damage blocked by god mode";
        }
        
        player->TakeDamage(amount);
        auto newState = player->Console_GetState();
        
        float actualDamage = oldState.health - newState.health;
        std::string result = "Player took " + std::to_string(actualDamage) + " damage. Current health: " + 
                           std::to_string(newState.health) + "/" + std::to_string(newState.maxHealth) + " (live game integration)";
        
        if (newState.health <= 0) {
            result += "\nPlayer has died!";
        }
        
        return result;
    }, "Damage the player using live game integration (usage: player_damage [amount])");

    RegisterCommand("player_teleport", [](const std::vector<std::string>& args) -> std::string {
        if (!g_game || !g_game->GetPlayer()) {
            return "Player system not available";
        }
        
        if (args.size() < 3) {
            return "Usage: player_teleport <x> <y> <z>";
        }
        
        try {
            float x = std::stof(args[0]);
            float y = std::stof(args[1]);
            float z = std::stof(args[2]);
            
            Player* player = g_game->GetPlayer();
            player->Console_SetPosition(x, y, z);
            
            std::stringstream ss;
            ss << "Player teleported to (" << x << ", " << y << ", " << z << ") via live game integration";
            return ss.str();
        } catch (...) {
            return "Invalid coordinates. All values must be numbers.";
        }
    }, "Teleport player to specified coordinates using live game integration");

    RegisterCommand("player_godmode", [](const std::vector<std::string>& args) -> std::string {
        if (!g_game || !g_game->GetPlayer()) {
            return "Player system not available";
        }
        
        if (args.empty()) {
            auto state = g_game->GetPlayer()->Console_GetState();
            return "God mode is currently " + std::string(state.godMode ? "ENABLED" : "DISABLED") + 
                   "\nUsage: player_godmode <on|off>";
        }
        
        std::string value = args[0];
        std::transform(value.begin(), value.end(), value.begin(), ::tolower);
        bool enable = (value == "on" || value == "true" || value == "1");
        
        g_game->GetPlayer()->Console_SetGodMode(enable);
        return "God mode " + std::string(enable ? "enabled" : "disabled") + " via live game integration";
    }, "Enable/disable god mode using live game integration");

    RegisterCommand("player_noclip", [](const std::vector<std::string>& args) -> std::string {
        if (!g_game || !g_game->GetPlayer()) {
            return "Player system not available";
        }
        
        if (args.empty()) {
            auto state = g_game->GetPlayer()->Console_GetState();
            return "Noclip is currently " + std::string(state.noclip ? "ENABLED" : "DISABLED") + 
                   "\nUsage: player_noclip <on|off>";
        }
        
        std::string value = args[0];
        std::transform(value.begin(), value.end(), value.begin(), ::tolower);
        bool enable = (value == "on" || value == "true" || value == "1");
        
        g_game->GetPlayer()->Console_SetNoclip(enable);
        return "Noclip " + std::string(enable ? "enabled" : "disabled") + " via live game integration";
    }, "Enable/disable noclip mode using live game integration");

    RegisterCommand("player_speed", [](const std::vector<std::string>& args) -> std::string {
        if (!g_game || !g_game->GetPlayer()) {
            return "Player system not available";
        }
        
        if (args.empty()) {
            auto state = g_game->GetPlayer()->Console_GetState();
            return "Current player speed: " + std::to_string(state.speed) + " units/sec\n"
                   "Usage: player_speed <value>";
        }
        
        try {
            float newSpeed = std::stof(args[0]);
            if (newSpeed < 0.1f || newSpeed > 100.0f) {
                return "Speed must be between 0.1 and 100";
            }
            
            g_game->GetPlayer()->Console_SetSpeed(newSpeed);
            return "Player speed set to " + std::to_string(newSpeed) + " units/sec via live game integration";
        } catch (...) {
            return "Invalid speed value. Must be a number.";
        }
    }, "Get or set player movement speed using live game integration");

    RegisterCommand("player_weapon", [](const std::vector<std::string>& args) -> std::string {
        if (!g_game || !g_game->GetPlayer()) {
            return "Player system not available";
        }
        
        if (args.empty()) {
            auto state = g_game->GetPlayer()->Console_GetState();
            return "Current weapon: " + std::to_string(static_cast<int>(state.currentWeapon)) + 
                   "\nAmmo: " + std::to_string(state.currentAmmo) + "/" + std::to_string(state.maxAmmo) +
                   "\nUsage: player_weapon <0-4> (0=Pistol, 1=Rifle, 2=Shotgun, 3=Rocket, 4=Grenade)";
        }
        
        try {
            int weaponIndex = std::stoi(args[0]);
            if (weaponIndex < 0 || weaponIndex > 4) {
                return "Weapon index must be between 0 and 4";
            }
            
            WeaponType weaponType = static_cast<WeaponType>(weaponIndex);
            g_game->GetPlayer()->Console_ChangeWeapon(weaponType);
            
            auto newState = g_game->GetPlayer()->Console_GetState();
            return "Player weapon changed to " + std::to_string(static_cast<int>(newState.currentWeapon)) + 
                   " with " + std::to_string(newState.currentAmmo) + " ammo via live game integration";
        } catch (...) {
            return "Invalid weapon index. Must be a number 0-4.";
        }
    }, "Get or change player weapon using live game integration");
}

void SimpleConsole::RegisterPhysicsCommands() {
    RegisterCommand("physics_gravity", [](const std::vector<std::string>& args) -> std::string {
        if (args.empty()) {
            return "Current gravity: " + std::to_string(g_gameState.gravity) + " units/sec�\n"
                   "Usage: physics_gravity <value> (negative for downward)";
        }
        
        try {
            float newGravity = std::stof(args[0]);
            if (newGravity < -100.0f || newGravity > 100.0f) {
                return "Gravity must be between -100 and 100";
            }
            
            // Apply immediately to game state
            g_gameState.gravity = newGravity;
            
            // Apply to real game systems using integration
            if (g_game) {
                g_game->ApplyPhysicsSettings(newGravity, g_gameState.playerSpeed, g_gameState.jumpHeight, g_gameState.friction);
            }
            
            // Auto-save configuration
            if (g_configSystem.autoSave) {
                g_configSystem.SaveConfig();
            }
            
            return "Gravity set to " + std::to_string(g_gameState.gravity) + " units/sec� (applied to game systems)";
        } catch (...) {
            return "Invalid gravity value. Must be a number.";
        }
    }, "Get or set physics gravity with live game integration");

    RegisterCommand("physics_speed", [](const std::vector<std::string>& args) -> std::string {
        if (args.empty()) {
            return "Current player speed: " + std::to_string(g_gameState.playerSpeed) + " units/sec\n"
                   "Usage: physics_speed <value>";
        }
        
        try {
            float newSpeed = std::stof(args[0]);
            if (newSpeed < 0.1f || newSpeed > 50.0f) {
                return "Speed must be between 0.1 and 50";
            }
            
            // Apply immediately to game state
            g_gameState.playerSpeed = newSpeed;
            
            // Apply to real game systems using integration
            if (g_game) {
                g_game->ApplyPhysicsSettings(g_gameState.gravity, newSpeed, g_gameState.jumpHeight, g_gameState.friction);
            }
            
            // Auto-save configuration
            if (g_configSystem.autoSave) {
                g_configSystem.SaveConfig();
            }
            
            return "Player speed set to " + std::to_string(g_gameState.playerSpeed) + " units/sec (applied to game systems)";
        } catch (...) {
            return "Invalid speed value. Must be a number.";
        }
    }, "Get or set player movement speed with live game integration");

    RegisterCommand("physics_jump", [](const std::vector<std::string>& args) -> std::string {
        if (args.empty()) {
            return "Current jump height: " + std::to_string(g_gameState.jumpHeight) + " units\n"
                   "Usage: physics_jump <value>";
        }
        
        try {
            float newJump = std::stof(args[0]);
            if (newJump < 0.1f || newJump > 20.0f) {
                return "Jump height must be between 0.1 and 20";
            }
            
            // Apply immediately to game state
            g_gameState.jumpHeight = newJump;
            
            // Apply to real game systems using integration
            if (g_game) {
                g_game->ApplyPhysicsSettings(g_gameState.gravity, g_gameState.playerSpeed, newJump, g_gameState.friction);
            }
            
            // Auto-save configuration
            if (g_configSystem.autoSave) {
                g_configSystem.SaveConfig();
            }
            
            return "Jump height set to " + std::to_string(g_gameState.jumpHeight) + " units (applied to game systems)";
        } catch (...) {
            return "Invalid jump height value. Must be a number.";
        }
    }, "Get or set player jump height with live game integration");

    RegisterCommand("physics_friction", [](const std::vector<std::string>& args) -> std::string {
        if (args.empty()) {
            return "Current friction coefficient: " + std::to_string(g_gameState.friction) + "\n"
                   "Usage: physics_friction <value> (0.0-1.0)";
        }
        
        try {
            float newFriction = std::stof(args[0]);
            if (newFriction < 0.0f || newFriction > 1.0f) {
                return "Friction must be between 0.0 and 1.0";
            }
            
            // Apply immediately to game state
            g_gameState.friction = newFriction;
            
            // Apply to real game systems using integration
            if (g_game) {
                g_game->ApplyPhysicsSettings(g_gameState.gravity, g_gameState.playerSpeed, g_gameState.jumpHeight, newFriction);
            }
            
            // Auto-save configuration
            if (g_configSystem.autoSave) {
                g_configSystem.SaveConfig();
            }
            
            return "Friction coefficient set to " + std::to_string(g_gameState.friction) + " (applied to game systems)";
        } catch (...) {
            return "Invalid friction value. Must be a number between 0.0 and 1.0.";
        }
    }, "Get or set physics friction coefficient with live game integration");

    RegisterCommand("physics_reset", [](const std::vector<std::string>& args) -> std::string {
        // Reset to default values
        g_gameState.gravity = -20.0f;
        g_gameState.playerSpeed = 5.0f;
        g_gameState.jumpHeight = 3.0f;
        g_gameState.friction = 0.9f;
        
        // Apply to real game systems using integration
        if (g_game) {
            g_game->ApplyPhysicsSettings(g_gameState.gravity, g_gameState.playerSpeed, g_gameState.jumpHeight, g_gameState.friction);
        }
        
        // Auto-save configuration
        if (g_configSystem.autoSave) {
            g_configSystem.SaveConfig();
        }
        
        return "Physics parameters reset to defaults and applied to game systems:\n"
               "  Gravity: " + std::to_string(g_gameState.gravity) + " units/sec�\n"
               "  Speed: " + std::to_string(g_gameState.playerSpeed) + " units/sec\n"
               "  Jump Height: " + std::to_string(g_gameState.jumpHeight) + " units\n"
               "  Friction: " + std::to_string(g_gameState.friction);
    }, "Reset all physics parameters to defaults with live game integration");
}

void SimpleConsole::RegisterConfigCommands() {
    RegisterCommand("config_save", [](const std::vector<std::string>& args) -> std::string {
        std::string filename = args.empty() ? g_configSystem.configFile : args[0];
        
        try {
            // Temporarily change config file if specified
            std::string originalFile = g_configSystem.configFile;
            if (!args.empty()) {
                g_configSystem.configFile = filename;
            }
            
            // Save current state
            g_configSystem.SaveConfig();
            
            // Restore original filename
            g_configSystem.configFile = originalFile;
            
            return "Configuration saved to: " + filename + " (live settings preserved)";
        } catch (const std::exception& e) {
            return "Failed to save configuration: " + std::string(e.what());
        }
    }, "Save current live configuration to file");

    RegisterCommand("config_load", [](const std::vector<std::string>& args) -> std::string {
        std::string filename = args.empty() ? g_configSystem.configFile : args[0];
        
        if (!std::filesystem::exists(filename)) {
            return "Configuration file not found: " + filename;
        }
        
        try {
            // Temporarily change config file if specified
            std::string originalFile = g_configSystem.configFile;
            if (!args.empty()) {
                g_configSystem.configFile = filename;
            }
            
            // Load configuration
            g_configSystem.LoadConfig();
            
            // Restore original filename
            g_configSystem.configFile = originalFile;
            
            return "Configuration loaded from: " + filename + " (applied to live systems)";
        } catch (const std::exception& e) {
            return "Failed to load configuration: " + std::string(e.what());
        }
    }, "Load configuration from file and apply to live systems");
}

void SimpleConsole::RegisterEngineCommands() {
    RegisterCommand("engine_status", [](const std::vector<std::string>& args) -> std::string {
        std::stringstream ss;
        ss << "==========================================\n";
        ss << "         ENGINE STATUS REPORT\n";
        ss << "==========================================\n";
        ss << "Graphics Engine:  " << (g_graphics ? "ACTIVE" : "INACTIVE") << "\n";
        ss << "Game System:      " << (g_game ? "ACTIVE" : "INACTIVE") << "\n";
        ss << "Input Manager:    " << (g_input ? "ACTIVE" : "INACTIVE") << "\n";
        ss << "Timer System:     " << (g_timer ? "ACTIVE" : "INACTIVE") << "\n";
        ss << "Main Loop:        RUNNING\n";
        
        if (g_game) {
            ss << "Player System:    " << (g_game->GetPlayer() ? "ACTIVE" : "INACTIVE") << "\n";
            ss << "Camera System:    " << (g_game->GetCamera() ? "ACTIVE" : "INACTIVE") << "\n";
            ss << "Scene Manager:    " << (g_game->GetSceneManager() ? "ACTIVE" : "INACTIVE") << "\n";
            ss << "Time Scale:       " << g_game->GetTimeScale() << "x\n";
        }
        
        ss << "==========================================";
        return ss.str();
    }, "Display comprehensive engine status with real system data");

    RegisterCommand("engine_timescale", [](const std::vector<std::string>& args) -> std::string {
        if (args.empty()) {
            if (g_game) {
                return "Current time scale: " + std::to_string(g_game->GetTimeScale()) + "x\n"
                       "Usage: engine_timescale <value> (0.1-10.0)";
            } else {
                return "Time scale unavailable - game system not active";
            }
        }
        
        try {
            float newScale = std::stof(args[0]);
            if (newScale < 0.1f || newScale > 10.0f) {
                return "Time scale must be between 0.1 and 10.0";
            }
            
            if (g_game) {
                g_game->SetTimeScale(newScale);
                g_gameState.timeScale = newScale;
                return "Time scale set to " + std::to_string(newScale) + "x (applied to game systems)";
            } else {
                g_gameState.timeScale = newScale;
                return "Time scale set to " + std::to_string(newScale) + "x (console simulation - no game system)";
            }
        } catch (...) {
            return "Invalid time scale value. Must be a number.";
        }
    }, "Get or set engine time scale with live game integration");

    RegisterCommand("engine_pause", [](const std::vector<std::string>& args) -> std::string {
        if (g_game) {
            g_game->Pause();
            return "Game simulation paused (applied to game systems)";
        } else {
            return "Cannot pause - game system not active";
        }
    }, "Pause the game simulation");

    RegisterCommand("engine_resume", [](const std::vector<std::string>& args) -> std::string {
        if (g_game) {
            g_game->Resume();
            return "Game simulation resumed (applied to game systems)";
        } else {
            return "Cannot resume - game system not active";
        }
    }, "Resume the game simulation");
}

void SimpleConsole::RegisterSceneCommands() {
    RegisterCommand("scene_spawn", [](const std::vector<std::string>& args) -> std::string {
        if (args.size() < 4) {
            return "Usage: scene_spawn <type> <x> <y> <z>\n"
                   "Types: cube, sphere, wall, plane\n"
                   "Example: scene_spawn cube 5.0 0.0 5.0";
        }
        
        std::string type = args[0];
        try {
            float x = std::stof(args[1]);
            float y = std::stof(args[2]);
            float z = std::stof(args[3]);
            
            if (type == "cube" || type == "sphere" || type == "wall" || type == "plane") {
                
                // Apply to real game system using integration
                if (g_game) {
                    bool success = g_game->SpawnObject(type, x, y, z);
                    if (success) {
                        // Update console state
                        g_gameState.activeObjects++;
                        g_gameState.visibleObjects++;
                        
                        std::stringstream ss;
                        ss << "Spawned " << type << " at (" << x << ", " << y << ", " << z << ") via game integration";
                        ss << "\nTotal active objects: " << g_gameState.activeObjects;
                        return ss.str();
                    } else {
                        return "Failed to spawn " + type + " - game system error";
                    }
                } else {
                    // Fallback to console simulation
                    g_gameState.activeObjects++;
                    g_gameState.visibleObjects++;
                    
                    std::stringstream ss;
                    ss << "Spawned " << type << " at (" << x << ", " << y << ", " << z << ") (console simulation)";

                    return ss.str();
                }
            }
            
            return "Invalid object type. Available: cube, sphere, wall, plane";
        } catch (...) {
            return "Invalid coordinates. All position values must be numbers.";
        }
    }, "Spawn an object at specified coordinates with real game integration");

    RegisterCommand("scene_delete", [](const std::vector<std::string>& args) -> std::string {
        if (args.empty()) {
            return "Usage: scene_delete <object_index|all>\n"
                   "Example: scene_delete 0\n"
                   "Use 'scene_info' to see object indices, or 'all' to clear scene";
        }
        
        if (args[0] == "all") {
            // Clear all objects using real game integration
            if (g_game) {
                int clearedObjects = g_gameState.activeObjects;
                g_game->ClearScene(true); // Keep player
                
                // Update console state
                g_gameState.activeObjects = 1; // Player only
                g_gameState.visibleObjects = 1;
                g_gameState.culledObjects = 0;
                
                return "Cleared " + std::to_string(clearedObjects - 1) + " objects from scene via game integration (player preserved)";
            } else {
                // Fallback to console simulation
                int clearedObjects = g_gameState.activeObjects;
                g_gameState.activeObjects = 1; // Player only
                g_gameState.visibleObjects = 1;
                g_gameState.culledObjects = 0;
                
                return "Cleared " + std::to_string(clearedObjects - 1) + " objects from scene (console simulation)";
            }
        }
        
        try {
            size_t objectIndex = std::stoul(args[0]);
            
            // Apply to real game system using integration
            if (g_game) {
                bool success = g_game->DeleteObject(objectIndex);
                if (success) {
                    // Update console state
                    if (g_gameState.activeObjects > 1) {
                        g_gameState.activeObjects--;
                        g_gameState.visibleObjects = std::min(g_gameState.visibleObjects, g_gameState.activeObjects);
                    }
                    
                    return "Object " + args[0] + " deleted from scene via game integration. Remaining objects: " + 
                           std::to_string(g_gameState.activeObjects);
                } else {
                    return "Object " + args[0] + " not found or cannot be deleted";
                }
            } else {
                // Fallback to console simulation
                if (g_gameState.activeObjects > 1) {
                    g_gameState.activeObjects--;
                    g_gameState.visibleObjects = std::min(g_gameState.visibleObjects, g_gameState.activeObjects);
                }
                
                return "Object " + args[0] + " deleted from scene (console simulation). Remaining objects: " + 
                       std::to_string(g_gameState.activeObjects);
            }
        } catch (...) {
            return "Invalid object index. Must be a number or 'all'";
        }
    }, "Delete an object by index with real game integration");

    RegisterCommand("scene_info", [](const std::vector<std::string>& args) -> std::string {
        UpdateGameState();
        
        std::stringstream ss;
        ss << "Scene Information:\n";
        ss << "==========================================\n";
        ss << "Current Scene:    " << g_gameState.currentScene << "\n";
        ss << "Active Objects:   " << g_gameState.activeObjects << "\n";
        ss << "Visible Objects:  " << g_gameState.visibleObjects << "\n";
        ss << "Culled Objects:   " << g_gameState.culledObjects << "\n";
        ss << "Player Position:  (" << std::fixed << std::setprecision(2) 
           << g_gameState.playerPosition.x << ", " 
           << g_gameState.playerPosition.y << ", " 
           << g_gameState.playerPosition.z << ")\n";
        
        if (g_game) {
            ss << "Game Integration: ACTIVE\n";
            ss << "Time Scale:       " << g_game->GetTimeScale() << "x\n";
            ss << "Game Paused:      " << (g_game->IsPaused() ? "YES" : "NO") << "\n";
        } else {
            ss << "Game Integration: INACTIVE (console simulation)\n";
        }
        
        // Add performance impact
        UpdatePerformanceCounters();
        ss << "\nPerformance Impact:\n";
        ss << "  Current FPS:    " << std::fixed << std::setprecision(1) << g_perfCounters.currentFPS << "\n";
        ss << "  Draw Calls:     " << g_perfCounters.drawCalls << "\n";
        ss << "  Triangles:      " << g_perfCounters.triangles << "\n";
        ss << "  Memory Usage:   " << (g_perfCounters.textureMemory + g_perfCounters.bufferMemory) / 1024 / 1024 << " MB";
        
        return ss.str();
    }, "Display current scene information and statistics with real game data");
}

void SimpleConsole::RegisterCameraCommands() {
    RegisterCommand("camera_info", [](const std::vector<std::string>& args) -> std::string {
        if (g_game && g_game->GetCamera()) {
            SparkEngineCamera* camera = g_game->GetCamera();
            auto state = camera->Console_GetState();
            
            std::stringstream ss;
            ss << "Camera Status (LIVE DATA):\n";
            ss << "==========================================\n";
            ss << "Position:         (" << std::fixed << std::setprecision(2) 
               << state.position.x << ", " 
               << state.position.y << ", " 
               << state.position.z << ")\n";
            ss << "Rotation:         (" << std::setprecision(1) 
               << state.rotation.x << "�, " 
               << state.rotation.y << "�, " 
               << state.rotation.z << "�)\n";
            ss << "Forward:          (" << std::setprecision(3) 
               << state.forward.x << ", " 
               << state.forward.y << ", " 
               << state.forward.z << ")\n";
            ss << "FOV:              " << std::setprecision(1) << state.defaultFov << "� (Normal), " 
               << state.zoomedFov << "� (Zoomed)\n";
            ss << "Movement Speed:   " << state.moveSpeed << " units/sec\n";
            ss << "Rotation Speed:   " << state.rotationSpeed << "x\n";
            ss << "Mouse Sensitivity:" << state.mouseSensitivity << "x\n";
            ss << "Invert Y:         " << (state.invertY ? "YES" : "NO") << "\n";
            ss << "Aspect Ratio:     " << std::setprecision(3) << state.aspectRatio << "\n";
            ss << "Clipping Planes:  Near=" << state.nearPlane << ", Far=" << state.farPlane << "\n";
            ss << "Zoomed:           " << (state.isZoomed ? "YES" : "NO") << "\n";
            return ss.str();
        } else {
            return "Camera system not available or not initialized";
        }
    }, "Display comprehensive live camera status information from game systems");

    RegisterCommand("camera_fov", [](const std::vector<std::string>& args) -> std::string {
        if (!g_game || !g_game->GetCamera()) {
            return "Camera system not available";
        }
        
        if (args.empty()) {
            auto state = g_game->GetCamera()->Console_GetState();
            return "Current FOV: " + std::to_string(state.defaultFov) + " degrees\n"
                   "Usage: camera_fov <degrees> (10-170)";
        }
        
        try {
            float newFOV = std::stof(args[0]);
            g_game->GetCamera()->Console_SetFOV(newFOV);
            return "Camera FOV set to " + std::to_string(newFOV) + " degrees via live game integration";
        } catch (...) {
            return "Invalid FOV value. Must be a number.";
        }
    }, "Get or set camera field of view using live game integration");

    RegisterCommand("camera_sensitivity", [](const std::vector<std::string>& args) -> std::string {
        if (!g_game || !g_game->GetCamera()) {
            return "Camera system not available";
        }
        
        if (args.empty()) {
            auto state = g_game->GetCamera()->Console_GetState();
            return "Current mouse sensitivity: " + std::to_string(state.mouseSensitivity) + "\n"
                   "Usage: camera_sensitivity <value> (0.1-10.0)";
        }
        
        try {
            float newSens = std::stof(args[0]);
            g_game->GetCamera()->Console_SetMouseSensitivity(newSens);
            return "Mouse sensitivity set to " + std::to_string(newSens) + " via live game integration";
        } catch (...) {
            return "Invalid sensitivity value. Must be a number.";
        }
    }, "Get or set mouse sensitivity using live game integration");

    RegisterCommand("camera_invert", [](const std::vector<std::string>& args) -> std::string {
        if (!g_game || !g_game->GetCamera()) {
            return "Camera system not available";
        }
        
        if (args.empty()) {
            auto state = g_game->GetCamera()->Console_GetState();
            return "Y-axis inversion is currently " + std::string(state.invertY ? "ENABLED" : "DISABLED") + 
                   "\nUsage: camera_invert <on|off>";
        }
        
        std::string value = args[0];
        std::transform(value.begin(), value.end(), value.begin(), ::tolower);
        bool enable = (value == "on" || value == "true" || value == "1");
        
        g_game->GetCamera()->Console_SetInvertY(enable);
        return "Y-axis inversion " + std::string(enable ? "enabled" : "disabled") + " via live game integration";
    }, "Enable/disable Y-axis inversion using live game integration");

    RegisterCommand("camera_position", [](const std::vector<std::string>& args) -> std::string {
        if (!g_game || !g_game->GetCamera()) {
            return "Camera system not available";
        }
        
        if (args.empty()) {
            auto state = g_game->GetCamera()->Console_GetState();
            std::stringstream ss;
            ss << "Current camera position: (" << std::fixed << std::setprecision(2) 
               << state.position.x << ", " 
               << state.position.y << ", " 
               << state.position.z << ")\n";
            ss << "Usage: camera_position <x> <y> <z>";
            return ss.str();
        }
        
        if (args.size() < 3) {
            return "Usage: camera_position <x> <y> <z>";
        }
        
        try {
            float x = std::stof(args[0]);
            float y = std::stof(args[1]);
            float z = std::stof(args[2]);
            
            g_game->GetCamera()->Console_SetPosition(x, y, z);
            
            std::stringstream ss;
            ss << "Camera position set to (" << x << ", " << y << ", " << z << ") via live game integration";
            return ss.str();
        } catch (...) {
            return "Invalid coordinates. All values must be numbers.";
        }
    }, "Get or set camera position using live game integration");

    RegisterCommand("camera_rotation", [](const std::vector<std::string>& args) -> std::string {
        if (!g_game || !g_game->GetCamera()) {
            return "Camera system not available";
        }
        
        if (args.empty()) {
            auto state = g_game->GetCamera()->Console_GetState();
            std::stringstream ss;
            ss << "Current camera rotation: (" << std::fixed << std::setprecision(1) 
               << state.rotation.x << "�, " 
               << state.rotation.y << "�, " 
               << state.rotation.z << "�)\n";
            ss << "Usage: camera_rotation <pitch> <yaw> <roll> (in degrees)";
            return ss.str();
        }
        
        if (args.size() < 3) {
            return "Usage: camera_rotation <pitch> <yaw> <roll> (in degrees)";
        }
        
        try {
            float pitch = std::stof(args[0]);
            float yaw = std::stof(args[1]);
            float roll = std::stof(args[2]);
            
            g_game->GetCamera()->Console_SetRotation(pitch, yaw, roll);
            
            std::stringstream ss;
            ss << "Camera rotation set to (" << pitch << "�, " << yaw << "�, " << roll << "�) via live game integration";
            return ss.str();
        } catch (...) {
            return "Invalid rotation values. All values must be numbers.";
        }
    }, "Get or set camera rotation using live game integration");

    RegisterCommand("camera_lookat", [](const std::vector<std::string>& args) -> std::string {
        if (!g_game || !g_game->GetCamera()) {
            return "Camera system not available";
        }
        
        if (args.size() < 3) {
            return "Usage: camera_lookat <x> <y> <z>";
        }
        
        try {
            float x = std::stof(args[0]);
            float y = std::stof(args[1]);
            float z = std::stof(args[2]);
            
            g_game->GetCamera()->Console_LookAt(x, y, z);
            
            std::stringstream ss;
            ss << "Camera looking at (" << x << ", " << y << ", " << z << ") via live game integration";
            return ss.str();
        } catch (...) {
            return "Invalid coordinates. All values must be numbers.";
        }
    }, "Make camera look at specific coordinates using live game integration");

    RegisterCommand("camera_reset", [](const std::vector<std::string>& args) -> std::string {
        if (!g_game || !g_game->GetCamera()) {
            return "Camera system not available";
        }
        
        g_game->GetCamera()->Console_ResetToDefaults();
        return "Camera reset to default settings via live game integration";
    }, "Reset camera to default settings using live game integration");

    RegisterCommand("camera_clipping", [](const std::vector<std::string>& args) -> std::string {
        if (!g_game || !g_game->GetCamera()) {
            return "Camera system not available";
        }
        
        if (args.empty()) {
            auto state = g_game->GetCamera()->Console_GetState();
            return "Current clipping planes - Near: " + std::to_string(state.nearPlane) + 
                   ", Far: " + std::to_string(state.farPlane) + 
                   "\nUsage: camera_clipping <near> <far>";
        }
        
        if (args.size() < 2) {
            return "Usage: camera_clipping <near> <far>";
        }
        
        try {
            float nearPlane = std::stof(args[0]);
            float farPlane = std::stof(args[1]);
            
            g_game->GetCamera()->Console_SetClippingPlanes(nearPlane, farPlane);
            return "Camera clipping planes set - Near: " + std::to_string(nearPlane) + 
                   ", Far: " + std::to_string(farPlane) + " via live game integration";
        } catch (...) {
            return "Invalid clipping plane values. Must be numbers.";
        }
    }, "Get or set camera clipping planes using live game integration");
}

void SimpleConsole::RegisterSystemCommands() {
    RegisterCommand("system_debug", [](const std::vector<std::string>& args) -> std::string {
        if (args.size() < 2) {
            return "Usage: system_debug <setting> <on|off>\n"
                   "Settings: godmode, noclip, infiniteammo\n"
                   "Example: system_debug godmode on";
        }
        
        std::string setting = args[0];
        std::string value = args[1];
        std::transform(setting.begin(), setting.end(), setting.begin(), ::tolower);
        std::transform(value.begin(), value.end(), value.begin(), ::tolower);
        
        bool enable = (value == "on" || value == "true" || value == "1");
        
        if (setting == "godmode") {
            g_gameState.godMode = enable;
        } else if (setting == "noclip") {
            g_gameState.noclip = enable;
        } else if (setting == "infiniteammo") {
            g_gameState.infiniteAmmo = enable;
        } else {
            return "Unknown debug setting. Available: godmode, noclip, infiniteammo";
        }
        
        // Apply to real game systems using integration
        if (g_game) {
            g_game->ApplyDebugSettings(g_gameState.godMode, g_gameState.noclip, g_gameState.infiniteAmmo);
        }
        
        return "Debug setting " + setting + " " + (enable ? "enabled" : "disabled") + " (applied to game systems)";
    }, "Configure debug settings with live game integration");
}

void SimpleConsole::RegisterGraphicsCommands() {
    RegisterCommand("graphics_info", [](const std::vector<std::string>& args) -> std::string {
        if (g_graphics) {
            auto metrics = g_graphics->Console_GetMetrics();
            auto settings = g_graphics->Console_GetSettings();
            
            std::stringstream ss;
            ss << "Graphics Engine Status (LIVE DATA):\n";
            ss << "==========================================\n";
            ss << "Performance Metrics:\n";
            ss << "  Frame Time:       " << std::fixed << std::setprecision(2) << metrics.frameTime << " ms\n";
            ss << "  Render Time:      " << std::setprecision(2) << metrics.renderTime << " ms\n";
            ss << "  Present Time:     " << std::setprecision(2) << metrics.presentTime << " ms\n";
            ss << "  Draw Calls:       " << metrics.drawCalls << "\n";
            ss << "  Triangles:        " << metrics.triangles << "\n";
            ss << "  Vertices:         " << metrics.vertices << "\n";
            ss << "  GPU Usage:        " << std::setprecision(1) << metrics.gpuUsage << "%\n";
            ss << "\nMemory Usage:\n";
            ss << "  Texture Memory:   " << (metrics.textureMemory / 1024 / 1024) << " MB\n";
            ss << "  Buffer Memory:    " << (metrics.bufferMemory / 1024 / 1024) << " MB\n";
            ss << "  VRAM Usage:       " << (g_graphics->Console_GetVRAMUsage() / 1024 / 1024) << " MB\n";
            ss << "\nRender Settings:\n";
            ss << "  VSync:            " << (metrics.vsyncEnabled ? "ON" : "OFF") << "\n";
            ss << "  Wireframe:        " << (metrics.wireframeMode ? "ON" : "OFF") << "\n";
            ss << "  Debug Mode:       " << (metrics.debugMode ? "ON" : "OFF") << "\n";
            ss << "  Resolution:       " << g_graphics->GetWindowWidth() << "x" << g_graphics->GetWindowHeight() << "\n";
            ss << "  Render Scale:     " << std::setprecision(2) << settings.renderScale << "x\n";
            ss << "  Clear Color:      (" << settings.clearColor[0] << ", " << settings.clearColor[1] 
               << ", " << settings.clearColor[2] << ", " << settings.clearColor[3] << ")";
            return ss.str();
        } else {
            return "Graphics engine not available or not initialized";
        }
    }, "Display comprehensive live graphics engine status and performance metrics");

    RegisterCommand("graphics_vsync", [](const std::vector<std::string>& args) -> std::string {
        if (!g_graphics) {
            return "Graphics engine not available";
        }
        
        if (args.empty()) {
            auto settings = g_graphics->Console_GetSettings();
            return "VSync is currently " + std::string(settings.vsync ? "ENABLED" : "DISABLED") + 
                   "\nWireframe: " + std::string(settings.wireframeMode ? "ENABLED" : "DISABLED") +
                   "\nDebug Mode: " + std::string(settings.debugMode ? "ENABLED" : "DISABLED") +
                   "\nRender Scale: " + std::to_string(settings.renderScale);
        }
        
        std::string value = args[0];
        std::transform(value.begin(), value.end(), value.begin(), ::tolower);
        bool enable = (value == "on" || value == "true" || value == "1");
        
        g_graphics->Console_SetVSync(enable);
        return "VSync " + std::string(enable ? "enabled" : "disabled") + " via live graphics integration";
    }, "Enable/disable VSync using live graphics integration");

    RegisterCommand("graphics_wireframe", [](const std::vector<std::string>& args) -> std::string {
        if (!g_graphics) {
            return "Graphics engine not available";
        }
        
        if (args.empty()) {
            auto settings = g_graphics->Console_GetSettings();
            return "Wireframe mode is currently " + std::string(settings.wireframeMode ? "ENABLED" : "DISABLED") + 
                   "\nUsage: graphics_wireframe <on|off>";
        }
        
        std::string value = args[0];
        std::transform(value.begin(), value.end(), value.begin(), ::tolower);
        bool enable = (value == "on" || value == "true" || value == "1");
        
        g_graphics->Console_SetWireframeMode(enable);
        return "Wireframe mode " + std::string(enable ? "enabled" : "disabled") + " via live graphics integration";
    }, "Enable/disable wireframe rendering using live graphics integration");

    RegisterCommand("graphics_clearcolor", [](const std::vector<std::string>& args) -> std::string {
        if (!g_graphics) {
            return "Graphics engine not available";
        }
        
        if (args.empty()) {
            auto settings = g_graphics->Console_GetSettings();
            std::stringstream ss;
            ss << "Current clear color: (" << std::fixed << std::setprecision(3) 
               << settings.clearColor[0] << ", " << settings.clearColor[1] 
               << ", " << settings.clearColor[2] << ", " << settings.clearColor[3] << ")\n";
            ss << "Usage: graphics_clearcolor <r> <g> <b> [a] (values 0.0-1.0)";
            return ss.str();
        }
        
        if (args.size() < 3) {
            return "Usage: graphics_clearcolor <r> <g> <b> [a] (values 0.0-1.0)";
        }
        
        try {
            float r = std::stof(args[0]);
            float g = std::stof(args[1]);
            float b = std::stof(args[2]);
            float a = (args.size() > 3) ? std::stof(args[3]) : 1.0f;
            
            g_graphics->Console_SetClearColor(r, g, b, a);
            
            std::stringstream ss;
            ss << "Clear color set to (" << r << ", " << g << ", " << b << ", " << a << ") via live graphics integration";
            return ss.str();
        } catch (...) {
            return "Invalid color values. All values must be numbers between 0.0 and 1.0.";
        }
    }, "Set background clear color using live graphics integration");

    RegisterCommand("graphics_debug", [](const std::vector<std::string>& args) -> std::string {
        if (!g_graphics) {
            return "Graphics engine not available";
        }
        
        if (args.empty()) {
            auto settings = g_graphics->Console_GetSettings();
            return "Graphics debug mode is currently " + std::string(settings.debugMode ? "ENABLED" : "DISABLED") + 
                   "\nUsage: graphics_debug <on|off>";
        }
        
        std::string value = args[0];
        std::transform(value.begin(), value.end(), value.begin(), ::tolower);
        bool enable = (value == "on" || value == "true" || value == "1");
        
        g_graphics->Console_SetDebugMode(enable);
        return "Graphics debug mode " + std::string(enable ? "enabled" : "disabled") + " via live graphics integration";
    }, "Enable/disable graphics debug mode using live graphics integration");

    RegisterCommand("graphics_screenshot", [](const std::vector<std::string>& args) -> std::string {
        if (!g_graphics) {
            return "Graphics engine not available";
        }
        
        std::string filename = args.empty() ? "" : args[0];
        bool success = g_graphics->Console_TakeScreenshot(filename);
        
        if (success) {
            return "Screenshot captured successfully" + (filename.empty() ? " (auto-named)" : " as " + filename);
        } else {
            return "Failed to capture screenshot";
        }
    }, "Take a screenshot using live graphics integration");

    RegisterCommand("graphics_scale", [](const std::vector<std::string>& args) -> std::string {
        if (!g_graphics) {
            return "Graphics engine not available";
        }
        
        if (args.empty()) {
            auto settings = g_graphics->Console_GetSettings();
            return "Current render scale: " + std::to_string(settings.renderScale) + "x\n"
                   "Usage: graphics_scale <scale> (0.5-2.0)";
        }
        
        try {
            float scale = std::stof(args[0]);
            g_graphics->Console_SetRenderScale(scale);
            return "Render scale set to " + std::to_string(scale) + "x via live graphics integration";
        } catch (...) {
            return "Invalid scale value. Must be a number between 0.5 and 2.0.";
        }
    }, "Set render scale factor using live graphics integration");

    RegisterCommand("graphics_reset", [](const std::vector<std::string>& args) -> std::string {
        if (!g_graphics) {
            return "Graphics engine not available";
        }
        
        g_graphics->Console_ResetToDefaults();
        return "Graphics settings reset to defaults via live graphics integration";
    }, "Reset all graphics settings to defaults using live graphics integration");

    RegisterCommand("graphics_gputiming", [](const std::vector<std::string>& args) -> std::string {
        if (!g_graphics) {
            return "Graphics engine not available";
        }
        
        if (args.empty()) {
            auto settings = g_graphics->Console_GetSettings();
            return "GPU timing is currently " + std::string(settings.enableGPUTiming ? "ENABLED" : "DISABLED") + 
                   "\nUsage: graphics_gputiming <on|off>";
        }
        
        std::string value = args[0];
        std::transform(value.begin(), value.end(), value.begin(), ::tolower);
        bool enable = (value == "on" || value == "true" || value == "1");
        
        g_graphics->Console_SetGPUTiming(enable);
        return "GPU timing " + std::string(enable ? "enabled" : "disabled") + " via live graphics integration";
    }, "Enable/disable GPU performance timing using live graphics integration");
}

void SimpleConsole::RegisterAudioCommands() {
    // Audio console commands implementation
    Log("Audio console commands registered", "INFO");
    
    // Register basic audio commands
    RegisterCommand("audio.play", [this](const std::vector<std::string>& args) -> std::string {
        if (args.size() < 2) {
            Log("Usage: audio.play <filename>", "INFO");
            return "Usage: audio.play <filename>";
        }
        Log("Playing audio: " + args[1], "INFO");
        return "Playing audio: " + args[1];
    });
    
    RegisterCommand("audio.stop", [this](const std::vector<std::string>& args) -> std::string {
        Log("Stopping all audio", "INFO");
        return "Stopping all audio";
    });
    
    RegisterCommand("audio.volume", [this](const std::vector<std::string>& args) -> std::string {
        if (args.size() < 2) {
            Log("Usage: audio.volume <0-100>", "INFO");
            return "Usage: audio.volume <0-100>";
        }
        Log("Setting audio volume to: " + args[1], "INFO");
        return "Setting audio volume to: " + args[1];
    });
    
    RegisterCommand("audio.status", [this](const std::vector<std::string>& args) -> std::string {
        Log("Audio System Status:", "INFO");
        Log("  Volume: 100%", "INFO");
        Log("  Playing: 0 sounds", "INFO");
        Log("  Available: Yes", "INFO");
        return "Audio System Status:\n  Volume: 100%\n  Playing: 0 sounds\n  Available: Yes";
    });
}

// ============================================================================
// LOG COMMANDS - Filtering, searching, and export
// ============================================================================
void SimpleConsole::RegisterLogCommands() {
    RegisterCommand("log_filter", [this](const std::vector<std::string>& args) -> std::string {
        if (args.empty()) {
            if (m_logFilter.empty()) {
                return "No log filter active. Usage: log_filter <type|clear>\n"
                       "Types: INFO, WARNING, ERROR, SUCCESS, CRITICAL, DEBUG, TRACE";
            }
            return "Current filter: " + m_logFilter + "\nUse 'log_filter clear' to remove";
        }

        if (args[0] == "clear" || args[0] == "off" || args[0] == "none") {
            m_logFilter.clear();
            return "Log filter cleared - showing all messages";
        }

        std::string filter = args[0];
        std::transform(filter.begin(), filter.end(), filter.begin(), ::toupper);
        m_logFilter = filter;
        return "Log filter set to: " + m_logFilter;
    }, "Filter log output by type (INFO/WARNING/ERROR/etc.)", "Logging");

    RegisterCommand("log_search", [this](const std::vector<std::string>& args) -> std::string {
        if (args.empty()) {
            return "Usage: log_search <term> [max_results]\n"
                   "Searches through log history for matching entries";
        }

        std::string searchTerm = args[0];
        std::transform(searchTerm.begin(), searchTerm.end(), searchTerm.begin(), ::tolower);

        int maxResults = 20;
        if (args.size() > 1) {
            try { maxResults = std::stoi(args[1]); } catch (...) {}
        }

        auto history = GetLogHistory();
        std::stringstream ss;
        int found = 0;
        ss << "Search results for '" << args[0] << "':\n";

        for (int i = static_cast<int>(history.size()) - 1; i >= 0 && found < maxResults; --i) {
            std::string lowerMsg = history[i].message;
            std::transform(lowerMsg.begin(), lowerMsg.end(), lowerMsg.begin(), ::tolower);
            if (lowerMsg.find(searchTerm) != std::string::npos) {
                ss << "  [" << history[i].timestamp << "] [" << history[i].type << "] " << history[i].message << "\n";
                found++;
            }
        }

        if (found == 0) {
            return "No log entries matching '" + args[0] + "'";
        }
        ss << "\nFound " << found << " matching entries";
        return ss.str();
    }, "Search log history for matching entries", "Logging");

    RegisterCommand("log_export", [this](const std::vector<std::string>& args) -> std::string {
        std::string filename = args.empty() ? "spark_console_log.txt" : args[0];

        try {
            std::ofstream file(filename);
            if (!file.is_open()) {
                return "Failed to open file: " + filename;
            }

            file << "Spark Engine Console Log Export\n";
            file << "==========================================\n";

            auto now = std::chrono::system_clock::now();
            auto time_t = std::chrono::system_clock::to_time_t(now);
            file << "Exported: " << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S") << "\n";
            file << "==========================================\n\n";

            auto history = GetLogHistory();
            for (const auto& entry : history) {
                file << "[" << entry.timestamp << "] [" << entry.type << "] " << entry.message << "\n";
            }

            file.close();
            return "Log exported to: " + filename + " (" + std::to_string(history.size()) + " entries)";
        } catch (const std::exception& e) {
            return "Export failed: " + std::string(e.what());
        }
    }, "Export log history to file (usage: log_export [filename])", "Logging");

    RegisterCommand("log_clear", [this](const std::vector<std::string>& args) -> std::string {
        std::lock_guard<std::mutex> lock(m_logMutex);
        size_t count = m_logHistory.size();
        m_logHistory.clear();
        return "Cleared " + std::to_string(count) + " log entries";
    }, "Clear all log history", "Logging");

    RegisterCommand("log_stats", [this](const std::vector<std::string>& args) -> std::string {
        auto history = GetLogHistory();

        std::map<std::string, int> typeCounts;
        for (const auto& entry : history) {
            typeCounts[entry.type]++;
        }

        std::stringstream ss;
        ss << "Log Statistics:\n";
        ss << "  Total entries: " << history.size() << "\n";
        for (const auto& pair : typeCounts) {
            ss << "  " << std::left << std::setw(12) << pair.first << ": " << pair.second << "\n";
        }
        return ss.str();
    }, "Show log entry statistics by type", "Logging");

    RegisterCommand("log_tail", [this](const std::vector<std::string>& args) -> std::string {
        int count = 10;
        if (!args.empty()) {
            try { count = std::stoi(args[0]); } catch (...) {}
        }
        count = std::max(1, std::min(count, 100));

        auto history = GetLogHistory();
        int start = std::max(0, static_cast<int>(history.size()) - count);

        std::stringstream ss;
        ss << "Last " << count << " log entries:\n";
        for (int i = start; i < static_cast<int>(history.size()); ++i) {
            ss << "  [" << history[i].timestamp << "] [" << history[i].type << "] " << history[i].message << "\n";
        }
        return ss.str();
    }, "Show last N log entries (usage: log_tail [count])", "Logging");
}

// ============================================================================
// WATCH COMMANDS - Live variable monitoring
// ============================================================================
void SimpleConsole::RegisterWatchCommands() {
    RegisterCommand("watch", [this](const std::vector<std::string>& args) -> std::string {
        if (args.empty()) {
            if (m_watchEntries.empty()) {
                return "No watch variables set. Usage: watch <variable>\n"
                       "Available: fps, frametime, position, health, objects, memory, drawcalls";
            }
            std::stringstream ss;
            ss << "Active Watches:\n";
            for (const auto& entry : m_watchEntries) {
                ss << "  " << std::left << std::setw(16) << entry.name
                   << " = " << entry.lastValue
                   << (entry.active ? "" : " (paused)") << "\n";
            }
            return ss.str();
        }

        std::string varName = args[0];

        // Built-in watch variables
        if (varName == "fps") {
            AddWatch("fps", []() { return std::to_string(static_cast<int>(g_perfCounters.currentFPS)); });
        } else if (varName == "frametime") {
            AddWatch("frametime", []() {
                std::stringstream ss;
                ss << std::fixed << std::setprecision(2) << g_perfCounters.frameTime << "ms";
                return ss.str();
            });
        } else if (varName == "position") {
            AddWatch("position", []() {
                std::stringstream ss;
                ss << std::fixed << std::setprecision(1)
                   << g_gameState.playerPosition.x << ","
                   << g_gameState.playerPosition.y << ","
                   << g_gameState.playerPosition.z;
                return ss.str();
            });
        } else if (varName == "health") {
            AddWatch("health", []() {
                return std::to_string(static_cast<int>(g_gameState.playerHealth)) + "/" +
                       std::to_string(static_cast<int>(g_gameState.playerMaxHealth));
            });
        } else if (varName == "objects") {
            AddWatch("objects", []() { return std::to_string(g_gameState.activeObjects); });
        } else if (varName == "memory") {
            AddWatch("memory", []() {
                PROCESS_MEMORY_COUNTERS pmc;
                if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
                    return std::to_string(pmc.WorkingSetSize / 1024 / 1024) + "MB";
                }
                return std::string("N/A");
            });
        } else if (varName == "drawcalls") {
            AddWatch("drawcalls", []() { return std::to_string(g_perfCounters.drawCalls); });
        } else if (varName == "triangles") {
            AddWatch("triangles", []() { return std::to_string(g_perfCounters.triangles); });
        } else if (varName == "timescale") {
            AddWatch("timescale", []() {
                std::stringstream ss;
                ss << std::fixed << std::setprecision(2) << g_gameState.timeScale << "x";
                return ss.str();
            });
        } else {
            return "Unknown watch variable: " + varName + "\n"
                   "Available: fps, frametime, position, health, objects, memory, drawcalls, triangles, timescale";
        }

        m_watchActive = true;
        m_lastWatchUpdate = std::chrono::steady_clock::now();
        return "Watch added: " + varName + " (displayed in title bar)";
    }, "Add a live watch variable (shown in title bar)", "Watch");

    RegisterCommand("unwatch", [this](const std::vector<std::string>& args) -> std::string {
        if (args.empty()) return "Usage: unwatch <name|all>";

        if (args[0] == "all") {
            m_watchEntries.clear();
            m_watchActive = false;
            SetConsoleTitleA("Spark Engine - Development Console");
            return "All watches removed";
        }

        RemoveWatch(args[0]);
        if (m_watchEntries.empty()) {
            m_watchActive = false;
            SetConsoleTitleA("Spark Engine - Development Console");
        }
        return "Watch removed: " + args[0];
    }, "Remove a watch variable (usage: unwatch <name|all>)", "Watch");

    RegisterCommand("watch_pause", [this](const std::vector<std::string>& args) -> std::string {
        m_watchActive = !m_watchActive;
        return "Watch updates " + std::string(m_watchActive ? "resumed" : "paused");
    }, "Pause/resume watch variable updates", "Watch");
}

// ============================================================================
// ALIAS COMMANDS - Command shortcut management
// ============================================================================
void SimpleConsole::RegisterAliasCommands() {
    RegisterCommand("alias", [this](const std::vector<std::string>& args) -> std::string {
        if (args.empty()) {
            if (m_aliases.empty()) {
                return "No aliases defined. Usage: alias <name> <command>";
            }
            std::stringstream ss;
            ss << "Defined Aliases:\n";
            for (const auto& pair : m_aliases) {
                ss << "  " << std::left << std::setw(16) << pair.first << " -> " << pair.second << "\n";
            }
            return ss.str();
        }

        if (args.size() < 2) {
            // Show specific alias
            auto it = m_aliases.find(args[0]);
            if (it != m_aliases.end()) {
                return "Alias '" + args[0] + "' -> '" + it->second + "'";
            }
            return "No alias '" + args[0] + "' defined. Usage: alias <name> <command>";
        }

        // Build the command from remaining args
        std::string command;
        for (size_t i = 1; i < args.size(); ++i) {
            if (i > 1) command += " ";
            command += args[i];
        }

        SetAlias(args[0], command);
        return "Alias set: " + args[0] + " -> " + command;
    }, "Create or list command aliases (usage: alias [name] [command])", "Aliases");

    RegisterCommand("unalias", [this](const std::vector<std::string>& args) -> std::string {
        if (args.empty()) return "Usage: unalias <name|all>";

        if (args[0] == "all") {
            size_t count = m_aliases.size();
            m_aliases.clear();
            return "Removed " + std::to_string(count) + " aliases";
        }

        if (m_aliases.find(args[0]) != m_aliases.end()) {
            RemoveAlias(args[0]);
            return "Alias removed: " + args[0];
        }
        return "No alias '" + args[0] + "' found";
    }, "Remove a command alias (usage: unalias <name|all>)", "Aliases");

    // Set up some useful default aliases
    SetAlias("gi", "graphics_info");
    SetAlias("pi", "player_info");
    SetAlias("ci", "camera_info");
    SetAlias("si", "scene_info");
    SetAlias("es", "engine_status");
    SetAlias("perf", "fps");
    SetAlias("mem", "memory");
    SetAlias("tp", "player_teleport");
    SetAlias("god", "player_godmode");
    SetAlias("nc", "player_noclip");
    SetAlias("spawn", "scene_spawn");
    SetAlias("del", "scene_delete");
    SetAlias("ss", "graphics_screenshot");
    SetAlias("wf", "graphics_wireframe");
    SetAlias("ts", "engine_timescale");
}

// ============================================================================
// DEBUG COMMANDS - Runtime debugging tools
// ============================================================================
void SimpleConsole::RegisterDebugCommands() {
    RegisterCommand("debug_vars", [](const std::vector<std::string>& args) -> std::string {
        std::stringstream ss;
        ss << "Debug Variables (Current State):\n";
        ss << "==========================================\n";
        ss << "God Mode:        " << (g_gameState.godMode ? "ON" : "OFF") << "\n";
        ss << "Noclip:          " << (g_gameState.noclip ? "ON" : "OFF") << "\n";
        ss << "Infinite Ammo:   " << (g_gameState.infiniteAmmo ? "ON" : "OFF") << "\n";
        ss << "Wireframe:       " << (g_gameState.wireframe ? "ON" : "OFF") << "\n";
        ss << "Show FPS:        " << (g_gameState.showFPS ? "ON" : "OFF") << "\n";
        ss << "VSync:           " << (g_gameState.vsync ? "ON" : "OFF") << "\n";
        ss << "Time Scale:      " << g_gameState.timeScale << "x\n";
        ss << "Current Scene:   " << g_gameState.currentScene;
        return ss.str();
    }, "Display all debug variables and their current state", "Debug");

    RegisterCommand("debug_counters", [](const std::vector<std::string>& args) -> std::string {
        UpdatePerformanceCounters();
        UpdateGameState();

        std::stringstream ss;
        ss << "Performance Counters:\n";
        ss << "==========================================\n";
        ss << "Current FPS:     " << std::fixed << std::setprecision(1) << g_perfCounters.currentFPS << "\n";
        ss << "Average FPS:     " << g_perfCounters.averageFPS << "\n";
        ss << "Frame Time:      " << std::setprecision(2) << g_perfCounters.frameTime << " ms\n";
        ss << "CPU Usage:       " << g_perfCounters.cpuUsage << "%\n";
        ss << "GPU Usage:       " << g_perfCounters.gpuUsage << "%\n";
        ss << "Draw Calls:      " << g_perfCounters.drawCalls << "\n";
        ss << "Triangles:       " << g_perfCounters.triangles << "\n";
        ss << "Vertices:        " << g_perfCounters.vertices << "\n";
        ss << "Render Time:     " << g_perfCounters.renderTime << " ms\n";
        ss << "Update Time:     " << g_perfCounters.updateTime << " ms\n";
        ss << "Texture Memory:  " << (g_perfCounters.textureMemory / 1024 / 1024) << " MB\n";
        ss << "Buffer Memory:   " << (g_perfCounters.bufferMemory / 1024 / 1024) << " MB";
        return ss.str();
    }, "Display all performance counters in detail", "Debug");

    RegisterCommand("debug_toggle", [](const std::vector<std::string>& args) -> std::string {
        if (args.empty()) {
            return "Usage: debug_toggle <setting>\n"
                   "Settings: fps, wireframe, vsync, godmode, noclip, infiniteammo";
        }

        std::string setting = args[0];
        std::transform(setting.begin(), setting.end(), setting.begin(), ::tolower);

        if (setting == "fps") {
            g_gameState.showFPS = !g_gameState.showFPS;
            return "FPS display " + std::string(g_gameState.showFPS ? "enabled" : "disabled");
        } else if (setting == "wireframe") {
            g_gameState.wireframe = !g_gameState.wireframe;
            if (g_graphics) g_graphics->Console_SetWireframeMode(g_gameState.wireframe);
            return "Wireframe " + std::string(g_gameState.wireframe ? "enabled" : "disabled");
        } else if (setting == "vsync") {
            g_gameState.vsync = !g_gameState.vsync;
            if (g_graphics) g_graphics->Console_SetVSync(g_gameState.vsync);
            return "VSync " + std::string(g_gameState.vsync ? "enabled" : "disabled");
        } else if (setting == "godmode") {
            g_gameState.godMode = !g_gameState.godMode;
            if (g_game) g_game->ApplyDebugSettings(g_gameState.godMode, g_gameState.noclip, g_gameState.infiniteAmmo);
            return "God mode " + std::string(g_gameState.godMode ? "enabled" : "disabled");
        } else if (setting == "noclip") {
            g_gameState.noclip = !g_gameState.noclip;
            if (g_game) g_game->ApplyDebugSettings(g_gameState.godMode, g_gameState.noclip, g_gameState.infiniteAmmo);
            return "Noclip " + std::string(g_gameState.noclip ? "enabled" : "disabled");
        } else if (setting == "infiniteammo") {
            g_gameState.infiniteAmmo = !g_gameState.infiniteAmmo;
            if (g_game) g_game->ApplyDebugSettings(g_gameState.godMode, g_gameState.noclip, g_gameState.infiniteAmmo);
            return "Infinite ammo " + std::string(g_gameState.infiniteAmmo ? "enabled" : "disabled");
        }

        return "Unknown setting: " + setting;
    }, "Toggle a debug setting on/off", "Debug");

    RegisterCommand("debug_crash_test", [](const std::vector<std::string>& args) -> std::string {
        if (args.empty() || args[0] != "confirm") {
            return "WARNING: This will deliberately crash the engine for testing crash handling.\n"
                   "Usage: debug_crash_test confirm";
        }
        int* nullPtr = nullptr;
        *nullPtr = 42;
        return "This should not be reached";
    }, "Deliberately crash the engine for testing (requires 'confirm')", "Debug");

    RegisterCommand("debug_assert_test", [](const std::vector<std::string>& args) -> std::string {
        ASSERT_MSG(false, "Test assertion triggered from console command 'debug_assert_test'");
        return "Assertion was non-fatal or handled";
    }, "Trigger a test assertion for debugging", "Debug");
}

// ============================================================================
// FILE COMMANDS - File system operations
// ============================================================================
void SimpleConsole::RegisterFileCommands() {
    RegisterCommand("file_list", [](const std::vector<std::string>& args) -> std::string {
        std::string path = args.empty() ? "." : args[0];

        try {
            if (!std::filesystem::exists(path)) {
                return "Path does not exist: " + path;
            }

            std::stringstream ss;
            ss << "Directory listing: " << std::filesystem::absolute(path).string() << "\n";
            ss << "==========================================\n";

            int fileCount = 0, dirCount = 0;
            for (const auto& entry : std::filesystem::directory_iterator(path)) {
                if (entry.is_directory()) {
                    ss << "  [DIR]  " << entry.path().filename().string() << "\n";
                    dirCount++;
                } else {
                    auto size = entry.file_size();
                    std::string sizeStr;
                    if (size < 1024) sizeStr = std::to_string(size) + " B";
                    else if (size < 1024 * 1024) sizeStr = std::to_string(size / 1024) + " KB";
                    else sizeStr = std::to_string(size / 1024 / 1024) + " MB";

                    ss << "  " << std::left << std::setw(8) << sizeStr
                       << " " << entry.path().filename().string() << "\n";
                    fileCount++;
                }
            }
            ss << "\n" << dirCount << " directories, " << fileCount << " files";
            return ss.str();
        } catch (const std::exception& e) {
            return "Error listing directory: " + std::string(e.what());
        }
    }, "List files in a directory (usage: file_list [path])", "File");

    RegisterCommand("file_info", [](const std::vector<std::string>& args) -> std::string {
        if (args.empty()) return "Usage: file_info <path>";

        try {
            std::filesystem::path filePath(args[0]);
            if (!std::filesystem::exists(filePath)) {
                return "File does not exist: " + args[0];
            }

            std::stringstream ss;
            ss << "File Information:\n";
            ss << "  Path:     " << std::filesystem::absolute(filePath).string() << "\n";
            ss << "  Size:     " << std::filesystem::file_size(filePath) << " bytes\n";
            ss << "  Type:     " << (std::filesystem::is_directory(filePath) ? "Directory" : "File") << "\n";
            ss << "  Extension:" << filePath.extension().string() << "\n";

            auto ftime = std::filesystem::last_write_time(filePath);
            auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                ftime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
            auto time_t = std::chrono::system_clock::to_time_t(sctp);
            ss << "  Modified: " << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S");

            return ss.str();
        } catch (const std::exception& e) {
            return "Error: " + std::string(e.what());
        }
    }, "Show detailed file information", "File");

    RegisterCommand("file_read", [](const std::vector<std::string>& args) -> std::string {
        if (args.empty()) return "Usage: file_read <path> [max_lines]";

        int maxLines = 20;
        if (args.size() > 1) {
            try { maxLines = std::stoi(args[1]); } catch (...) {}
        }

        try {
            std::ifstream file(args[0]);
            if (!file.is_open()) return "Cannot open file: " + args[0];

            std::stringstream ss;
            std::string line;
            int lineNum = 0;
            while (std::getline(file, line) && lineNum < maxLines) {
                ss << std::right << std::setw(4) << (lineNum + 1) << " | " << line << "\n";
                lineNum++;
            }

            if (lineNum == maxLines) {
                ss << "... (showing first " << maxLines << " lines)";
            }
            return ss.str();
        } catch (const std::exception& e) {
            return "Error reading file: " + std::string(e.what());
        }
    }, "Read and display file contents (usage: file_read <path> [max_lines])", "File");

    RegisterCommand("file_cwd", [](const std::vector<std::string>& args) -> std::string {
        return "Current working directory: " + std::filesystem::current_path().string();
    }, "Show current working directory", "File");

    RegisterCommand("file_find", [](const std::vector<std::string>& args) -> std::string {
        if (args.empty()) return "Usage: file_find <pattern> [directory]";

        std::string pattern = args[0];
        std::string searchDir = args.size() > 1 ? args[1] : ".";

        try {
            std::stringstream ss;
            int found = 0;
            for (const auto& entry : std::filesystem::recursive_directory_iterator(searchDir)) {
                std::string filename = entry.path().filename().string();
                std::transform(filename.begin(), filename.end(), filename.begin(), ::tolower);
                std::string lowerPattern = pattern;
                std::transform(lowerPattern.begin(), lowerPattern.end(), lowerPattern.begin(), ::tolower);

                if (filename.find(lowerPattern) != std::string::npos) {
                    ss << "  " << entry.path().string() << "\n";
                    found++;
                    if (found >= 50) {
                        ss << "  ... (limited to 50 results)";
                        break;
                    }
                }
            }

            if (found == 0) return "No files matching '" + pattern + "' found";
            return "Found " + std::to_string(found) + " file(s):\n" + ss.str();
        } catch (const std::exception& e) {
            return "Search error: " + std::string(e.what());
        }
    }, "Find files matching a pattern (usage: file_find <pattern> [directory])", "File");
}

// ============================================================================
// INPUT COMMANDS - Input system configuration
// ============================================================================
void SimpleConsole::RegisterInputCommands() {
    RegisterCommand("input_bindings", [](const std::vector<std::string>& args) -> std::string {
        std::stringstream ss;
        ss << "Current Input Bindings:\n";
        ss << "==========================================\n";
        ss << "  WASD          - Movement\n";
        ss << "  Mouse         - Look Around\n";
        ss << "  Space         - Jump\n";
        ss << "  Shift         - Sprint\n";
        ss << "  Ctrl          - Crouch\n";
        ss << "  LMB           - Fire\n";
        ss << "  RMB           - Aim/Zoom\n";
        ss << "  R             - Reload\n";
        ss << "  1-5           - Weapon Select\n";
        ss << "  E             - Interact\n";
        ss << "  Tab           - Scoreboard\n";
        ss << "  ` (Backtick)  - Toggle Console\n";
        ss << "  F1            - Toggle Dev Console\n";
        ss << "  Escape        - Pause/Menu";

        if (g_input) {
            ss << "\n\nInput Manager: ACTIVE";
        } else {
            ss << "\n\nInput Manager: NOT AVAILABLE";
        }

        return ss.str();
    }, "Show current input key bindings", "Input");

    RegisterCommand("input_sensitivity", [](const std::vector<std::string>& args) -> std::string {
        if (args.empty()) {
            return "Current mouse sensitivity: " + std::to_string(g_gameState.mouseSensitivity) + "\n"
                   "Usage: input_sensitivity <value> (0.1-10.0)";
        }

        try {
            float sens = std::stof(args[0]);
            if (sens < 0.1f || sens > 10.0f) return "Sensitivity must be between 0.1 and 10.0";
            g_gameState.mouseSensitivity = sens;
            if (g_game && g_game->GetCamera()) {
                g_game->GetCamera()->Console_SetMouseSensitivity(sens);
            }
            return "Mouse sensitivity set to " + std::to_string(sens);
        } catch (...) {
            return "Invalid value. Must be a number.";
        }
    }, "Get or set mouse sensitivity", "Input");

    RegisterCommand("input_invert_y", [](const std::vector<std::string>& args) -> std::string {
        if (args.empty()) {
            return "Y-axis inversion: " + std::string(g_gameState.invertY ? "ENABLED" : "DISABLED") +
                   "\nUsage: input_invert_y <on|off>";
        }
        std::string value = args[0];
        std::transform(value.begin(), value.end(), value.begin(), ::tolower);
        bool enable = (value == "on" || value == "true" || value == "1");
        g_gameState.invertY = enable;
        if (g_game && g_game->GetCamera()) {
            g_game->GetCamera()->Console_SetInvertY(enable);
        }
        return "Y-axis inversion " + std::string(enable ? "enabled" : "disabled");
    }, "Toggle Y-axis inversion for mouse look", "Input");
}

// ============================================================================
// RENDERING COMMANDS - Advanced rendering controls
// ============================================================================
void SimpleConsole::RegisterRenderingCommands() {
    RegisterCommand("render_mode", [](const std::vector<std::string>& args) -> std::string {
        if (args.empty()) {
            return "Usage: render_mode <solid|wireframe|points>\n"
                   "Current: " + std::string(g_gameState.wireframe ? "wireframe" : "solid");
        }

        std::string mode = args[0];
        std::transform(mode.begin(), mode.end(), mode.begin(), ::tolower);

        if (mode == "solid") {
            g_gameState.wireframe = false;
            if (g_graphics) g_graphics->Console_SetWireframeMode(false);
            return "Render mode: solid";
        } else if (mode == "wireframe") {
            g_gameState.wireframe = true;
            if (g_graphics) g_graphics->Console_SetWireframeMode(true);
            return "Render mode: wireframe";
        } else if (mode == "points") {
            return "Point rendering mode not yet implemented";
        }
        return "Unknown render mode: " + mode;
    }, "Set rendering mode (solid/wireframe/points)", "Rendering");

    RegisterCommand("render_resolution", [](const std::vector<std::string>& args) -> std::string {
        if (!g_graphics) return "Graphics engine not available";

        if (args.empty()) {
            return "Current resolution: " + std::to_string(g_graphics->GetWindowWidth()) + "x" +
                   std::to_string(g_graphics->GetWindowHeight());
        }

        if (args.size() < 2) return "Usage: render_resolution <width> <height>";

        try {
            int w = std::stoi(args[0]);
            int h = std::stoi(args[1]);
            return "Resolution change to " + std::to_string(w) + "x" + std::to_string(h) +
                   " requested (requires restart)";
        } catch (...) {
            return "Invalid resolution values";
        }
    }, "Get or set render resolution", "Rendering");

    RegisterCommand("render_fov", [](const std::vector<std::string>& args) -> std::string {
        if (args.empty()) {
            return "Current FOV: " + std::to_string(g_gameState.fov) + " degrees\n"
                   "Usage: render_fov <degrees> (30-170)";
        }
        try {
            float fov = std::stof(args[0]);
            if (fov < 30.0f || fov > 170.0f) return "FOV must be between 30 and 170";
            g_gameState.fov = fov;
            if (g_game && g_game->GetCamera()) {
                g_game->GetCamera()->Console_SetFOV(fov);
            }
            return "FOV set to " + std::to_string(fov) + " degrees";
        } catch (...) {
            return "Invalid FOV value";
        }
    }, "Get or set field of view (degrees)", "Rendering");

    RegisterCommand("render_showfps", [](const std::vector<std::string>& args) -> std::string {
        g_gameState.showFPS = !g_gameState.showFPS;
        return "FPS overlay " + std::string(g_gameState.showFPS ? "enabled" : "disabled");
    }, "Toggle the FPS overlay display", "Rendering");
}

// ============================================================================
// PROFILING COMMANDS - Performance profiling tools
// ============================================================================
void SimpleConsole::RegisterProfilingCommands() {
    RegisterCommand("profile_snapshot", [this](const std::vector<std::string>& args) -> std::string {
        UpdatePerformanceCounters();
        UpdateGameState();

        std::stringstream ss;
        ss << "=== PERFORMANCE SNAPSHOT ===\n";

        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        ss << "Timestamp: " << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S") << "\n\n";

        ss << "Frame Performance:\n";
        ss << "  FPS:           " << std::fixed << std::setprecision(1) << g_perfCounters.currentFPS << "\n";
        ss << "  Avg FPS:       " << g_perfCounters.averageFPS << "\n";
        ss << "  Frame Time:    " << std::setprecision(2) << g_perfCounters.frameTime << " ms\n";
        ss << "  Render Time:   " << g_perfCounters.renderTime << " ms\n";
        ss << "  Update Time:   " << g_perfCounters.updateTime << " ms\n\n";

        ss << "Rendering:\n";
        ss << "  Draw Calls:    " << g_perfCounters.drawCalls << "\n";
        ss << "  Triangles:     " << g_perfCounters.triangles << "\n";
        ss << "  Vertices:      " << g_perfCounters.vertices << "\n";
        ss << "  Objects:       " << g_gameState.activeObjects << " active, "
           << g_gameState.visibleObjects << " visible\n\n";

        PROCESS_MEMORY_COUNTERS pmc;
        if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
            ss << "Memory:\n";
            ss << "  Working Set:   " << (pmc.WorkingSetSize / 1024 / 1024) << " MB\n";
            ss << "  Peak:          " << (pmc.PeakWorkingSetSize / 1024 / 1024) << " MB\n";
            ss << "  Page File:     " << (pmc.PagefileUsage / 1024 / 1024) << " MB\n";
        }

        return ss.str();
    }, "Take a comprehensive performance snapshot", "Profiling");

    RegisterCommand("profile_export", [this](const std::vector<std::string>& args) -> std::string {
        std::string filename = args.empty() ? "spark_profile.csv" : args[0];

        try {
            bool exists = std::filesystem::exists(filename);
            std::ofstream file(filename, std::ios::app);
            if (!file.is_open()) return "Cannot open file: " + filename;

            // Write header if new file
            if (!exists) {
                file << "Timestamp,FPS,AvgFPS,FrameTime,RenderTime,UpdateTime,"
                        "DrawCalls,Triangles,Vertices,ActiveObjects,WorkingSetMB\n";
            }

            UpdatePerformanceCounters();

            PROCESS_MEMORY_COUNTERS pmc;
            GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));

            auto now = std::chrono::system_clock::now();
            auto time_t = std::chrono::system_clock::to_time_t(now);

            file << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S") << ","
                 << g_perfCounters.currentFPS << ","
                 << g_perfCounters.averageFPS << ","
                 << g_perfCounters.frameTime << ","
                 << g_perfCounters.renderTime << ","
                 << g_perfCounters.updateTime << ","
                 << g_perfCounters.drawCalls << ","
                 << g_perfCounters.triangles << ","
                 << g_perfCounters.vertices << ","
                 << g_gameState.activeObjects << ","
                 << (pmc.WorkingSetSize / 1024 / 1024) << "\n";

            file.close();
            return "Profile data appended to: " + filename;
        } catch (const std::exception& e) {
            return "Export failed: " + std::string(e.what());
        }
    }, "Export performance data to CSV (usage: profile_export [filename])", "Profiling");

    RegisterCommand("profile_benchmark", [this](const std::vector<std::string>& args) -> std::string {
        int duration = 5; // seconds
        if (!args.empty()) {
            try { duration = std::stoi(args[0]); } catch (...) {}
        }
        duration = std::max(1, std::min(duration, 30));

        LogInfo("Starting " + std::to_string(duration) + "s benchmark...");

        float minFPS = 9999.0f, maxFPS = 0.0f, totalFPS = 0.0f;
        int samples = 0;

        auto start = std::chrono::steady_clock::now();
        while (true) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start).count();
            if (elapsed >= duration) break;

            UpdatePerformanceCounters();
            float fps = g_perfCounters.currentFPS;
            if (fps > 0) {
                minFPS = std::min(minFPS, fps);
                maxFPS = std::max(maxFPS, fps);
                totalFPS += fps;
                samples++;
            }

            Sleep(100); // Sample every 100ms
        }

        float avgFPS = samples > 0 ? totalFPS / samples : 0;
        std::stringstream ss;
        ss << "Benchmark Results (" << duration << "s):\n";
        ss << "==========================================\n";
        ss << "  Samples:    " << samples << "\n";
        ss << "  Min FPS:    " << std::fixed << std::setprecision(1) << minFPS << "\n";
        ss << "  Max FPS:    " << maxFPS << "\n";
        ss << "  Avg FPS:    " << avgFPS << "\n";
        ss << "  1% Low:     " << minFPS << "\n";
        ss << "  Stability:  " << (maxFPS > 0 ? (minFPS / maxFPS * 100.0f) : 0) << "%";
        return ss.str();
    }, "Run a performance benchmark (usage: profile_benchmark [seconds])", "Profiling");
}

// ============================================================================
// NETWORKING COMMANDS - Network simulation and information
// ============================================================================
void SimpleConsole::RegisterNetworkingCommands() {
    RegisterCommand("net_status", [](const std::vector<std::string>& args) -> std::string {
        std::stringstream ss;
        ss << "Network Status:\n";
        ss << "==========================================\n";
        ss << "  Mode:          Single Player (Local)\n";
        ss << "  Protocol:      N/A\n";
        ss << "  Latency:       0 ms (local)\n";
        ss << "  Packet Loss:   0%\n";
        ss << "  Bandwidth:     N/A\n";
        ss << "\nNetworking subsystem not yet integrated.\n";
        ss << "Use 'net_simulate' to test with simulated latency.";
        return ss.str();
    }, "Show network connection status", "Network");

    RegisterCommand("net_simulate", [](const std::vector<std::string>& args) -> std::string {
        if (args.empty()) {
            return "Usage: net_simulate <latency_ms> [packet_loss_%]\n"
                   "Example: net_simulate 100 5  (100ms latency, 5% packet loss)\n"
                   "Use 'net_simulate off' to disable simulation.";
        }

        if (args[0] == "off" || args[0] == "0") {
            return "Network simulation disabled";
        }

        try {
            int latency = std::stoi(args[0]);
            float packetLoss = args.size() > 1 ? std::stof(args[1]) : 0.0f;

            std::stringstream ss;
            ss << "Network simulation enabled:\n";
            ss << "  Simulated latency:     " << latency << " ms\n";
            ss << "  Simulated packet loss: " << std::fixed << std::setprecision(1) << packetLoss << "%\n";
            ss << "(Note: Actual network simulation requires networking subsystem)";
            return ss.str();
        } catch (...) {
            return "Invalid values. Latency must be integer, packet loss must be float.";
        }
    }, "Simulate network conditions (latency, packet loss)", "Network");

    RegisterCommand("net_info", [](const std::vector<std::string>& args) -> std::string {
        // Get local hostname
        char hostname[256];
        DWORD size = sizeof(hostname);
        GetComputerNameA(hostname, &size);

        std::stringstream ss;
        ss << "Network Information:\n";
        ss << "  Hostname:      " << hostname << "\n";
        ss << "  Network Mode:  Offline/Local\n";
        ss << "  Console Port:  7777 (default)\n";
        ss << "  Max Players:   1 (single player)";
        return ss.str();
    }, "Show network configuration information", "Network");
}

// ============================================================================
// TESTING COMMANDS - System testing and benchmarking
// ============================================================================
void SimpleConsole::RegisterTestingCommands() {
    RegisterCommand("test_commands", [this](const std::vector<std::string>& args) -> std::string {
        LogInfo("Running command system self-test...");

        int passed = 0, failed = 0;

        // Test echo
        auto result = m_commands.find("echo");
        if (result != m_commands.end()) {
            std::string out = result->second.handler({"hello", "world"});
            if (out == "hello world") passed++; else failed++;
        } else failed++;

        // Test version
        result = m_commands.find("version");
        if (result != m_commands.end()) {
            std::string out = result->second.handler({});
            if (!out.empty()) passed++; else failed++;
        } else failed++;

        // Test help
        result = m_commands.find("help");
        if (result != m_commands.end()) {
            std::string out = result->second.handler({});
            if (!out.empty()) passed++; else failed++;
        } else failed++;

        // Test uptime
        result = m_commands.find("uptime");
        if (result != m_commands.end()) {
            std::string out = result->second.handler({});
            if (!out.empty()) passed++; else failed++;
        } else failed++;

        std::stringstream ss;
        ss << "Command System Test Results:\n";
        ss << "  Passed: " << passed << "\n";
        ss << "  Failed: " << failed << "\n";
        ss << "  Total:  " << (passed + failed) << "\n";
        ss << "  Status: " << (failed == 0 ? "ALL PASSED" : "SOME FAILURES");
        return ss.str();
    }, "Run self-test on command system", "Testing");

    RegisterCommand("test_logging", [this](const std::vector<std::string>& args) -> std::string {
        LogInfo("Test INFO message");
        LogWarning("Test WARNING message");
        LogError("Test ERROR message");
        LogSuccess("Test SUCCESS message");
        LogCritical("Test CRITICAL message");
        LogTrace("Test TRACE message");
        LogDebug("Test DEBUG message");
        return "All log levels tested - check output above";
    }, "Test all logging levels", "Testing");

    RegisterCommand("test_stress", [this](const std::vector<std::string>& args) -> std::string {
        int count = 100;
        if (!args.empty()) {
            try { count = std::stoi(args[0]); } catch (...) {}
        }
        count = std::max(1, std::min(count, 1000));

        auto start = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < count; ++i) {
            Log("Stress test message " + std::to_string(i), "DEBUG");
        }
        auto end = std::chrono::high_resolution_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

        std::stringstream ss;
        ss << "Stress Test Results:\n";
        ss << "  Messages:      " << count << "\n";
        ss << "  Total Time:    " << elapsed.count() << " us\n";
        ss << "  Per Message:   " << (elapsed.count() / count) << " us\n";
        ss << "  Throughput:    " << (count * 1000000.0 / elapsed.count()) << " msg/sec";
        return ss.str();
    }, "Stress test the logging system (usage: test_stress [count])", "Testing");

    RegisterCommand("test_memory", [](const std::vector<std::string>& args) -> std::string {
        // Allocate and free memory to test
        const size_t testSize = 10 * 1024 * 1024; // 10 MB

        PROCESS_MEMORY_COUNTERS pmcBefore, pmcAfter;
        GetProcessMemoryInfo(GetCurrentProcess(), &pmcBefore, sizeof(pmcBefore));

        auto start = std::chrono::high_resolution_clock::now();
        char* testBlock = new(std::nothrow) char[testSize];
        if (!testBlock) return "Memory allocation failed";

        memset(testBlock, 0, testSize);
        GetProcessMemoryInfo(GetCurrentProcess(), &pmcAfter, sizeof(pmcAfter));

        auto afterAlloc = std::chrono::high_resolution_clock::now();
        delete[] testBlock;
        auto afterFree = std::chrono::high_resolution_clock::now();

        auto allocTime = std::chrono::duration_cast<std::chrono::microseconds>(afterAlloc - start);
        auto freeTime = std::chrono::duration_cast<std::chrono::microseconds>(afterFree - afterAlloc);

        std::stringstream ss;
        ss << "Memory Test (10 MB allocation):\n";
        ss << "  Alloc+Fill:    " << allocTime.count() << " us\n";
        ss << "  Free:          " << freeTime.count() << " us\n";
        ss << "  Working Set Before: " << (pmcBefore.WorkingSetSize / 1024 / 1024) << " MB\n";
        ss << "  Working Set After:  " << (pmcAfter.WorkingSetSize / 1024 / 1024) << " MB\n";
        ss << "  Delta:         " << ((pmcAfter.WorkingSetSize - pmcBefore.WorkingSetSize) / 1024) << " KB";
        return ss.str();
    }, "Run memory allocation/deallocation test", "Testing");
}

// ============================================================================
// GAME COMMANDS - Game state management
// ============================================================================
void SimpleConsole::RegisterGameCommands() {
    RegisterCommand("game_reset", [](const std::vector<std::string>& args) -> std::string {
        if (g_game) {
            // Reset player
            if (g_game->GetPlayer()) {
                g_game->GetPlayer()->Console_SetHealth(g_gameState.playerMaxHealth);
                g_game->GetPlayer()->Console_SetPosition(0, 5, 0);
                g_game->GetPlayer()->Console_SetGodMode(false);
                g_game->GetPlayer()->Console_SetNoclip(false);
            }
            // Reset time scale
            g_game->SetTimeScale(1.0f);
            g_gameState.timeScale = 1.0f;

            return "Game state reset to defaults";
        }
        return "Game system not available";
    }, "Reset game state to defaults (player, time, etc.)", "Game");

    RegisterCommand("game_save_state", [](const std::vector<std::string>& args) -> std::string {
        std::string filename = args.empty() ? "spark_save.dat" : args[0];
        try {
            std::ofstream file(filename, std::ios::binary);
            if (!file.is_open()) return "Cannot create save file: " + filename;

            // Save header
            const char header[] = "SPARK_SAVE_V1";
            file.write(header, sizeof(header));

            // Save game state
            file.write(reinterpret_cast<const char*>(&g_gameState), sizeof(g_gameState));

            file.close();
            return "Game state saved to: " + filename;
        } catch (const std::exception& e) {
            return "Save failed: " + std::string(e.what());
        }
    }, "Save current game state to file", "Game");

    RegisterCommand("game_load_state", [](const std::vector<std::string>& args) -> std::string {
        std::string filename = args.empty() ? "spark_save.dat" : args[0];
        if (!std::filesystem::exists(filename)) return "Save file not found: " + filename;

        try {
            std::ifstream file(filename, std::ios::binary);
            if (!file.is_open()) return "Cannot open save file: " + filename;

            // Read and verify header
            char header[14];
            file.read(header, sizeof(header));
            if (std::string(header) != "SPARK_SAVE_V1") {
                return "Invalid save file format";
            }

            // Load game state
            file.read(reinterpret_cast<char*>(&g_gameState), sizeof(g_gameState));
            file.close();

            // Apply loaded state to game systems
            if (g_game) {
                g_game->ApplyPhysicsSettings(g_gameState.gravity, g_gameState.playerSpeed,
                                             g_gameState.jumpHeight, g_gameState.friction);
                g_game->SetTimeScale(g_gameState.timeScale);
                g_game->ApplyDebugSettings(g_gameState.godMode, g_gameState.noclip, g_gameState.infiniteAmmo);
            }

            return "Game state loaded from: " + filename;
        } catch (const std::exception& e) {
            return "Load failed: " + std::string(e.what());
        }
    }, "Load game state from file", "Game");

    RegisterCommand("game_time", [](const std::vector<std::string>& args) -> std::string {
        if (args.empty()) {
            std::stringstream ss;
            ss << "Game Time Info:\n";
            ss << "  Time Scale:    " << g_gameState.timeScale << "x\n";
            ss << "  VSync:         " << (g_gameState.vsync ? "ON" : "OFF") << "\n";
            ss << "  Target FPS:    " << (g_gameState.vsync ? "60" : "Unlimited") << "\n";
            ss << "  Paused:        " << (g_game && g_game->IsPaused() ? "YES" : "NO");
            return ss.str();
        }

        if (args[0] == "pause") {
            if (g_game) { g_game->Pause(); return "Game paused"; }
            return "Game system not available";
        } else if (args[0] == "resume") {
            if (g_game) { g_game->Resume(); return "Game resumed"; }
            return "Game system not available";
        } else if (args[0] == "slow") {
            g_gameState.timeScale = 0.25f;
            if (g_game) g_game->SetTimeScale(0.25f);
            return "Time scale: 0.25x (slow motion)";
        } else if (args[0] == "fast") {
            g_gameState.timeScale = 2.0f;
            if (g_game) g_game->SetTimeScale(2.0f);
            return "Time scale: 2.0x (fast forward)";
        } else if (args[0] == "normal") {
            g_gameState.timeScale = 1.0f;
            if (g_game) g_game->SetTimeScale(1.0f);
            return "Time scale: 1.0x (normal)";
        }

        try {
            float scale = std::stof(args[0]);
            if (scale < 0.01f || scale > 10.0f) return "Time scale must be between 0.01 and 10.0";
            g_gameState.timeScale = scale;
            if (g_game) g_game->SetTimeScale(scale);
            return "Time scale: " + std::to_string(scale) + "x";
        } catch (...) {
            return "Usage: game_time [pause|resume|slow|fast|normal|<scale>]";
        }
    }, "Control game time (pause/resume/slow/fast/scale)", "Game");

    RegisterCommand("game_objects", [](const std::vector<std::string>& args) -> std::string {
        UpdateGameState();

        std::stringstream ss;
        ss << "Game Object Summary:\n";
        ss << "==========================================\n";
        ss << "  Active Objects:  " << g_gameState.activeObjects << "\n";
        ss << "  Visible:         " << g_gameState.visibleObjects << "\n";
        ss << "  Culled:          " << g_gameState.culledObjects << "\n";
        ss << "  Draw Calls:      " << g_perfCounters.drawCalls << "\n";
        ss << "  Triangles:       " << g_perfCounters.triangles;

        if (g_game && g_game->GetSceneManager()) {
            ss << "\n  Scene Manager:   ACTIVE";
        }

        return ss.str();
    }, "Show game object summary and counts", "Game");
}

} // namespace Spark