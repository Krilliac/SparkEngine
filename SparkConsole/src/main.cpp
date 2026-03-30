#include "ConsoleApp.h"
#include <iostream>
#ifdef SPARK_PLATFORM_WINDOWS
#include <windows.h>
#include <conio.h>
#endif // SPARK_PLATFORM_WINDOWS

int main()
{
#ifdef SPARK_PLATFORM_WINDOWS
    // Set console title and properties
    SetConsoleTitleW(L"Spark Engine Debug Console");
    SetConsoleOutputCP(CP_UTF8);

    // Configure console buffer size for lots of scrollback
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    GetConsoleScreenBufferInfo(hConsole, &csbi);
    COORD bufferSize = {csbi.srWindow.Right - csbi.srWindow.Left + 1, 5000};
    SetConsoleScreenBufferSize(hConsole, bufferSize);

    // Enable ANSI color codes (Windows 10+)
    DWORD mode = 0;
    GetConsoleMode(hConsole, &mode);
    mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    SetConsoleMode(hConsole, mode);
#endif // SPARK_PLATFORM_WINDOWS

    std::wcout << L"Spark Engine Console v1.0.0" << std::endl;
    std::wcout << L"Waiting for engine connection..." << std::endl;
    std::wcout << L"Type 'help' for available commands" << std::endl;
    std::wcout << L"========================================" << std::endl;

    try
    {
        ConsoleApp app;
        app.Run();
    }
    catch (const std::exception& e)
    {
        std::cerr << "Console error: " << e.what() << std::endl;
        std::wcout << L"Press any key to continue..." << std::endl;
#ifdef SPARK_PLATFORM_WINDOWS
        _getch();
#else
        std::cin.get();
#endif
        return 1;
    }

    std::wcout << L"Console application finished. Press any key to exit..." << std::endl;
#ifdef SPARK_PLATFORM_WINDOWS
    _getch();
#else
    std::cin.get();
#endif
    return 0;
}
