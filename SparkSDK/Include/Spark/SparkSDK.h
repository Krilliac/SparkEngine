/**
 * @file SparkSDK.h
 * @brief Master include for the Spark Engine SDK
 *
 * Game modules can include this single header to get the full SDK surface:
 *   - IModule interface, ModuleInfo struct, and lifecycle hooks
 *   - IEngineContext service locator (26 subsystem getters)
 *   - ILogger logging interface
 *   - SPARK_IMPLEMENT_MODULE macro
 *   - Version and compatibility utilities
 *   - Export macros
 *   - Common math types (Vec3, Quat, Color, Mat4x4, AABB, Ray)
 *   - Input types (MouseButton, GamepadButton, InputAction)
 *   - Event types for pub/sub messaging
 */

#pragma once

#include "Version.h"
#include "SparkExport.h"
#include "IEngineContext.h"
#include "IModule.h"
#include "ILogger.h"
#include "ModuleABI.h"
#include "ModuleRegistry.h"
#include "MathTypes.h"
#include "InputTypes.h"
#include "EventTypes.h"
