/**
 * @file SparkEngineMacOS.h
 * @brief macOS-specific engine entry-point helpers (Metal view, exe path).
 *
 * The shared SDL2/POSIX entry-point code in SparkEngineLinux.cpp calls these
 * helpers without any platform guards — each function is safe on every
 * platform and no-ops on non-macOS. Actual Cocoa / Metal / mach-o calls live
 * in SparkEngineMacOS.cpp and only compile under SPARK_PLATFORM_MACOS.
 */
#pragma once

#include "Platform.h"

#include <cstdint>
#include <filesystem>

namespace Spark::MacOS
{
    /**
     * @brief Decide whether the current session should use the Metal backend.
     *
     * Queries the RHI's recommended backend and confirms Metal support is
     * compiled in. Returns false on non-macOS builds.
     */
    bool ShouldPreferMetal();

    /**
     * @brief SDL window-creation flag used to request a Metal-capable window.
     * @return SDL_WINDOW_METAL on macOS, 0 on other platforms.
     */
    uint32_t GetMetalWindowFlag();

    /**
     * @brief Create an NSView-wrapped Metal view for an SDL_Window.
     *
     * The returned pointer is the NSView that MetalSwapChain::ConfigureMetalLayer
     * can attach to. Always returns nullptr on non-macOS.
     */
    void* CreateMetalView(void* sdlWindow);

    /**
     * @brief Destroy a view previously returned by CreateMetalView(). No-op on non-macOS.
     */
    void DestroyMetalView(void* metalView);

    /**
     * @brief Resolve the directory containing the running executable via
     *        _NSGetExecutablePath.
     *
     * Returns an empty path on non-macOS so callers can fall back to the
     * platform's native mechanism (e.g. /proc/self/exe on Linux).
     */
    std::filesystem::path GetExecutableDirectory();
} // namespace Spark::MacOS
