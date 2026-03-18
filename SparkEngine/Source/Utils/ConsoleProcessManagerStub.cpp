/**
 * @file ConsoleProcessManagerStub.cpp
 * @brief Fallback stubs for unsupported platforms (macOS, etc.)
 *
 * Provides no-op implementations so the engine compiles on platforms
 * where subprocess management is not yet implemented.
 */

#include "Core/Platform.h"

#if !defined(SPARK_PLATFORM_WINDOWS) && !defined(SPARK_PLATFORM_LINUX)

#include "ConsoleProcessManager.h"
#include <iostream>

namespace Spark
{

    static std::string WStrToStr(const std::wstring& w)
    {
        std::string result;
        result.reserve(w.size());
        for (wchar_t c : w)
        {
            if (c < 0x80)
                result.push_back(static_cast<char>(c));
            else
                result.push_back('?');
        }
        return result;
    }

    bool ConsoleProcessManager::Initialize(const std::wstring&)
    {
        m_initialized = true;
        return true;
    }
    void ConsoleProcessManager::Shutdown()
    {
        m_initialized = false;
    }
    void ConsoleProcessManager::Log(const std::wstring& msg, const std::wstring& type)
    {
        std::cerr << "[" << WStrToStr(type) << "] " << WStrToStr(msg) << "\n";
    }
    void ConsoleProcessManager::LogCrash(const std::string& info)
    {
        std::cerr << "[CRASH] " << info << "\n";
    }
    bool ConsoleProcessManager::LaunchConsoleProcess(const std::wstring&)
    {
        return false;
    }
    bool ConsoleProcessManager::ReadFromConsole()
    {
        return false;
    }
    bool ConsoleProcessManager::WriteToConsole(const std::wstring&)
    {
        return false;
    }
    void ConsoleProcessManager::ConsoleThreadMain() {}

} // namespace Spark

#endif // !SPARK_PLATFORM_WINDOWS && !SPARK_PLATFORM_LINUX
