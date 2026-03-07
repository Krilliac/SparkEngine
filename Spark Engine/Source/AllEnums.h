/**
 * @file AllEnums.h
 * @brief Master include file for all engine enumerations
 * @author Spark Engine Team
 * @date 2025
 * 
 * This header provides convenient access to all enumeration types
 * used throughout the Spark Engine and SparkEditor. Include this
 * file when you need access to multiple enum types.
 */

#pragma once

// SparkEditor Enums
#include "SparkEditor/Source/Enums/BuildSystemEnums.h"
#include "SparkEditor/Source/Enums/VersionControlEnums.h"
#include "SparkEditor/Source/Enums/CoreEditorEnums.h"
#include "SparkEditor/Source/Enums/SceneSystemEnums.h"
#include "SparkEditor/Source/Enums/ProfilerEnums.h"
#include "SparkEditor/Source/Enums/RenderingEnums.h"
#include "SparkEditor/Source/Enums/LevelStreamingEnums.h"
#include "SparkEditor/Source/Enums/AudioSystemEnums.h"

// Spark Engine Enums
#include "Spark Engine/Source/Enums/GraphicsEnums.h"
#include "Spark Engine/Source/Enums/InputEnums.h"

// Game Enums (from SparkGame project)
#include "SparkGame/Source/Enums/GameSystemEnums.h"

// Enum Utilities (validation, string conversion, iteration)
#include "Spark Engine/Source/Enums/EnumUtils.h"

/**
 * @brief Enum organization reference
 * 
 * This file organizes enumerations into the following categories:
 * 
 * ## SparkEditor Enums
 * - BuildSystemEnums.h: Build platforms, configurations, deployment targets
 * - VersionControlEnums.h: VCS types, file status, merge operations
 * - CoreEditorEnums.h: Editor states, panel types, dock positions
 * - SceneSystemEnums.h: Scene components, lights, cameras, physics
 * - ProfilerEnums.h: Performance profiling and optimization
 * - RenderingEnums.h: Graphics pipeline, shaders, rendering states
 * - LevelStreamingEnums.h: World streaming, LOD management
 * - AudioSystemEnums.h: Audio formats, playback, effects
 * 
 * ## Spark Engine Enums
 * - GameSystemEnums.h: Weapon types, player states, game mechanics (namespace SparkEditor)
 * - GraphicsEnums.h: Graphics API types, buffers, resources (namespace SparkEngine)
 * - InputEnums.h: Input devices, actions, bindings (namespace SparkEngine)
 * - EnumUtils.h: Validation, string conversion, iteration utilities (namespace SparkEditor)
 * 
 * ## Usage Guidelines
 * 
 * ### For specific functionality:
 * Include only the specific enum file you need:
 * ```cpp
 * #include "Enums/GameSystemEnums.h"  // For WeaponType, etc.
 * ```
 * 
 * ### For broad functionality:
 * Include this master header:
 * ```cpp
 * #include "Enums/AllEnums.h"  // All enums available
 * ```
 * 
 * ### Naming Conventions:
 * - Enum classes use PascalCase: `WeaponType`, `BuildPlatform`
 * - Enum values use UPPER_CASE: `PISTOL`, `WINDOWS_X64`
 * - Files use descriptive names ending in "Enums.h"
 * 
 * ### C++14 Compatibility:
 * All enums are designed to compile with C++14 standard
 * and use appropriate scoping and type safety features.
 */
