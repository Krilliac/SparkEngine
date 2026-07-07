#include "ConsoleApp.h"
#include "CommandParser.h"
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

// ---------------------------------------------------------------------------
// Console version — single-sourced. The narrow and wide forms must stay in
// sync; they exist only because the banner is emitted through std::wcout while
// the command results are narrow std::string.
// ---------------------------------------------------------------------------
namespace
{
    constexpr const char* kConsoleVersion = "2.0.0";
    constexpr const wchar_t* kConsoleVersionW = L"2.0.0";
} // namespace

ConsoleApp::ConsoleApp()
    : m_running(true),
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

void ConsoleApp::PrintBanner()
{
#ifdef SPARK_PLATFORM_WINDOWS
    [[maybe_unused]] int rc_ = system("cls"); // Intentional: side-effect only
    std::wcout << L"========================================" << std::endl;
    std::wcout << L"   Spark Engine Console v" << kConsoleVersionW << std::endl;
    std::wcout << L"   Type 'help' for commands, 'history' to recall" << std::endl;
    std::wcout << L"========================================" << std::endl;
    std::wcout << std::endl;
#else
    // On Linux the subprocess's stdout is wired straight into the engine's
    // ConsoleProcessManager command queue. Writing the banner to stdout would
    // make the engine interpret each banner line as a console command and
    // emit "Unknown command: ..." back to the user. Route human-facing text
    // through stderr so stdout is reserved for commands only.
    std::cerr << "========================================" << std::endl;
    std::cerr << "   Spark Engine Console v" << kConsoleVersion << std::endl;
    std::cerr << "   Type 'help' for commands, 'history' to recall" << std::endl;
    std::cerr << "========================================" << std::endl;
    std::cerr << std::endl;
#endif
    PrintLog(L"Console application started. Type 'help' for commands or 'exit' to quit.");
}

bool ConsoleApp::DetectPipeMode()
{
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
    return pipeMode;
}

void ConsoleApp::PollPipeModeInput(std::string& input, int& noInputCounter, bool& pipeMode,
                                   std::atomic<bool>& keyboardThreadRunning)
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
        HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
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

void ConsoleApp::PollStandaloneInput(std::string& input)
{
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
            std::string resolved = ResolveAlias(input);
            if (resolved == "exit" || resolved == "quit")
            {
                PrintLog(L"Console shutting down...");
                m_running = false;
                return;
            }
            ExecuteCommand(resolved);
        }
    }
    else
    {
        PrintLog(L"Input stream closed. Exiting...");
        m_running = false;
    }
}

