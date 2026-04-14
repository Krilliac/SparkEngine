/**
 * @file WineDetection.cpp
 * @brief Wine runtime detection via `ntdll.dll::wine_get_version`
 */

#include "WineDetection.h"
#include "LogMacros.h"

#include "../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <windows.h>
#endif

namespace Spark
{

#ifdef SPARK_PLATFORM_WINDOWS

    namespace
    {
        // Pointer-to-function type of the optional Wine export.
        using WineGetVersionFn = const char*(__cdecl*)(void);

        struct WineState
        {
            bool isWine = false;
            std::string version;
        };

        const WineState& GetState()
        {
            static const WineState s = []()
            {
                WineState st{};
                // ntdll.dll is already loaded in every Windows process, so
                // GetModuleHandleA is sufficient — no LoadLibrary needed.
                HMODULE ntdll = GetModuleHandleA("ntdll.dll");
                if (!ntdll)
                {
                    return st;
                }
                auto fn = reinterpret_cast<WineGetVersionFn>(
                    reinterpret_cast<void*>(GetProcAddress(ntdll, "wine_get_version")));
                if (!fn)
                {
                    return st;
                }
                st.isWine = true;
                const char* v = fn();
                if (v != nullptr)
                {
                    st.version = v;
                }
                return st;
            }();
            return s;
        }
    } // namespace

    bool IsRunningUnderWine()
    {
        return GetState().isWine;
    }

    const std::string& GetWineVersion()
    {
        return GetState().version;
    }

    void LogWineEnvironmentIfApplicable()
    {
        const auto& s = GetState();
        if (!s.isWine)
        {
            return;
        }
        if (!s.version.empty())
        {
            SPARK_LOG_INFO(LogCategory::Core, "Running under Wine %s (ntdll.dll::wine_get_version)", s.version.c_str());
        }
        else
        {
            SPARK_LOG_INFO(LogCategory::Core, "Running under Wine (version unknown)");
        }
    }

#else // !SPARK_PLATFORM_WINDOWS

    bool IsRunningUnderWine()
    {
        return false;
    }

    const std::string& GetWineVersion()
    {
        static const std::string empty;
        return empty;
    }

    void LogWineEnvironmentIfApplicable()
    {
        // no-op on non-Windows builds
    }

#endif // SPARK_PLATFORM_WINDOWS

} // namespace Spark
