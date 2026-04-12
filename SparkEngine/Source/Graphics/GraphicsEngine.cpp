/**
 * @file GraphicsEngine.cpp
 * @brief Central rendering orchestrator — platform-split redirect
 *
 * All implementations have been split into platform-specific files:
 *   - GraphicsEngineWindows.cpp — D3D11 rendering orchestrator
 *   - GraphicsEngineLinux.cpp   — RHI-bridge rendering orchestrator
 *
 * Rendering pipeline implementations are in GraphicsRenderPipelines*.cpp.
 * Device/resource creation is in GraphicsDeviceResources*.cpp.
 * State/settings/metrics are in GraphicsStateAndSettings*.cpp.
 * Console integration is in GraphicsConsoleOps.cpp.
 */
