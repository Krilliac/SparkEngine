/**
 * @file SparkEngineMacOS.cpp
 * @brief macOS-specific engine entry-point helpers (Metal view lifecycle, exe path).
 *
 * On macOS, this file implements the hooks declared in SparkEngineMacOS.h
 * using SDL_Metal, mach-o/dyld, and the RHI bridge. On every other platform,
 * the same functions compile as trivial no-op stubs so SparkEngineLinux.cpp
 * can call them unconditionally without platform guards.
 *
 * Windows counterpart lives in SparkEngineWindows.cpp; shared POSIX logic
 * lives in SparkEngineLinux.cpp (name is historical — it runs on macOS too).
 */
#include "SparkEngineMacOS.h"

#ifdef SPARK_PLATFORM_MACOS

#include "Utils/SparkConsole.h"
#include "Graphics/RHI/RHIBridge.h"
#include "Graphics/RHI/RHITypes.h"
#include "Utils/LogMacros.h"
#include "Utils/Logger.h"

#include <mach-o/dyld.h>
#include <string>
#include <system_error>

#ifdef SPARK_SDL2_AVAILABLE
#include <SDL.h>
#include <SDL_metal.h>
#endif

namespace Spark::MacOS
{

    bool ShouldPreferMetal()
    {
#if defined(SPARK_METAL_SUPPORT) && defined(SPARK_SDL2_AVAILABLE)
        const auto recommended = Spark::RHI::RHIBridge::GetRecommendedBackend();
        if (recommended == Spark::RHI::GraphicsBackend::Metal)
        {
            SPARK_LOG_INFO(Spark::LogCategory::Graphics, "SDL2 on macOS — creating Metal-backed window");
            return true;
        }
#endif
        return false;
    }

    uint32_t GetMetalWindowFlag()
    {
#ifdef SPARK_SDL2_AVAILABLE
        return SDL_WINDOW_METAL;
#else
        return 0;
#endif
    }

    void* CreateMetalView(void* sdlWindow)
    {
#if defined(SPARK_METAL_SUPPORT) && defined(SPARK_SDL2_AVAILABLE)
        if (!sdlWindow)
            return nullptr;

        // SDL_Metal_CreateView returns an NSView whose .layer is already a
        // CAMetalLayer — MetalSwapChain::ConfigureMetalLayer reuses that layer
        // rather than replacing it. Caller passes this pointer through as the
        // NativeWindowHandle to GraphicsEngine::Initialize.
        SDL_MetalView view = SDL_Metal_CreateView(static_cast<SDL_Window*>(sdlWindow));
        if (!view)
        {
            Spark::SimpleConsole::GetInstance().LogError(std::string("SDL_Metal_CreateView failed: ") + SDL_GetError());
            return nullptr;
        }
        return static_cast<void*>(view);
#else
        (void)sdlWindow;
        return nullptr;
#endif
    }

    void DestroyMetalView(void* metalView)
    {
#if defined(SPARK_METAL_SUPPORT) && defined(SPARK_SDL2_AVAILABLE)
        if (metalView)
            SDL_Metal_DestroyView(static_cast<SDL_MetalView>(metalView));
#else
        (void)metalView;
#endif
    }

    std::filesystem::path GetExecutableDirectory()
    {
        uint32_t size = 0;
        _NSGetExecutablePath(nullptr, &size);
        std::string exePath(size, '\0');
        if (_NSGetExecutablePath(exePath.data(), &size) == 0)
        {
            std::error_code ec;
            auto canonical = std::filesystem::weakly_canonical(std::filesystem::path(exePath.c_str()), ec);
            if (!ec)
                return canonical.parent_path();
        }
        return std::filesystem::current_path();
    }

} // namespace Spark::MacOS

#else // !SPARK_PLATFORM_MACOS

// Non-macOS stubs so the translation unit provides symbols on every platform
// and SparkEngineLinux.cpp can call the hooks unconditionally.
namespace Spark::MacOS
{
    bool ShouldPreferMetal()
    {
        return false;
    }

    uint32_t GetMetalWindowFlag()
    {
        return 0;
    }

    void* CreateMetalView(void*)
    {
        return nullptr;
    }

    void DestroyMetalView(void*) {}

    std::filesystem::path GetExecutableDirectory()
    {
        return {};
    }
} // namespace Spark::MacOS

#endif // SPARK_PLATFORM_MACOS
