#include "ConsoleApp.h"
#include <iostream>
#include <string_view>
#include <tuple>
#ifdef SPARK_PLATFORM_WINDOWS
#include <windows.h>
#include <conio.h>
#endif // SPARK_PLATFORM_WINDOWS

#ifdef SPARK_PLATFORM_WINDOWS
namespace
{
    bool HasInteractiveConsoleInput()
    {
        DWORD mode = 0;
        HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
        return input != nullptr && input != INVALID_HANDLE_VALUE && GetConsoleMode(input, &mode) != FALSE;
    }
} // namespace
#endif

int main(int argc, char* argv[])
{
    bool enginePipeRequested = false;
    for (int i = 1; i < argc; ++i)
    {
        if (std::string_view(argv[i]) == "--engine-pipe")
        {
            enginePipeRequested = true;
        }
    }

#ifdef SPARK_PLATFORM_WINDOWS
    // Everything a human reads goes to the console screen buffer. In engine-pipe
    // mode STD_OUTPUT_HANDLE is the command pipe: configuring or printing to it
    // would silently fail and, worse, feed this text to the engine as commands.
    // Resolve the display handle first — it is what attaches a console at all.
    HANDLE hConsole = ConsoleApp::DisplayHandle();

    // Set console title and properties
    SetConsoleTitleW(L"Spark Engine Debug Console");
    SetConsoleOutputCP(CP_UTF8);
    CONSOLE_SCREEN_BUFFER_INFO csbi{};
    if (GetConsoleScreenBufferInfo(hConsole, &csbi))
    {
        COORD bufferSize = {static_cast<SHORT>(csbi.srWindow.Right - csbi.srWindow.Left + 1), 5000};
        SetConsoleScreenBufferSize(hConsole, bufferSize);
    }

    // Enable ANSI color codes (Windows 10+)
    DWORD mode = 0;
    if (GetConsoleMode(hConsole, &mode))
    {
        mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
        SetConsoleMode(hConsole, mode);
    }

    ConsoleApp::WriteDisplay(L"Spark Engine Console v1.0.0\n"
                             L"Waiting for engine connection...\n"
                             L"Type 'help' for available commands\n"
                             L"========================================\n");
#else
    // Narrow std::cerr, matching ConsoleApp.cpp's POSIX branch: stdout is the
    // engine's command queue, so human-facing text goes to stderr. It must be
    // the same stream *orientation* too -- the first insertion fixes stderr as
    // byte- or wide-oriented, and every later insertion of the other width
    // silently writes nothing. Printing this banner through std::wcerr made the
    // whole narrow-std::cerr display path in ConsoleApp.cpp go dark.
    std::cerr << "Spark Engine Console v1.0.0" << std::endl;
    std::cerr << "Waiting for engine connection..." << std::endl;
    std::cerr << "Type 'help' for available commands" << std::endl;
    std::cerr << "========================================" << std::endl;
#endif // SPARK_PLATFORM_WINDOWS

    try
    {
        ConsoleApp app(enginePipeRequested);
        app.Run();
    }
    catch (const std::exception& e)
    {
        std::cerr << "Console error: " << e.what() << std::endl;
#ifdef SPARK_PLATFORM_WINDOWS
        ConsoleApp::WriteDisplay(L"Press any key to continue...\n");
        if (HasInteractiveConsoleInput())
        {
            std::ignore = _getch();
        }
#else
        std::cin.get();
#endif
        return 1;
    }

#ifdef SPARK_PLATFORM_WINDOWS
    ConsoleApp::WriteDisplay(L"Console application finished. Press any key to exit...\n");
    if (HasInteractiveConsoleInput())
    {
        std::ignore = _getch();
    }
#else
    std::cin.get();
#endif
    return 0;
}
