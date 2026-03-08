#include "ConsoleApp.h"
#include <iostream>
#include <sstream>
#include <filesystem>
#include <thread>
#include <chrono>
#include <algorithm>
#include <iomanip>
#ifdef SPARK_PLATFORM_WINDOWS
#include <conio.h>
#else
#include <unistd.h>
#include <sys/select.h>
#include <termios.h>
#include <sys/stat.h>
#endif // SPARK_PLATFORM_WINDOWS

// ---------------------------------------------------------------------------
// Linux terminal helpers
// ---------------------------------------------------------------------------
#ifndef SPARK_PLATFORM_WINDOWS
// ANSI color codes for Linux terminal
static constexpr const char* ANSI_RESET = "\033[0m";
static constexpr const char* ANSI_GREEN_BOLD = "\033[1;32m";
static constexpr const char* ANSI_YELLOW = "\033[33m";
static constexpr const char* ANSI_CYAN = "\033[36m";

static bool LinuxKbhit()
{
    struct timeval tv = {0, 0};
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    return select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv) > 0;
}

static char LinuxGetch()
{
    char ch = 0;
    struct termios oldt, newt;
    tcgetattr(STDIN_FILENO, &oldt);
    newt = oldt;
    newt.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);
    if (read(STDIN_FILENO, &ch, 1) < 0)
    {
        ch = 0;
    }
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    return ch;
}

static bool LinuxIsStdinPipe()
{
    struct stat st;
    if (fstat(STDIN_FILENO, &st) == 0)
    {
        return S_ISFIFO(st.st_mode) || S_ISREG(st.st_mode);
    }
    return false;
}
#endif // !SPARK_PLATFORM_WINDOWS

ConsoleApp::ConsoleApp()
    : m_historyIndex(0), m_running(true),
#ifdef SPARK_PLATFORM_WINDOWS
      m_consoleOutput(GetStdHandle(STD_OUTPUT_HANDLE)), m_consoleInput(GetStdHandle(STD_INPUT_HANDLE))
#else
      m_consoleOutput(STDOUT_FILENO), m_consoleInput(STDIN_FILENO)
#endif
{
    RegisterDefaultCommands();

    // Start engine input reading thread (joined in destructor)
    m_engineInputThread = std::thread(&ConsoleApp::ReadEngineInput, this);

    PrintLog(L"Console initialized with engine communication support.");
}

ConsoleApp::~ConsoleApp()
{
    m_running = false;
    if (m_engineInputThread.joinable())
    {
        m_engineInputThread.join();
    }
}

