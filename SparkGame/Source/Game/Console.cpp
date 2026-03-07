#include "Core/Platform.h"
// Console.cpp
#include "Console.h"
#include "Utils/Assert.h"
#include <iostream>
#include <sstream>
#include <algorithm>

// -----------------------------------------------------------------------------
void Console::Initialize(int screenW, int screenH)
{
    ASSERT(screenW > 0 && screenH > 0);
    width = screenW;
    height = screenH;

    historyIndex = 0;
    scrollOffset = 0;

    // Register built-in commands
    commands.push_back({ L"help", [this](auto& tokens)
    {
        Log(L"=== In-Game Console Commands ===");
        for (auto& c : commands) {
            Log(L"  " + c.name);
        }
        Log(L"================================");
        Log(L"Up/Down: History | PageUp/Down: Scroll | Esc: Close");
    } });

    commands.push_back({ L"clear", [this](auto& tokens)
    {
        buffer.clear();
        scrollOffset = 0;
        Log(L"Console cleared.");
    } });

    commands.push_back({ L"version", [this](auto& tokens)
    {
        Log(L"Spark Engine v1.0.0 - In-Game Console");
    } });

    commands.push_back({ L"history", [this](auto& tokens)
    {
        if (commandHistory.empty()) {
            Log(L"No command history.");
            return;
        }
        Log(L"Command History:");
        for (size_t i = 0; i < commandHistory.size(); ++i) {
            Log(L"  " + std::to_wstring(i + 1) + L": " + commandHistory[i]);
        }
    } });

    commands.push_back({ L"echo", [this](auto& tokens)
    {
        std::wstring msg;
        for (size_t i = 1; i < tokens.size(); ++i) {
            if (i > 1) msg += L" ";
            msg += tokens[i];
        }
        Log(msg);
    } });

    std::wcout << L"[INFO] Console initialization complete (" << commands.size() << " commands)." << std::endl;
}

// -----------------------------------------------------------------------------
void Console::RegisterCommand(const std::wstring& name,
                              std::function<void(const std::vector<std::wstring>&)> callback)
{
    // Check for duplicate
    for (auto& cmd : commands) {
        if (cmd.name == name) {
            cmd.callback = callback;
            return;
        }
    }
    commands.push_back({ name, callback });
}

// -----------------------------------------------------------------------------
bool Console::HandleChar(wchar_t c)
{
    if (!visible) return false;

    ASSERT_MSG(c >= 0, "Invalid character code");
    if (c >= L' ' && c <= L'~')      // printable ASCII
        inputLine.push_back(c);

    return true;                     // swallow input
}

// -----------------------------------------------------------------------------
bool Console::HandleKeyDown(WPARAM key)
{
    if (!visible) return false;

    switch (key)
    {
    case VK_BACK:
        if (!inputLine.empty()) inputLine.pop_back();
        return true;

    case VK_RETURN:
        if (!inputLine.empty()) {
            // Add to history
            commandHistory.push_back(inputLine);
            if (commandHistory.size() > 100) {
                commandHistory.erase(commandHistory.begin());
            }
            historyIndex = static_cast<int>(commandHistory.size());

            ExecuteCommand(inputLine);
            inputLine.clear();
        }
        return true;

    case VK_UP:
        // Navigate history up
        if (!commandHistory.empty() && historyIndex > 0) {
            historyIndex--;
            inputLine = commandHistory[historyIndex];
        }
        return true;

    case VK_DOWN:
        // Navigate history down
        if (!commandHistory.empty()) {
            if (historyIndex < static_cast<int>(commandHistory.size()) - 1) {
                historyIndex++;
                inputLine = commandHistory[historyIndex];
            } else {
                historyIndex = static_cast<int>(commandHistory.size());
                inputLine.clear();
            }
        }
        return true;

    case VK_PRIOR: // Page Up - scroll up
        scrollOffset = std::min(scrollOffset + 5, std::max(0, static_cast<int>(buffer.size()) - 20));
        return true;

    case VK_NEXT: // Page Down - scroll down
        scrollOffset = std::max(0, scrollOffset - 5);
        return true;

    case VK_HOME:
        scrollOffset = std::max(0, static_cast<int>(buffer.size()) - 20);
        return true;

    case VK_END:
        scrollOffset = 0;
        return true;

    case VK_DELETE:
        inputLine.clear();
        return true;

    case VK_ESCAPE:
        Toggle();                    // close console
        return true;

    default:
        return false;
    }
}

// -----------------------------------------------------------------------------
void Console::Log(const std::wstring& msg)
{
    // Allow empty messages (blank lines) intentionally
    buffer.push_back(msg);
    if (buffer.size() > 200)         // cap log size
        buffer.erase(buffer.begin());

    // Reset scroll to bottom when new message arrives
    scrollOffset = 0;
}

// -----------------------------------------------------------------------------
void Console::ExecuteCommand(const std::wstring& line)
{
    ASSERT_ALWAYS(!line.empty());
    Log(L"> " + line);

    std::wistringstream iss(line);
    std::vector<std::wstring> tokens;
    std::wstring tok;
    while (iss >> tok)
        tokens.push_back(tok);

    if (tokens.empty()) return;

    // Case-insensitive command lookup
    std::wstring cmdName = tokens[0];
    std::transform(cmdName.begin(), cmdName.end(), cmdName.begin(), ::towlower);

    for (auto& cmd : commands)
    {
        std::wstring registeredName = cmd.name;
        std::transform(registeredName.begin(), registeredName.end(), registeredName.begin(), ::towlower);

        if (registeredName == cmdName)
        {
            cmd.callback(tokens);
            return;
        }
    }

    Log(L"Unknown command: " + tokens[0] + L" (type 'help' for available commands)");
}

// -----------------------------------------------------------------------------
void Console::Render(ID3D11DeviceContext* ctx)
{
    if (!visible) return;
    ASSERT(ctx != nullptr);

    constexpr float lineH = 18.0f;
    constexpr float scale = 1.0f;
    float y = 10.0f;

    // Calculate visible range with scroll support
    int totalMessages = static_cast<int>(buffer.size());
    int maxVisible = 20;
    int endIdx = totalMessages - scrollOffset;
    int startIdx = std::max(0, endIdx - maxVisible);

    for (int i = startIdx; i < endIdx && i < totalMessages; ++i)
    {
        DrawText(buffer[i], 10.0f, y, scale, ctx);
        y += lineH;
    }

    // Draw scroll indicator if not at bottom
    if (scrollOffset > 0) {
        DrawText(L"--- Scroll: " + std::to_wstring(scrollOffset) + L" lines above ---", 10.0f, y, scale * 0.8f, ctx);
        y += lineH;
    }

    // Draw input line with cursor blink
    static int cursorBlink = 0;
    cursorBlink = (cursorBlink + 1) % 60;
    std::wstring cursor = (cursorBlink < 30) ? L"_" : L"";
    DrawText(L"> " + inputLine + cursor, 10.0f, y, scale, ctx);
}

