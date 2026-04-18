/**
 * @file RTSceneFeeder.h
 * @brief Utility that walks an ECS world and pushes every visible
 *        triangle mesh into a `HybridRTManager`, used to populate the
 *        hardware RT scene on macOS (Metal RT).
 *
 * This is a *helper*, not an auto-hooked system. Engine code (or a game
 * module) calls `PopulateRTSceneFromECS` once per scene change — or
 * every frame if transforms animate — to keep the BLAS/TLAS in sync
 * with the current world state. On non-macOS builds the underlying
 * `HybridRTManager::PushTriangleMesh` no-ops, so this is safe to call
 * unconditionally.
 *
 * Separate file because coupling HybridRTManager directly to the ECS
 * would drag the ECS layer into every consumer of the RT manager.
 * Keeping the walker as a free function lets game code wire in only
 * when RT scene feeding is actually needed.
 *
 * Intended call site (future): the graphics engine's render-frame
 * entry point, right before `HybridRTManager::Execute`. Today the
 * macOS render pipeline does not yet call `Execute` — when it does,
 * this helper is the step that precedes it.
 */

#pragma once

#include <cstdint>

class World;
class AssetPipeline;

namespace Spark::Graphics
{
    class HybridRTManager;

    /**
     * @brief Walk `world`, resolve every visible `MeshRenderer` through
     *        `assets`, and push the resulting triangle geometry into
     *        `rt`. Clears the RT scene first.
     *
     * @return Number of meshes actually pushed.
     *
     *         Off macOS (or when the RT backend is not
     *         `HardwareMetalRT`) this is a no-op and returns 0.
     *         Callers can use the return value to log "RT scene
     *         populated with N meshes" at scene-transition boundaries.
     */
    uint32_t PopulateRTSceneFromECS(HybridRTManager& rt, World& world, AssetPipeline& assets);

} // namespace Spark::Graphics
