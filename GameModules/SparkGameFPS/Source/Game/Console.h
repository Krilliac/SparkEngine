/**
 * @file Console.h
 * @brief Debug console system for runtime commands and logging
 * @author Spark Engine Team
 * @date 2025
 * 
 * This file provides a debug console interface that can be toggled during gameplay
 * to execute commands, view log messages, and interact with the engine at runtime.
 * The console supports command registration, input handling, and simple text rendering.
 */

#pragma once
#include "Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <d3d11.h>
#include <windows.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <string>
#include <vector>
#include <functional>
#include <algorithm> // std::max
#include <sstream>   // std::wistringstream
#include "Utils/Assert.h"

/**
 * @brief Simple text output function for console rendering
 *
 * Placeholder for proper text rendering. The previous implementation
 * used OutputDebugString which caused extreme spam (called per-line,
 * per-frame whenever the console was visible).
 *
 * @param text Text string to display
 * @param x Horizontal position (currently unused)
 * @param y Vertical position (currently unused)
 * @param scale Text scale factor (currently unused)
 * @param ctx DirectX device context (currently unused)
 *
 * @todo Replace with actual GPU text rendering (e.g. SpriteBatch or ImGui)
 */
inline void DrawText(const std::wstring& /*text*/, float /*x*/, float /*y*/, float /*scale*/,
                     ID3D11DeviceContext* /*ctx*/)
{
    // No-op: OutputDebugString was removed because it fired per-line per-frame
    // and caused significant performance degradation when a debugger was attached.
}

/**
 * @brief Console command structure
 * 
 * Represents a single console command with its name and callback function.
 * Commands can be registered with the console and executed by typing the
 * command name followed by optional arguments.
 */
struct ConsoleCommand
{
    std::wstring name;                                              ///< Command name as typed by the user
    std::function<void(const std::vector<std::wstring>&)> callback; ///< Function to execute when command is called
};

/**
 * @brief Debug console for runtime interaction and logging
 * 
 * The Console class provides a toggleable debug interface that allows developers
 * to execute commands, view system logs, and interact with the engine during
 * runtime. It handles keyboard input, maintains a command history, and provides
 * basic text rendering for console output.
 * 
 * Features include:
 * - Toggle visibility with a hotkey (typically backtick `)
 * - Command registration and execution
 * - Scrolling text buffer for log messages
 * - Input line editing with basic text input
 * - Mouse cursor management
 * 
 * @note Console uses wide character strings for Unicode support
 * @warning Ensure Initialize() is called before using console functionality
 */
class Console
{
  public:
    /**
     * @brief Initialize the console system
     *
     * Sets up the console with the specified screen dimensions for proper
     * positioning and sizing of the console interface.
     *
     * @param screenW Screen width in pixels
     * @param screenH Screen height in pixels
     */
    void Initialize(int screenW, int screenH);

    /**
     * @brief Toggle console visibility
     *
     * Shows or hides the console interface and manages the mouse cursor
     * visibility accordingly.
     */
    void Toggle()
    {
        visible = !visible;
#ifdef SPARK_PLATFORM_WINDOWS
        ShowCursor(visible);
#endif
    }

    /**
     * @brief Check if the console is currently visible
     * @return true if console is visible and active, false otherwise
     */
    bool IsVisible() const { return visible; }

    /**
     * @brief Handle character input for console text entry
     *
     * Processes WM_CHAR messages to build the current input line.
     * Handles printable characters and adds them to the input buffer.
     *
     * @param c Wide character from WM_CHAR message
     * @return true if character was handled, false otherwise
     */
    bool HandleChar(wchar_t c); // WM_CHAR

    /**
     * @brief Handle key down events for console control
     *
     * Processes WM_KEYDOWN messages for special keys like Enter, Backspace,
     * arrow keys for history, and other control keys.
     *
     * @param key Virtual key code from WM_KEYDOWN message
     * @return true if key was handled, false otherwise
     */
    bool HandleKeyDown(WPARAM key); // WM_KEYDOWN

    /**
     * @brief Add a log message to the console buffer
     *
     * Appends a new message to the console's scrolling text buffer.
     * The message will be visible when the console is open.
     *
     * @param msg Log message to add to the console
     */
    void Log(const std::wstring& msg);

    /**
     * @brief Register a command with the in-game console
     *
     * @param name Command name
     * @param callback Function to call when command is executed
     */
    void RegisterCommand(const std::wstring& name, std::function<void(const std::vector<std::wstring>&)> callback);

    /**
     * @brief Render the console interface
     *
     * Draws the console background, text buffer, and input line to the screen.
     * Only renders if the console is currently visible.
     *
     * @param ctx DirectX device context for rendering operations
     */
    void Render(ID3D11DeviceContext* ctx);

    /**
     * @brief Get number of registered commands
     */
    int GetCommandCount() const { return static_cast<int>(commands.size()); }

  private:
    bool visible{false};     ///< Whether console is currently visible
    int width{0}, height{0}; ///< Screen dimensions for positioning

    std::vector<std::wstring> buffer;     ///< Rolling log buffer for console messages
    std::wstring inputLine;               ///< Current user input line
    std::vector<ConsoleCommand> commands; ///< Registered console commands

    // Command history for arrow key navigation
    std::vector<std::wstring> commandHistory; ///< History of executed commands
    int historyIndex{0};                      ///< Current position in history

    // Scroll offset for viewing older messages
    int scrollOffset{0}; ///< Scroll position in buffer

    /**
     * @brief Parse and execute a command line
     *
     * Takes a complete command line, parses it into command name and arguments,
     * and executes the corresponding registered command if found.
     *
     * @param line Complete command line string to execute
     */
    void ExecuteCommand(const std::wstring& line);
};