void ConsoleApp::Run()
{
#ifdef SPARK_PLATFORM_WINDOWS
    system("cls");
#else
    system("clear");
#endif
    std::wcout << L"========================================" << std::endl;
    std::wcout << L"   Spark Engine Console v2.0.0" << std::endl;
    std::wcout << L"   Tab: Autocomplete | Up/Down: History" << std::endl;
    std::wcout << L"========================================" << std::endl;
    std::wcout << std::endl;
    PrintLog(L"Console application started. Type 'help' for commands or 'exit' to quit.");

#ifdef SPARK_PLATFORM_WINDOWS
    HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
    DWORD fileType = GetFileType(hStdin);
    bool pipeMode = (fileType == FILE_TYPE_PIPE);
#else
    bool pipeMode = LinuxIsStdinPipe();
#endif
    if (pipeMode)
    {
        PrintLog(L"Connected to Spark Engine via pipe communication.");
    }
    else
    {
        PrintLog(L"Running in standalone mode. Engine commands will not be available.");
        PrintLog(L"Waiting for SparkEngine to connect... (or type commands to use standalone)");
    }

    std::string input;
    int noInputCounter = 0;

    // In pipe mode, start a separate thread for keyboard input
    std::thread keyboardThread;
    std::atomic<bool> keyboardThreadRunning{false};

    if (pipeMode)
    {
        keyboardThreadRunning = true;
        keyboardThread = std::thread(
            [&]()
            {
                while (keyboardThreadRunning && m_running)
                {
#ifdef SPARK_PLATFORM_WINDOWS
                    if (_kbhit())
                    {
                        char ch = _getch();
#else
                    if (LinuxKbhit())
                    {
                        char ch = LinuxGetch();
#endif
                        if (ch == '\r' || ch == '\n')
                        {
                            if (!input.empty())
                            {
                                AddToHistory(input);
                                if (input == "exit" || input == "quit")
                                {
                                    PrintLog(L"Console shutting down...");
                                    m_running = false;
                                    keyboardThreadRunning = false;
                                    break;
                                }
                                // Send command to engine via stdout
                                std::cout << input << std::endl;
                                std::cout.flush();
                                input.clear();
                            }
#ifdef SPARK_PLATFORM_WINDOWS
                            HANDLE hConsoleOut = GetStdHandle(STD_OUTPUT_HANDLE);
                            WriteConsoleW(hConsoleOut, L"\n", 1, NULL, NULL);
#else
                            std::cout << std::endl;
#endif
                        }
                        else if (ch == '\b' || ch == 127) // 127 = DEL on Linux
                        {
                            if (!input.empty())
                            {
                                input.pop_back();
#ifdef SPARK_PLATFORM_WINDOWS
                                HANDLE hConsoleOut = GetStdHandle(STD_OUTPUT_HANDLE);
                                WriteConsoleW(hConsoleOut, L"\b \b", 3, NULL, NULL);
#else
                                std::cout << "\b \b" << std::flush;
#endif
                            }
                        }
                        else if (ch >= 32 && ch <= 126)
                        {
                            input += ch;
#ifdef SPARK_PLATFORM_WINDOWS
                            HANDLE hConsoleOut = GetStdHandle(STD_OUTPUT_HANDLE);
                            wchar_t wch = static_cast<wchar_t>(ch);
                            WriteConsoleW(hConsoleOut, &wch, 1, NULL, NULL);
#else
                            std::cout << ch << std::flush;
#endif
                        }
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            });
    }

    while (m_running)
    {
        // In pipe mode, display prompt and let keyboard thread handle input
        if (pipeMode)
        {
#ifdef SPARK_PLATFORM_WINDOWS
            HANDLE hConsoleOut = GetStdHandle(STD_OUTPUT_HANDLE);
            CONSOLE_SCREEN_BUFFER_INFO csbi;
            GetConsoleScreenBufferInfo(hConsoleOut, &csbi);

            SetConsoleTextAttribute(hConsoleOut, FOREGROUND_GREEN | FOREGROUND_INTENSITY);
            WriteConsoleW(hConsoleOut, L"> ", 2, NULL, NULL);
            SetConsoleTextAttribute(hConsoleOut, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
#else
            std::cout << ANSI_GREEN_BOLD << "> " << ANSI_RESET << std::flush;
#endif

            // Just sleep and let the keyboard thread and ReadEngineInput thread do their work
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

            noInputCounter++;
            if (noInputCounter > 100)
            { // Check connection every 10 seconds
#ifdef SPARK_PLATFORM_WINDOWS
                DWORD newFileType = GetFileType(hStdin);
                if (newFileType != FILE_TYPE_PIPE)
                {
                    PrintLog(L"Engine connection lost. Switching to standalone mode.");
                    pipeMode = false;
                    keyboardThreadRunning = false;
                }
#endif
                noInputCounter = 0;
            }
        }
        else
        {
            // Standalone mode - use standard input handling
#ifdef SPARK_PLATFORM_WINDOWS
            SetConsoleColor(FOREGROUND_GREEN | FOREGROUND_INTENSITY);
#else
            std::cout << ANSI_GREEN_BOLD;
#endif
            std::cout << "> ";
#ifdef SPARK_PLATFORM_WINDOWS
            SetConsoleColor(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
#else
            std::cout << ANSI_RESET;
#endif
            std::cout.flush();

            if (std::getline(std::cin, input))
            {
                input.erase(0, input.find_first_not_of(" \t"));
                input.erase(input.find_last_not_of(" \t") + 1);
                if (!input.empty())
                {
                    AddToHistory(input);
                    if (input == "exit" || input == "quit")
                    {
                        PrintLog(L"Console shutting down...");
                        m_running = false;
                        break;
                    }
                    ExecuteCommand(input);
                }
            }
            else
            {
                PrintLog(L"Input stream closed. Exiting...");
                m_running = false;
                break;
            }
        }
    }

    // Cleanup keyboard thread
    if (keyboardThreadRunning)
    {
        keyboardThreadRunning = false;
        if (keyboardThread.joinable())
        {
            keyboardThread.join();
        }
    }

    PrintLog(L"Console application terminated.");
}

void ConsoleApp::ReadEngineInput()
{
    // This method reads messages from the engine via stdin (which is redirected from engine's pipe)
    std::string line;

#ifdef SPARK_PLATFORM_WINDOWS
    char buffer[1024];
    DWORD bytesRead = 0;
    HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);

    OutputDebugStringA("ReadEngineInput: Starting engine input reader thread\n");

    // Check if stdin is redirected (connected to a pipe)
    DWORD fileType = GetFileType(hStdin);

    // Try to detect if we're connected to a pipe by testing for pipe-specific operations
    DWORD bytesAvailable = 0;
    BOOL isPipeConnected = PeekNamedPipe(hStdin, NULL, 0, NULL, &bytesAvailable, NULL);

    if (isPipeConnected || fileType == FILE_TYPE_PIPE)
    {
        PrintLog(L"Connected to engine via pipe communication.");
        OutputDebugStringA("ReadEngineInput: Pipe connection confirmed\n");
    }
    else
    {
        PrintLog(L"No pipe connection detected. Running in standalone mode.");
        OutputDebugStringA("ReadEngineInput: No pipe connection detected\n");
        return; // Exit thread if no pipe connection
    }

    while (m_running)
    {
        // Check if data is available
        DWORD bytesAvailable2 = 0;
        BOOL pipeResult = PeekNamedPipe(hStdin, NULL, 0, NULL, &bytesAvailable2, NULL);

        if (!pipeResult)
        {
            DWORD error = GetLastError();
            if (error == ERROR_BROKEN_PIPE || error == ERROR_INVALID_HANDLE)
            {
                PrintLog(L"Engine pipe connection lost.");
                OutputDebugStringA("ReadEngineInput: Pipe connection lost\n");
                break;
            }
            // For other errors, continue trying
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        if (bytesAvailable2 > 0)
        {
            OutputDebugStringA(("ReadEngineInput: " + std::to_string(bytesAvailable2) + " bytes available\n").c_str());

            // Read available data
            DWORD bytesToRead = std::min(bytesAvailable2, static_cast<DWORD>(sizeof(buffer) - 1));
            if (ReadFile(hStdin, buffer, bytesToRead, &bytesRead, NULL))
            {
                if (bytesRead > 0)
                {
                    buffer[bytesRead] = '\0';
                    std::string message(buffer);

                    std::string debugMsg = "ReadEngineInput: Received data: " + message.substr(0, 100) + "\n";
                    OutputDebugStringA(debugMsg.c_str());

                    // Process each line in the message
                    std::istringstream iss(message);
                    std::string pipeLine;
                    while (std::getline(iss, pipeLine))
                    {
                        if (!pipeLine.empty())
                        {
                            // Remove carriage return if present
                            if (!pipeLine.empty() && pipeLine.back() == '\r')
                            {
                                pipeLine.pop_back();
                            }

                            if (!pipeLine.empty())
                            {
                                OutputDebugStringA(("ReadEngineInput: Processing line: " + pipeLine + "\n").c_str());

                                // Convert to wide string and display
                                std::wstring wMessage(pipeLine.begin(), pipeLine.end());
                                PrintEngineLog(wMessage);

                                // Add to message buffer
                                m_messageBuffer.push_back(wMessage);
                                if (m_messageBuffer.size() > MAX_BUFFER_SIZE)
                                {
                                    m_messageBuffer.pop_front();
                                }
                            }
                        }
                    }
                }
            }
            else
            {
                DWORD error = GetLastError();
                OutputDebugStringA(
                    ("ReadEngineInput: ReadFile failed with error " + std::to_string(error) + "\n").c_str());
                if (error == ERROR_BROKEN_PIPE)
                {
                    PrintLog(L"Engine connection lost.");
                    break;
                }
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    OutputDebugStringA("ReadEngineInput: Engine input reader thread terminated\n");
    PrintLog(L"Engine input reader thread terminated.");

#else  // Linux/macOS
    if (!LinuxIsStdinPipe())
    {
        PrintLog(L"No pipe connection detected. Running in standalone mode.");
        return;
    }

    PrintLog(L"Connected to engine via pipe communication.");

    while (m_running)
    {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 50000; // 50ms timeout

        int ret = select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv);
        if (ret > 0 && FD_ISSET(STDIN_FILENO, &fds))
        {
            char buffer[1024];
            ssize_t bytesRead = read(STDIN_FILENO, buffer, sizeof(buffer) - 1);
            if (bytesRead > 0)
            {
                buffer[bytesRead] = '\0';
                std::string message(buffer);

                std::istringstream iss(message);
                std::string pipeLine;
                while (std::getline(iss, pipeLine))
                {
                    if (!pipeLine.empty())
                    {
                        if (!pipeLine.empty() && pipeLine.back() == '\r')
                        {
                            pipeLine.pop_back();
                        }
                        if (!pipeLine.empty())
                        {
                            std::wstring wMessage(pipeLine.begin(), pipeLine.end());
                            PrintEngineLog(wMessage);

                            m_messageBuffer.push_back(wMessage);
                            if (m_messageBuffer.size() > MAX_BUFFER_SIZE)
                            {
                                m_messageBuffer.pop_front();
                            }
                        }
                    }
                }
            }
            else if (bytesRead == 0)
            {
                // EOF - pipe closed
                PrintLog(L"Engine pipe connection closed.");
                break;
            }
        }
        else if (ret < 0)
        {
            PrintLog(L"Engine pipe read error.");
            break;
        }
    }

    PrintLog(L"Engine input reader thread terminated.");
#endif // SPARK_PLATFORM_WINDOWS
}

void ConsoleApp::ReadUserInput()
{
    // Advanced input handling with history navigation and tab completion
    std::string input;

    while (m_running)
    {
#ifdef SPARK_PLATFORM_WINDOWS
        if (_kbhit())
        {
            char ch = _getch();
#else
        if (LinuxKbhit())
        {
            char ch = LinuxGetch();
#endif

            // Handle special keys (arrows etc.)
            if (ch == 0 || ch == -32)
            {
#ifdef SPARK_PLATFORM_WINDOWS
                char scanCode = _getch();
#else
                char scanCode = LinuxGetch();
#endif
                switch (scanCode)
                {
                case 72:
                { // Up arrow - previous command
                    std::string prev = GetPreviousCommand();
                    if (!prev.empty())
                    {
                        ClearInputLine();
                        input = prev;
                        UpdateInputLine(input);
                    }
                    break;
                }
                case 80:
                { // Down arrow - next command
                    ClearInputLine();
                    std::string next = GetNextCommand();
                    input = next;
                    UpdateInputLine(input);
                    break;
                }
                default:
                    break;
                }
                continue;
            }
#ifndef SPARK_PLATFORM_WINDOWS
            // Linux escape sequences (e.g. arrow keys: ESC [ A/B/C/D)
            if (ch == 27)
            {
                if (LinuxKbhit())
                {
                    char seq = LinuxGetch();
                    if (seq == '[' && LinuxKbhit())
                    {
                        char arrow = LinuxGetch();
                        if (arrow == 'A')
                        { // Up arrow
                            std::string prev = GetPreviousCommand();
                            if (!prev.empty())
                            {
                                ClearInputLine();
                                input = prev;
                                UpdateInputLine(input);
                            }
                        }
                        else if (arrow == 'B')
                        { // Down arrow
                            ClearInputLine();
                            std::string next = GetNextCommand();
                            input = next;
                            UpdateInputLine(input);
                        }
                    }
                }
                else
                {
                    // Standalone Escape - clear line
                    ClearInputLine();
                    input.clear();
                }
                continue;
            }
#endif

            // Reset tab state on non-tab
            if (ch != '\t')
            {
                m_tabIndex = -1;
                m_tabCompletions.clear();
            }

            if (ch == '\r' || ch == '\n')
            {
#ifdef SPARK_PLATFORM_WINDOWS
                HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
                WriteConsoleW(hOut, L"\n", 1, NULL, NULL);
#else
                std::cout << std::endl;
#endif

                if (!input.empty())
                {
                    AddToHistory(input);
                    std::string resolved = ResolveAlias(input);
                    ExecuteCommand(resolved);
                    input.clear();
                }
            }
            else if (ch == '\b' || ch == 127) // 127 = DEL on Linux
            {
                if (!input.empty())
                {
                    input.pop_back();
#ifdef SPARK_PLATFORM_WINDOWS
                    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
                    WriteConsoleW(hOut, L"\b \b", 3, NULL, NULL);
#else
                    std::cout << "\b \b" << std::flush;
#endif
                }
            }
            else if (ch == '\t')
            {
                HandleTabCompletion(input);
            }
            else if (ch == 27)
            { // Escape - clear line (Windows path)
                ClearInputLine();
                input.clear();
            }
            else if (ch >= 32 && ch <= 126)
            {
                input += ch;
#ifdef SPARK_PLATFORM_WINDOWS
                HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
                wchar_t wch = static_cast<wchar_t>(ch);
                WriteConsoleW(hOut, &wch, 1, NULL, NULL);
#else
                std::cout << ch << std::flush;
#endif
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

std::vector<std::string> ConsoleApp::GetCompletions(const std::string& prefix)
{
    std::vector<std::string> completions;

    // Match registered commands
    auto commands = m_commandRegistry.GetAllCommands();
    for (const auto& cmd : commands)
    {
        if (cmd.name.size() >= prefix.size() && cmd.name.substr(0, prefix.size()) == prefix)
        {
            completions.push_back(cmd.name);
        }
    }

    // Match aliases
    for (const auto& pair : m_aliases)
    {
        if (pair.first.size() >= prefix.size() && pair.first.substr(0, prefix.size()) == prefix)
        {
            completions.push_back(pair.first);
        }
    }

    // Match known engine commands
    static const std::vector<std::string> engineCmds = {"fps",           "info",          "memory_info",
                                                        "graphics_info", "engine_status", "render_debug",
                                                        "shader_debug",  "console_status"};
    for (const auto& cmd : engineCmds)
    {
        if (cmd.size() >= prefix.size() && cmd.substr(0, prefix.size()) == prefix)
        {
            completions.push_back(cmd);
        }
    }

    std::sort(completions.begin(), completions.end());
    // Remove duplicates
    completions.erase(std::unique(completions.begin(), completions.end()), completions.end());
    return completions;
}

void ConsoleApp::HandleTabCompletion(std::string& input)
{
    if (input.empty())
        return;

    if (m_tabIndex == -1)
    {
        m_tabPrefix = input;
        m_tabCompletions = GetCompletions(m_tabPrefix);
        if (m_tabCompletions.empty())
            return;
        m_tabIndex = 0;
    }
    else
    {
        m_tabIndex = (m_tabIndex + 1) % static_cast<int>(m_tabCompletions.size());
    }

    // Show all completions on first tab when multiple matches
    if (m_tabCompletions.size() > 1 && m_tabIndex == 0)
    {
#ifdef SPARK_PLATFORM_WINDOWS
        HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        WriteConsoleW(hOut, L"\n", 1, NULL, NULL);
        SetConsoleColor(FOREGROUND_BLUE | FOREGROUND_GREEN);
        for (const auto& comp : m_tabCompletions)
        {
            std::wstring wcomp(comp.begin(), comp.end());
            wcomp += L"  ";
            WriteConsoleW(hOut, wcomp.c_str(), static_cast<DWORD>(wcomp.length()), NULL, NULL);
        }
        WriteConsoleW(hOut, L"\n", 1, NULL, NULL);
        SetConsoleColor(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);

        // Re-display prompt
        SetConsoleColor(FOREGROUND_GREEN | FOREGROUND_INTENSITY);
        WriteConsoleW(hOut, L"> ", 2, NULL, NULL);
        SetConsoleColor(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
#else
        std::cout << "\n" << ANSI_CYAN;
        for (const auto& comp : m_tabCompletions)
        {
            std::cout << comp << "  ";
        }
        std::cout << ANSI_RESET << "\n";
        std::cout << ANSI_GREEN_BOLD << "> " << ANSI_RESET;
#endif
    }

    // Replace input with completion
#ifdef SPARK_PLATFORM_WINDOWS
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    for (size_t i = 0; i < input.size(); ++i)
    {
        WriteConsoleW(hOut, L"\b \b", 3, NULL, NULL);
    }

    input = m_tabCompletions[m_tabIndex];
    std::wstring winput(input.begin(), input.end());
    WriteConsoleW(hOut, winput.c_str(), static_cast<DWORD>(winput.length()), NULL, NULL);
#else
    for (size_t i = 0; i < input.size(); ++i)
    {
        std::cout << "\b \b";
    }
    input = m_tabCompletions[m_tabIndex];
    std::cout << input << std::flush;
#endif
}

std::string ConsoleApp::ResolveAlias(const std::string& input)
{
    std::istringstream iss(input);
    std::string firstWord;
    iss >> firstWord;

    auto it = m_aliases.find(firstWord);
    if (it != m_aliases.end())
    {
        std::string rest;
        std::getline(iss, rest);
        return it->second + rest;
    }
    return input;
}

void ConsoleApp::PrintLog(const std::wstring& msg)
{
    std::lock_guard<std::mutex> lock(m_outputMutex);

    // Add timestamp
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);

#ifdef SPARK_PLATFORM_WINDOWS
    HANDLE hConsoleOut = GetStdHandle(STD_OUTPUT_HANDLE);

    // Format timestamp
    std::wstringstream timeStr;
    timeStr << std::put_time(std::localtime(&time_t), L"[%H:%M:%S] ");

    SetConsoleTextAttribute(hConsoleOut, FOREGROUND_BLUE | FOREGROUND_GREEN);
    DWORD written;
    std::wstring timestamp = timeStr.str();
    WriteConsoleW(hConsoleOut, timestamp.c_str(), static_cast<DWORD>(timestamp.length()), &written, NULL);

    SetConsoleTextAttribute(hConsoleOut, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
    WriteConsoleW(hConsoleOut, msg.c_str(), static_cast<DWORD>(msg.length()), &written, NULL);
    WriteConsoleW(hConsoleOut, L"\n", 1, &written, NULL);
#else
    std::stringstream timeStr;
    timeStr << std::put_time(std::localtime(&time_t), "[%H:%M:%S] ");

    std::string narrowMsg(msg.begin(), msg.end());
    std::cout << ANSI_CYAN << timeStr.str() << ANSI_RESET << narrowMsg << std::endl;
#endif

    // Manage buffer size
    m_messageBuffer.push_back(msg);
    if (m_messageBuffer.size() > MAX_BUFFER_SIZE)
    {
        m_messageBuffer.pop_front();
    }
}

void ConsoleApp::PrintEngineLog(const std::wstring& msg)
{
    std::lock_guard<std::mutex> lock(m_outputMutex);

    // Duplicate message filtering
    static std::wstring lastMessage;
    static int duplicateCount = 0;
    static auto lastLogTime = std::chrono::steady_clock::now();

    auto now = std::chrono::steady_clock::now();
    auto timeDiff = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastLogTime).count();

    // If same message within 200ms, count as duplicate
    if (msg == lastMessage && timeDiff < 200)
    {
        duplicateCount++;
        if (duplicateCount > 3)
        {
            return; // Skip excessive duplicate messages
        }
    }
    else
    {
        if (duplicateCount > 3)
        {
            auto currentTime = std::chrono::system_clock::now();
            auto time_t = std::chrono::system_clock::to_time_t(currentTime);
#ifdef SPARK_PLATFORM_WINDOWS
            HANDLE hConsoleOut = GetStdHandle(STD_OUTPUT_HANDLE);
            SetConsoleTextAttribute(hConsoleOut, FOREGROUND_RED | FOREGROUND_GREEN);

            std::wstringstream skipMsg;
            skipMsg << L"[" << std::put_time(std::localtime(&time_t), L"%H:%M:%S") << L"] ENGINE: (Skipped "
                    << duplicateCount - 3 << L" duplicate messages)\n";

            std::wstring skipStr = skipMsg.str();
            DWORD written;
            WriteConsoleW(hConsoleOut, skipStr.c_str(), static_cast<DWORD>(skipStr.length()), &written, NULL);
#else
            std::stringstream skipMsg;
            skipMsg << "[" << std::put_time(std::localtime(&time_t), "%H:%M:%S") << "] ENGINE: (Skipped "
                    << duplicateCount - 3 << " duplicate messages)";
            std::cout << ANSI_YELLOW << skipMsg.str() << ANSI_RESET << std::endl;
#endif
        }
        duplicateCount = 0;
        lastMessage = msg;
        lastLogTime = now;
    }

    auto currentTime = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(currentTime);

#ifdef SPARK_PLATFORM_WINDOWS
    HANDLE hConsoleOut = GetStdHandle(STD_OUTPUT_HANDLE);

    std::wstringstream fullMsg;
    fullMsg << L"[" << std::put_time(std::localtime(&time_t), L"%H:%M:%S") << L"] ENGINE: " << msg << L"\n";

    SetConsoleTextAttribute(hConsoleOut, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY);
    std::wstring fullStr = fullMsg.str();
    DWORD written;
    WriteConsoleW(hConsoleOut, fullStr.c_str(), static_cast<DWORD>(fullStr.length()), &written, NULL);

    SetConsoleTextAttribute(hConsoleOut, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
#else
    std::string narrowMsg(msg.begin(), msg.end());
    std::stringstream fullMsg;
    fullMsg << "[" << std::put_time(std::localtime(&time_t), "%H:%M:%S") << "] ENGINE: " << narrowMsg;
    std::cout << ANSI_YELLOW << fullMsg.str() << ANSI_RESET << std::endl;
#endif
}

void ConsoleApp::PrintResult(const std::string& result)
{
    std::lock_guard<std::mutex> lock(m_outputMutex);

    if (!result.empty())
    {
#ifdef SPARK_PLATFORM_WINDOWS
        HANDLE hConsoleOut = GetStdHandle(STD_OUTPUT_HANDLE);

        SetConsoleTextAttribute(hConsoleOut, FOREGROUND_GREEN | FOREGROUND_BLUE);

        std::wstring wResult(result.begin(), result.end());
        DWORD written;
        WriteConsoleW(hConsoleOut, wResult.c_str(), static_cast<DWORD>(wResult.length()), &written, NULL);
        WriteConsoleW(hConsoleOut, L"\n", 1, &written, NULL);

        SetConsoleTextAttribute(hConsoleOut, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
#else
        std::cout << ANSI_CYAN << result << ANSI_RESET << std::endl;
#endif
    }
}

#ifdef SPARK_PLATFORM_WINDOWS
void ConsoleApp::SetConsoleColor(WORD color)
{
    SetConsoleTextAttribute(m_consoleOutput, color);
}
#else
void ConsoleApp::SetConsoleColor(int color)
{
    // On Linux, color is unused - we use ANSI codes directly in output
    (void)color;
}
#endif

void ConsoleApp::ExecuteCommand(const std::string& cmdLine)
{
    if (cmdLine.empty())
        return;

    // Parse command and arguments
    std::istringstream iss(cmdLine);
    std::string command;
    iss >> command;

    std::vector<std::string> args;
    std::string arg;
    while (iss >> arg)
    {
        args.push_back(arg);
    }

    // Check if this is an engine command first, then forward it
    if (ShouldForwardToEngine(command))
    {
        std::cout << cmdLine << std::endl;
        std::cout.flush();
        PrintResult("Command sent to engine: " + cmdLine);
    }
    else
    {
        std::string result = m_commandRegistry.ExecuteCommand(command, args);
        PrintResult(result);
    }
}

bool ConsoleApp::ShouldForwardToEngine(const std::string& command)
{
    static const std::vector<std::string> engineCommands = {"fps",
                                                            "info",
                                                            "test_assert",
                                                            "test_null_access",
                                                            "test_assert_not_null",
                                                            "test_assert_range",
                                                            "crash_mode",
                                                            "memory_info",
                                                            "assert_test",
                                                            "crash_test",
                                                            "assert_mode",
                                                            "graphics_info",
                                                            "engine_status",
                                                            "render_debug",
                                                            "shader_debug",
                                                            "test_engine",
                                                            "minimal_test",
                                                            "console_status",
                                                            "quit",
                                                            "help"};

    return std::find(engineCommands.begin(), engineCommands.end(), command) != engineCommands.end();
}

void ConsoleApp::RegisterDefaultCommands()
{
    // Help command
    m_commandRegistry.RegisterCommand("help", "Show available commands or help for specific command",
                                      "help [command_name]",
                                      [this](const std::vector<std::string>& args) -> std::string
                                      {
                                          if (args.empty())
                                          {
                                              std::stringstream ss;
                                              ss << "Available Console Commands:\n";
                                              auto commands = m_commandRegistry.GetAllCommands();
                                              for (const auto& cmd : commands)
                                              {
                                                  ss << "  " << cmd.name;
                                                  if (!cmd.description.empty())
                                                  {
                                                      ss << " - " << cmd.description;
                                                  }
                                                  ss << "\n";
                                              }
                                              ss << "\nEngine Commands (forwarded to engine):\n";
                                              ss << "  fps - Show current FPS\n";
                                              ss << "  info - Show engine information\n";
                                              ss << "  memory_info - Show memory information\n";
                                              ss << "  test_assert - Trigger test assertion\n";
                                              ss << "  crash_mode <on|off> - Toggle crash dumps\n";
                                              ss << "\nType 'help <command>' for detailed information about a command.";
                                              return ss.str();
                                          }
                                          else
                                          {
                                              return m_commandRegistry.GetCommandHelp(args[0]);
                                          }
                                      });

    // Clear command
    m_commandRegistry.RegisterCommand("clear", "Clear the console screen and refresh display", "clear",
                                      [this](const std::vector<std::string>& args) -> std::string
                                      {
#ifdef SPARK_PLATFORM_WINDOWS
                                          system("cls");

                                          HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
                                          DWORD fileType = GetFileType(hStdin);
#else
            system("clear");
#endif

                                          std::wcout << L"========================================" << std::endl;
                                          std::wcout << L"   Spark Engine Console v1.0.0" << std::endl;
                                          std::wcout << L"   Console Refreshed" << std::endl;
                                          std::wcout << L"========================================" << std::endl;
                                          std::wcout << std::endl;

#ifdef SPARK_PLATFORM_WINDOWS
                                          if (fileType == FILE_TYPE_PIPE)
                                              std::wcout << L"Connected to Spark Engine via pipe" << std::endl;
                                          else
                                              std::wcout << L"Running in standalone mode" << std::endl;
#else
            if (LinuxIsStdinPipe())
                std::wcout << L"Connected to Spark Engine via pipe" << std::endl;
            else
                std::wcout << L"Running in standalone mode" << std::endl;
#endif

                                          std::wcout << L"Type 'help' for available commands" << std::endl;
                                          std::wcout << L"Type 'info' to test engine connection" << std::endl;
                                          std::wcout << std::endl;
                                          std::wcout.flush();

                                          return "";
                                      });

    // History command
    m_commandRegistry.RegisterCommand("history", "Show command history", "history",
                                      [this](const std::vector<std::string>& args) -> std::string
                                      {
                                          std::lock_guard<std::mutex> lock(m_historyMutex);
                                          std::stringstream ss;
                                          ss << "Command History:\n";
                                          for (size_t i = 0; i < m_commandHistory.size(); ++i)
                                          {
                                              ss << "  " << (i + 1) << ": " << m_commandHistory[i] << "\n";
                                          }
                                          return ss.str();
                                      });

    // Status command
    m_commandRegistry.RegisterCommand("status", "Show console status information", "status",
                                      [this](const std::vector<std::string>& args) -> std::string
                                      {
                                          std::stringstream ss;
                                          ss << "Spark Engine Debug Console\n";
                                          ss << "Version: 1.0.0\n";
                                          ss << "Commands registered: " << m_commandRegistry.GetAllCommands().size()
                                             << "\n";
                                          ss << "History entries: " << m_commandHistory.size() << "\n";
                                          ss << "Buffer size: " << m_messageBuffer.size() << "/" << MAX_BUFFER_SIZE
                                             << "\n";
                                          ss << "Connection status: " << (m_running ? "Active" : "Disconnected");
                                          return ss.str();
                                      });

    // Echo command
    m_commandRegistry.RegisterCommand("echo", "Echo back the provided arguments", "echo <message>",
                                      [](const std::vector<std::string>& args) -> std::string
                                      {
                                          std::stringstream ss;
                                          for (size_t i = 0; i < args.size(); ++i)
                                          {
                                              if (i > 0)
                                                  ss << " ";
                                              ss << args[i];
                                          }
                                          return ss.str();
                                      });

    // Test connection command
    m_commandRegistry.RegisterCommand("test_connection", "Test connection to engine", "test_connection",
                                      [this](const std::vector<std::string>& args) -> std::string
                                      {
                                          std::cout << "info" << std::endl;
                                          std::cout.flush();
                                          return "Test command sent to engine. Check for response above.";
                                      });

    // Diagnostic command
    m_commandRegistry.RegisterCommand("diag", "Show console diagnostic information", "diag",
                                      [this](const std::vector<std::string>& args) -> std::string
                                      {
                                          std::stringstream ss;
                                          ss << "SparkConsole Diagnostics:\n";
                                          ss << "  Console running: " << (m_running ? "Yes" : "No") << "\n";
                                          ss << "  Commands registered: " << m_commandRegistry.GetAllCommands().size()
                                             << "\n";
                                          ss << "  Message buffer size: " << m_messageBuffer.size() << "/"
                                             << MAX_BUFFER_SIZE << "\n";

#ifdef SPARK_PLATFORM_WINDOWS
                                          HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
                                          DWORD fileType = GetFileType(hStdin);
                                          ss << "  Input mode: ";
                                          switch (fileType)
                                          {
                                          case FILE_TYPE_CHAR:
                                              ss << "Character device";
                                              break;
                                          case FILE_TYPE_DISK:
                                              ss << "Disk file";
                                              break;
                                          case FILE_TYPE_PIPE:
                                              ss << "Named pipe (connected to engine)";
                                              break;
                                          default:
                                              ss << "Unknown (" << fileType << ")";
                                              break;
                                          }
                                          ss << "\n";
                                          ss << "  Looking for SparkEngine.exe: ";
                                          ss << (std::filesystem::exists("SparkEngine.exe") ? "Found" : "Not found");
#else
            ss << "  Input mode: " << (LinuxIsStdinPipe() ? "Pipe (connected to engine)" : "Terminal (standalone)") << "\n";
            ss << "  Looking for SparkEngine: ";
            ss << (std::filesystem::exists("SparkEngine") ? "Found" : "Not found");
#endif

                                          return ss.str();
                                      });

    // Pipe test command
    m_commandRegistry.RegisterCommand("pipe_test", "Test pipe communication with engine", "pipe_test",
                                      [this](const std::vector<std::string>& args) -> std::string
                                      {
                                          std::stringstream ss;
                                          ss << "Pipe Communication Test:\n";

#ifdef SPARK_PLATFORM_WINDOWS
                                          HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
                                          DWORD fileType = GetFileType(hStdin);
                                          bool connected = (fileType == FILE_TYPE_PIPE);
                                          ss << "  Stdin file type: ";
                                          switch (fileType)
                                          {
                                          case FILE_TYPE_CHAR:
                                              ss << "Character device (no pipe)";
                                              break;
                                          case FILE_TYPE_PIPE:
                                              ss << "Named pipe (connected!)";
                                              break;
                                          default:
                                              ss << "Other (" << fileType << ")";
                                              break;
                                          }
#else
            bool connected = LinuxIsStdinPipe();
            ss << "  Stdin type: " << (connected ? "Pipe (connected!)" : "Terminal (no pipe)");
#endif
                                          ss << "\n";

                                          if (connected)
                                          {
                                              ss << "  Sending test command to engine...";
                                              std::cout << "test_engine" << std::endl;
                                              std::cout.flush();
                                              ss << " Sent!\n";
                                              ss << "  Watch for response from engine above.";
                                          }
                                          else
                                          {
                                              ss << "  No pipe connection - cannot send commands to engine";
                                          }

                                          return ss.str();
                                      });

    // Console refresh command
    m_commandRegistry.RegisterCommand("refresh", "Refresh console display", "refresh",
                                      [this](const std::vector<std::string>& args) -> std::string
                                      {
                                          std::wcout << std::endl;
#ifdef SPARK_PLATFORM_WINDOWS
                                          SetConsoleColor(FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_INTENSITY);
#else
                                          std::cout << ANSI_CYAN;
#endif
                                          std::wcout << L"Console display refreshed." << std::endl;
#ifdef SPARK_PLATFORM_WINDOWS
                                          SetConsoleColor(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
#else
                                          std::cout << ANSI_RESET;
#endif
                                          std::wcout.flush();
                                          return "";
                                      });

    // Alias command
    m_commandRegistry.RegisterCommand("alias", "Create or list command aliases", "alias [name] [command]",
                                      [this](const std::vector<std::string>& args) -> std::string
                                      {
                                          if (args.empty())
                                          {
                                              if (m_aliases.empty())
                                                  return "No aliases defined. Usage: alias <name> <command>";
                                              std::stringstream ss;
                                              ss << "Defined Aliases:\n";
                                              for (const auto& pair : m_aliases)
                                              {
                                                  ss << "  " << pair.first << " -> " << pair.second << "\n";
                                              }
                                              return ss.str();
                                          }
                                          if (args.size() < 2)
                                          {
                                              auto it = m_aliases.find(args[0]);
                                              if (it != m_aliases.end())
                                                  return "Alias '" + args[0] + "' -> '" + it->second + "'";
                                              return "No alias '" + args[0] + "'. Usage: alias <name> <command>";
                                          }
                                          std::string cmd;
                                          for (size_t i = 1; i < args.size(); ++i)
                                          {
                                              if (i > 1)
                                                  cmd += " ";
                                              cmd += args[i];
                                          }
                                          m_aliases[args[0]] = cmd;
                                          return "Alias set: " + args[0] + " -> " + cmd;
                                      });

    // Unalias command
    m_commandRegistry.RegisterCommand("unalias", "Remove a command alias", "unalias <name|all>",
                                      [this](const std::vector<std::string>& args) -> std::string
                                      {
                                          if (args.empty())
                                              return "Usage: unalias <name|all>";
                                          if (args[0] == "all")
                                          {
                                              size_t count = m_aliases.size();
                                              m_aliases.clear();
                                              return "Removed " + std::to_string(count) + " aliases";
                                          }
                                          if (m_aliases.erase(args[0]) > 0)
                                              return "Alias removed: " + args[0];
                                          return "No alias '" + args[0] + "' found";
                                      });

    // Version command
    m_commandRegistry.RegisterCommand("version", "Show console version", "version",
                                      [](const std::vector<std::string>& args) -> std::string
                                      {
                                          return "Spark Engine Console v2.0.0\n"
                                                 "Features: Tab completion, command aliases, history navigation\n"
                                                 "Build: Development";
                                      });

    // Uptime command
    m_commandRegistry.RegisterCommand("uptime", "Show how long the console has been running", "uptime",
                                      [](const std::vector<std::string>& args) -> std::string
                                      {
                                          static auto startTime = std::chrono::steady_clock::now();
                                          auto now = std::chrono::steady_clock::now();
                                          auto uptime =
                                              std::chrono::duration_cast<std::chrono::seconds>(now - startTime);
                                          int h = static_cast<int>(uptime.count()) / 3600;
                                          int m = (static_cast<int>(uptime.count()) % 3600) / 60;
                                          int s = static_cast<int>(uptime.count()) % 60;
                                          std::stringstream ss;
                                          ss << "Console uptime: " << h << "h " << m << "m " << s << "s";
                                          return ss.str();
                                      });

    // Set default aliases
    m_aliases["cls"] = "clear";
    m_aliases["q"] = "exit";
    m_aliases["h"] = "help";
    m_aliases["hist"] = "history";
    m_aliases["tc"] = "test_connection";
    m_aliases["pt"] = "pipe_test";
}

void ConsoleApp::AddToHistory(const std::string& cmd)
{
    std::lock_guard<std::mutex> lock(m_historyMutex);

    if (!m_commandHistory.empty() && m_commandHistory.back() == cmd)
    {
        return;
    }

    m_commandHistory.push_back(cmd);

    const size_t MAX_HISTORY = 100;
    if (m_commandHistory.size() > MAX_HISTORY)
    {
        m_commandHistory.erase(m_commandHistory.begin());
    }

    m_historyIndex = m_commandHistory.size();
}

std::string ConsoleApp::GetPreviousCommand()
{
    std::lock_guard<std::mutex> lock(m_historyMutex);

    if (m_commandHistory.empty() || m_historyIndex == 0)
    {
        return "";
    }

    --m_historyIndex;
    return m_commandHistory[m_historyIndex];
}

std::string ConsoleApp::GetNextCommand()
{
    std::lock_guard<std::mutex> lock(m_historyMutex);

    if (m_commandHistory.empty() || m_historyIndex >= m_commandHistory.size() - 1)
    {
        m_historyIndex = m_commandHistory.size();
        return "";
    }

    ++m_historyIndex;
    return m_commandHistory[m_historyIndex];
}

void ConsoleApp::ClearInputLine()
{
#ifdef SPARK_PLATFORM_WINDOWS
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    HANDLE hStdOut = GetStdHandle(STD_OUTPUT_HANDLE);

    if (GetConsoleScreenBufferInfo(hStdOut, &csbi))
    {
        COORD coord = {0, csbi.dwCursorPosition.Y};
        SetConsoleCursorPosition(hStdOut, coord);

        DWORD written;
        std::string spaces(csbi.dwSize.X, ' ');
        WriteConsoleA(hStdOut, spaces.c_str(), csbi.dwSize.X, &written, nullptr);

        SetConsoleCursorPosition(hStdOut, coord);
    }
#else
    // ANSI escape: move to column 0 and clear line
    std::cout << "\r\033[K" << std::flush;
#endif
}

void ConsoleApp::UpdateInputLine(const std::string& text)
{
    ClearInputLine();

#ifdef SPARK_PLATFORM_WINDOWS
    SetConsoleColor(FOREGROUND_GREEN | FOREGROUND_INTENSITY);
    std::wcout << L"SparkConsole> ";
    SetConsoleColor(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
#else
    std::cout << ANSI_GREEN_BOLD << "SparkConsole> " << ANSI_RESET;
#endif
    std::cout << text;
    std::cout.flush();
}