void ConsoleApp::PipeKeyboardThreadFunc(std::string& input, std::atomic<bool>& keyboardThreadRunning)
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
                    std::string resolved = ResolveAlias(input);
                    if (resolved == "exit" || resolved == "quit")
                    {
                        PrintLog(L"Console shutting down...");
                        m_running = false;
                        keyboardThreadRunning = false;
                        break;
                    }
                    std::cout << resolved << std::endl;
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
            else if (ch == '\b' || ch == 127)
            {
                HandleBackspaceKey(input);
            }
            else if (ch >= 32 && ch <= 126)
            {
                HandlePrintableChar(input, ch);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void ConsoleApp::Run()
{
    PrintBanner();
    bool pipeMode = DetectPipeMode();

    std::string input;
    int noInputCounter = 0;

    std::thread keyboardThread;
    std::atomic<bool> keyboardThreadRunning{false};

    if (pipeMode)
    {
        keyboardThreadRunning = true;
        keyboardThread =
            std::thread(&ConsoleApp::PipeKeyboardThreadFunc, this, std::ref(input), std::ref(keyboardThreadRunning));
    }

    while (m_running)
    {
        if (pipeMode)
        {
            PollPipeModeInput(input, noInputCounter, pipeMode, keyboardThreadRunning);
        }
        else
        {
            PollStandaloneInput(input);
        }
    }

    // Cleanup keyboard thread
    if (keyboardThreadRunning)
    {
        keyboardThreadRunning = false;
    }
    if (keyboardThread.joinable())
    {
        keyboardThread.join();
    }

    PrintLog(L"Console application terminated.");
}

void ConsoleApp::ProcessPipeMessages(const std::string& message)
{
    std::istringstream iss(message);
    std::string pipeLine;
    while (std::getline(iss, pipeLine))
    {
        if (pipeLine.empty())
        {
            continue;
        }

        // Remove carriage return if present
        if (pipeLine.back() == '\r')
        {
            pipeLine.pop_back();
        }
        if (pipeLine.empty())
        {
            continue;
        }

#ifdef SPARK_PLATFORM_WINDOWS
        OutputDebugStringA(("ReadEngineInput: Processing line: " + pipeLine + "\n").c_str());
#endif

        std::wstring wMessage(pipeLine.begin(), pipeLine.end());
        PrintEngineLog(wMessage);

        // PrintEngineLog above takes m_outputMutex and releases it before returning,
        // so a fresh guard is required here: the buffer is also mutated by PrintLog
        // and read by the 'status'/'diag' command handlers on other threads.
        std::lock_guard<std::mutex> lock(m_outputMutex);
        m_messageBuffer.push_back(wMessage);
        if (m_messageBuffer.size() > MAX_BUFFER_SIZE)
        {
            m_messageBuffer.pop_front();
        }
    }
}

#ifdef SPARK_PLATFORM_WINDOWS
bool ConsoleApp::PollWindowsPipeData(HANDLE hStdin)
{
    char buffer[1024];
    DWORD bytesRead = 0;
    DWORD bytesAvailable = 0;
    BOOL pipeResult = PeekNamedPipe(hStdin, NULL, 0, NULL, &bytesAvailable, NULL);

    if (!pipeResult)
    {
        DWORD error = GetLastError();
        if (error == ERROR_BROKEN_PIPE || error == ERROR_INVALID_HANDLE)
        {
            PrintLog(L"Engine pipe connection lost.");
            OutputDebugStringA("ReadEngineInput: Pipe connection lost\n");
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        return true;
    }

    if (bytesAvailable > 0)
    {
        OutputDebugStringA(("ReadEngineInput: " + std::to_string(bytesAvailable) + " bytes available\n").c_str());

        DWORD bytesToRead = std::min(bytesAvailable, static_cast<DWORD>(sizeof(buffer) - 1));
        if (ReadFile(hStdin, buffer, bytesToRead, &bytesRead, NULL))
        {
            if (bytesRead > 0)
            {
                buffer[bytesRead] = '\0';
                std::string message(buffer);

                std::string debugMsg = "ReadEngineInput: Received data: " + message.substr(0, 100) + "\n";
                OutputDebugStringA(debugMsg.c_str());

                ProcessPipeMessages(message);
            }
        }
        else
        {
            DWORD error = GetLastError();
            OutputDebugStringA(("ReadEngineInput: ReadFile failed with error " + std::to_string(error) + "\n").c_str());
            if (error == ERROR_BROKEN_PIPE)
            {
                PrintLog(L"Engine connection lost.");
                return false;
            }
        }
    }

    return true;
}

void ConsoleApp::ReadEngineInputWindows()
{
    HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);

    OutputDebugStringA("ReadEngineInput: Starting engine input reader thread\n");

    DWORD fileType = GetFileType(hStdin);
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
        return;
    }

    while (m_running)
    {
        if (!PollWindowsPipeData(hStdin))
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    OutputDebugStringA("ReadEngineInput: Engine input reader thread terminated\n");
    PrintLog(L"Engine input reader thread terminated.");
}
#else
void ConsoleApp::ReadEngineInputPosix()
{
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
                ProcessPipeMessages(std::string(buffer));
            }
            else if (bytesRead == 0)
            {
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
}
#endif // SPARK_PLATFORM_WINDOWS

void ConsoleApp::ReadEngineInput()
{
#ifdef SPARK_PLATFORM_WINDOWS
    ReadEngineInputWindows();
#else
    ReadEngineInputPosix();
#endif
}

void ConsoleApp::HandleBackspaceKey(std::string& input)
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

void ConsoleApp::HandlePrintableChar(std::string& input, char ch)
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
    // stderr, not stdout — stdout is the command channel to the engine.
    std::cerr << ANSI_CYAN << timeStr.str() << ANSI_RESET << narrowMsg << std::endl;
#endif

    // Manage buffer size
    m_messageBuffer.push_back(msg);
    if (m_messageBuffer.size() > MAX_BUFFER_SIZE)
    {
        m_messageBuffer.pop_front();
    }
}

