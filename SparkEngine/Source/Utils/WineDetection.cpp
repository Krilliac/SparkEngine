/**
 * @file WineDetection.cpp
 * @brief Wine runtime detection via `ntdll.dll::wine_get_version`, and
 *        gVisor detection via `/proc/1/comm` / `/proc/self/maps`.
 */

#include "WineDetection.h"
#include "LogMacros.h"

#include "../Core/Platform.h"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

#ifdef SPARK_PLATFORM_WINDOWS
#include <windows.h>
#endif

namespace Spark
{

    namespace
    {
        // ---- gVisor detection ------------------------------------------------
        //
        // gVisor's runsc user-mode kernel swallows the ucontext `trap_no` field
        // in signal handlers, which loops Wine's segv_handler on "Got unexpected
        // trap 0" during startup. Subsystems that care — the RHI factory when
        // auto-picking a backend, the crash handler, the Wine launcher probe —
        // can query this at startup and select a safer fallback tier
        // automatically. See .claude/knowledge/wine-gvisor-incompatibility.md
        // and .claude/knowledge/wine-role-and-fallback-tiers-2026-04-14.md.
        //
        // The probe is pure-C++ / POSIX (reads /proc text files), so it runs on
        // both native Linux and MinGW-under-Wine builds. On native Windows
        // /proc does not exist and the probe returns false.

        bool ReadFileFirstLine(const char* path, std::string& outLine)
        {
            std::ifstream f(path);
            if (!f.is_open())
            {
                return false;
            }
            std::getline(f, outLine);
            return true;
        }

        bool FileContainsSubstring(const char* path, const char* needle)
        {
            std::ifstream f(path);
            if (!f.is_open())
            {
                return false;
            }
            std::string line;
            while (std::getline(f, line))
            {
                if (line.find(needle) != std::string::npos)
                {
                    return true;
                }
            }
            return false;
        }

        bool DetectGvisorImpl()
        {
            // 1. Explicit override — useful for tests and for machines where
            //    the heuristic probe is wrong. Accept "1"/"true"/"yes" as true,
            //    "0"/"false"/"no" as false; any other value or absence means
            //    fall through to the real probe.
            if (const char* env = std::getenv("SPARK_FORCE_GVISOR"))
            {
                if (env[0] == '1' || env[0] == 't' || env[0] == 'T' || env[0] == 'y' || env[0] == 'Y')
                {
                    return true;
                }
                if (env[0] == '0' || env[0] == 'f' || env[0] == 'F' || env[0] == 'n' || env[0] == 'N')
                {
                    return false;
                }
            }

            // 2. /proc/1/comm — on user-mode-kernel sandboxes PID 1 is a
            //    well-known sentinel. We match any of these because they all
            //    exhibit the same ucontext trap_no / virtual_setup_exception
            //    failure modes that break Wine 9.0's signal handling:
            //      - "runsc" / "runsc-sandbox" — gVisor (Google)
            //      - "process_api"            — Claude Code harness
            //        (SparkEngine container), empirically confirmed 2026-04-14
            //        by reproducing the "Got unexpected trap 0" segv loop
            //        on Wine 9.0 inside it. See
            //        .claude/knowledge/wine-role-and-fallback-tiers-2026-04-14.md
            //
            //    If more sandbox families turn up with the same quirk, add
            //    their PID 1 comm here rather than broadening the function
            //    name — the contract is "this sandbox breaks Wine signal
            //    handling the same way gVisor does", not "this is literally
            //    runsc".
            std::string comm;
            if (ReadFileFirstLine("/proc/1/comm", comm))
            {
                if (comm.find("runsc") != std::string::npos || comm.find("process_api") != std::string::npos)
                {
                    return true;
                }
            }

            // 3. /proc/self/maps — gVisor's vDSO / stack entries carry the
            //    string "runsc" in their backing path. Secondary signal in
            //    case /proc/1 is masked (some container runtimes hide it).
            if (FileContainsSubstring("/proc/self/maps", "runsc"))
            {
                return true;
            }

            return false;
        }

        struct GvisorState
        {
            bool isGvisor = false;
        };

        const GvisorState& GetGvisorState()
        {
            static const GvisorState s = []()
            {
                GvisorState st{};
                st.isGvisor = DetectGvisorImpl();
                return st;
            }();
            return s;
        }
    } // namespace

    bool IsRunningUnderGvisor()
    {
        return GetGvisorState().isGvisor;
    }

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
        if (s.isWine)
        {
            if (!s.version.empty())
            {
                SPARK_LOG_INFO(LogCategory::Core, "Running under Wine %s (ntdll.dll::wine_get_version)",
                               s.version.c_str());
            }
            else
            {
                SPARK_LOG_INFO(LogCategory::Core, "Running under Wine (version unknown)");
            }
        }

        if (IsRunningUnderGvisor())
        {
            SPARK_LOG_WARN(LogCategory::Core,
                           "Running under gVisor (runsc user-mode kernel) — Wine signal handling "
                           "is known-incompatible. Subsystems should prefer the software-rendering "
                           "fallback ladder (tier 2 WineD3D+llvmpipe / tier 3 NullRHI / tier 4 "
                           "native Linux). See .claude/knowledge/wine-role-and-fallback-tiers-2026-04-14.md");
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
        // Wine branch is a no-op on non-Windows builds, but the gVisor branch
        // still runs — the native Linux launcher is tier 4 of the fallback
        // ladder and also benefits from knowing it's inside runsc so the RHI
        // factory can skip any GPU-backend auto-selection that would hit a
        // blocked ioctl.
        if (IsRunningUnderGvisor())
        {
            SPARK_LOG_WARN(LogCategory::Core,
                           "Running under gVisor (runsc user-mode kernel) — GPU ioctls and some "
                           "signal paths are restricted. RHI factory will prefer NullRHIDevice / "
                           "software Vulkan. See .claude/knowledge/wine-role-and-fallback-tiers-2026-04-14.md");
        }
    }

#endif // SPARK_PLATFORM_WINDOWS

} // namespace Spark
