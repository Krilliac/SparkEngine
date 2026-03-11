/**
 * @file SparkArchitecture.h
 * @brief Master include for all architecture extensions
 *
 * Single-include header that provides access to all architecture
 * improvements implemented from the ARCHITECTURE_ANALYSIS.md document.
 * Include this header to access the full integrated feature set.
 *
 * ## Architecture components
 *
 * ### Core (R1)
 * - EngineContext: Unified generic registry with dependency-aware init
 * - EngineBootstrap: Topological-sort subsystem initialization
 * - AssetHandle: FNV-1a hashed asset identifiers
 * - AssetRegistry: Handle-to-resource mapping
 * - EngineSetup: Wiring helpers for engine startup
 *
 * ### ECS (R2)
 * - PhaseSystemManager: Deterministic phase-ordered execution
 * - FixedTimestepPhysics: Accumulator-based physics stepping
 * - ReactiveSystem: EnTT signal-based change tracking
 * - ParallelSystemExecutor: Automatic parallel system execution
 *
 * ### Graphics (R3)
 * - RenderDevice: Thin device + swap chain wrapper
 * - RenderPipeline: Frame graph execution manager
 * - SceneRenderer: ECS draw command collection with culling
 * - RenderGraphBuilder: Standard pass construction
 * - RHIAdapter: Legacy DX11 bridge to abstract RHI
 *
 * ### AI (R4)
 * - ParallelPerception: Octree + JobSystem perception
 * - AIBudgetLimiter: Per-frame time budget with priorities
 * - NavMeshObstacles: Dynamic obstacle carving
 * - AIIntegratedSystem: Unified AI pipeline
 *
 * ### Animation (R5)
 * - Split headers: Skeleton, Clip, Blender, StateMachine, Evaluator, IK
 * - AnimationCompression: Keyframe reduction + quantization
 * - AnimationPipeline: Compressed clip management
 *
 * ### Networking (R6)
 * - ITransport: Pluggable transport (UDP, Steam)
 * - NetworkSecurity: Encryption and connection tokens
 * - NetworkStack: Integrated network management
 *
 * ### Editor (R7)
 * - EditorPluginManager: DLL plugin loading
 * - EditorConsoleBridge: Console consolidation
 *
 * @see docs/ARCHITECTURE_ANALYSIS.md for the full recommendation list
 */

#pragma once

// ============================================================================
// Core architecture (R1)
// ============================================================================
#include "Core/EngineContext.h"
#include "Core/EngineBootstrap.h"
#include "Core/AssetHandle.h"
#include "Core/AssetIntegration.h"
#include "Core/EngineSetup.h"

// ============================================================================
// ECS architecture (R2)
// ============================================================================
#include "Engine/ECS/ECSIntegration.h"

// ============================================================================
// Graphics architecture (R3)
// ============================================================================
#include "Graphics/GraphicsIntegration.h"

// ============================================================================
// AI architecture (R4)
// ============================================================================
#include "Engine/AI/AIIntegration.h"

// ============================================================================
// Animation architecture (R5)
// ============================================================================
#include "Engine/Animation/AnimationIntegration.h"

// ============================================================================
// Networking architecture (R6)
// ============================================================================
#include "Engine/Networking/NetworkIntegration.h"

// ============================================================================
// Precompiled header (R9.2)
// ============================================================================
#include "SparkPCH.h"