void ConsoleApp::PrintDuplicateSkipNotice(int skippedCount)
{
    auto currentTime = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(currentTime);
#ifdef SPARK_PLATFORM_WINDOWS
    HANDLE hConsoleOut = GetStdHandle(STD_OUTPUT_HANDLE);
    SetConsoleTextAttribute(hConsoleOut, FOREGROUND_RED | FOREGROUND_GREEN);

    std::wstringstream skipMsg;
    skipMsg << L"[" << std::put_time(std::localtime(&time_t), L"%H:%M:%S") << L"] ENGINE: (Skipped " << skippedCount
            << L" duplicate messages)\n";

    std::wstring skipStr = skipMsg.str();
    DWORD written;
    WriteConsoleW(hConsoleOut, skipStr.c_str(), static_cast<DWORD>(skipStr.length()), &written, NULL);
#else
    std::stringstream skipMsg;
    skipMsg << "[" << std::put_time(std::localtime(&time_t), "%H:%M:%S") << "] ENGINE: (Skipped " << skippedCount
            << " duplicate messages)";
    std::cout << ANSI_YELLOW << skipMsg.str() << ANSI_RESET << std::endl;
#endif
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

    if (msg == lastMessage && timeDiff < 200)
    {
        duplicateCount++;
        if (duplicateCount > 3)
        {
            return;
        }
    }
    else
    {
        if (duplicateCount > 3)
        {
            PrintDuplicateSkipNotice(duplicateCount - 3);
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

    // Parse command and arguments via the shared parser so quoted arguments
    // (e.g. echo "a b") are preserved as a single token.
    std::string command;
    std::vector<std::string> args;
    if (!CommandParser::ParseCommandLine(cmdLine, command, args))
        return;

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
    RegisterCoreCommands();
    RegisterDiagnosticCommands();
    RegisterAliasCommands();
}

void ConsoleApp::RegisterCoreCommands()
{
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

    m_commandRegistry.RegisterCommand("clear", "Clear the console screen and refresh display", "clear",
                                      [this](const std::vector<std::string>& args) -> std::string
                                      {
#ifdef SPARK_PLATFORM_WINDOWS
                                          [[maybe_unused]] int rc_ = system("cls"); // Intentional: side-effect only

                                          HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
                                          DWORD fileType = GetFileType(hStdin);
#else
            [[maybe_unused]] int rc_ = system("clear"); // Intentional: side-effect only
#endif

                                          std::wcout << L"========================================" << std::endl;
                                          std::wcout << L"   Spark Engine Console v" << kConsoleVersionW << std::endl;
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

    m_commandRegistry.RegisterCommand("version", "Show console version", "version",
                                      [](const std::vector<std::string>& args) -> std::string
                                      {
                                          return std::string("Spark Engine Console v") + kConsoleVersion +
                                                 "\nFeatures: command aliases, command history\n"
                                                 "Build: Development";
                                      });
}

void ConsoleApp::RegisterDiagnosticCommands()
{
    m_commandRegistry.RegisterCommand("status", "Show console status information", "status",
                                      [this](const std::vector<std::string>& args) -> std::string
                                      {
                                          std::stringstream ss;
                                          ss << "Spark Engine Debug Console\n";
                                          ss << "Version: " << kConsoleVersion << "\n";
                                          ss << "Commands registered: " << m_commandRegistry.GetAllCommands().size()
                                             << "\n";
                                          ss << "History entries: " << m_commandHistory.size() << "\n";
                                          size_t bufferSize = 0;
                                          {
                                              std::lock_guard<std::mutex> lock(m_outputMutex);
                                              bufferSize = m_messageBuffer.size();
                                          }
                                          ss << "Buffer size: " << bufferSize << "/" << MAX_BUFFER_SIZE << "\n";
                                          ss << "Connection status: " << (m_running ? "Active" : "Disconnected");
                                          return ss.str();
                                      });

    m_commandRegistry.RegisterCommand("test_connection", "Test connection to engine", "test_connection",
                                      [this](const std::vector<std::string>& args) -> std::string
                                      {
                                          std::cout << "info" << std::endl;
                                          std::cout.flush();
                                          return "Test command sent to engine. Check for response above.";
                                      });

    m_commandRegistry.RegisterCommand("diag", "Show console diagnostic information", "diag",
                                      [this](const std::vector<std::string>& args) -> std::string
                                      {
                                          std::stringstream ss;
                                          ss << "SparkConsole Diagnostics:\n";
                                          ss << "  Console running: " << (m_running ? "Yes" : "No") << "\n";
                                          ss << "  Commands registered: " << m_commandRegistry.GetAllCommands().size()
                                             << "\n";
                                          size_t bufferSize = 0;
                                          {
                                              std::lock_guard<std::mutex> lock(m_outputMutex);
                                              bufferSize = m_messageBuffer.size();
                                          }
                                          ss << "  Message buffer size: " << bufferSize << "/" << MAX_BUFFER_SIZE
                                             << "\n";

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
}

void ConsoleApp::RegisterAliasCommands()
{
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
}
