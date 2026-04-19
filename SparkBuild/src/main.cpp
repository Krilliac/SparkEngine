#include "Platform.h"
#include "SparkBuild.h"
#include <iostream>
#include <string>

#ifdef SPARK_PLATFORM_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

int main(int argc, char* argv[])
{
#ifdef SPARK_PLATFORM_WINDOWS
    // Enable UTF-8 console output
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif

    // Check for help/version flags
    if (argc > 1)
    {
        std::string arg = argv[1];
        if (arg == "--help" || arg == "-h")
        {
            std::cout << "SparkBuild - SparkEngine Build Tool v2.1\n\n";
            std::cout << "Usage:\n";
            std::cout << "  sparkbuild              Run interactive TUI\n";
            std::cout << "  sparkbuild --help       Show this help\n";
            std::cout << "  sparkbuild --version    Show version\n\n";
            std::cout << "Platform: " SPARK_PLATFORM_NAME "\n";
            return 0;
        }
        if (arg == "--version" || arg == "-v")
        {
            std::cout << "SparkBuild v2.1.0 (" SPARK_PLATFORM_NAME ")\n";
            return 0;
        }
    }

    // Interactive TUI mode
    SparkBuild::SparkBuildApp app;
    return app.Run();
}
