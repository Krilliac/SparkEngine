/**
 * @file RHI.h
 * @brief Master include for the Rendering Hardware Interface
 * @author Spark Engine Team
 * @date 2025
 *
 * Single-include header that pulls in the entire RHI abstraction layer.
 * Use this header in application code instead of including individual RHI headers.
 *
 * Supported backends:
 *   - D3D11  (Windows, always available)
 *   - Vulkan (Windows/Linux/macOS via MoltenVK, requires SPARK_VULKAN_SUPPORT)
 *   - OpenGL (Windows/Linux, requires SPARK_OPENGL_SUPPORT)
 *
 * Usage:
 *   #include "Graphics/RHI/RHI.h"
 *
 *   // Create a device with auto-detection
 *   auto device = Spark::RHI::RHIFactory::CreateDevice(Spark::RHI::GraphicsBackend::Auto);
 *
 *   // Or use the bridge for a higher-level API
 *   Spark::RHI::RHIBridge bridge;
 *   bridge.Initialize(hwnd, 1280, 720, Spark::RHI::GraphicsBackend::Vulkan);
 */

#pragma once

// Core RHI types and enumerations
#include "RHITypes.h"

// Abstract resource interfaces
#include "RHIResources.h"

// Abstract device and command list interfaces
#include "RHIDevice.h"

// Backend factory and shader compilation
#include "RHIFactory.h"

// High-level integration bridge
#include "RHIBridge.h"
