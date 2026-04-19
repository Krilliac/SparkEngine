/**
 * @file LauncherBackend.h
 * @brief Platform backend API for SparkLauncher (Win32/D3D11 or SDL2/OpenGL).
 * @author Spark Engine Team
 * @date 2025
 */

#pragma once

namespace SparkLauncher
{
    bool Backend_Init(int width, int height, const char* title);
    void Backend_Shutdown();
    bool Backend_BeginFrame(); // returns false when user requested quit
    void Backend_EndFrame();
} // namespace SparkLauncher
