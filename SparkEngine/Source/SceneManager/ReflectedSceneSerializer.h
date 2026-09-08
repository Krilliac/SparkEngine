/**
 * @file ReflectedSceneSerializer.h
 * @brief Reflection-driven JSON scene serialization for World objects.
 */

#pragma once
#include <string>

class World;

namespace Spark
{

    /// Serialize/deserialize a World to a reflection-driven JSON scene.
    /// Every component that is registered in ComponentFactory + TypeRegistry is
    /// handled generically — no per-type code. Field types beyond the scalar/
    /// string/vector set the reflection layer round-trips are logged and skipped.
    std::string SerializeWorld(const World& world);
    bool DeserializeInto(World& world, const std::string& json);
    /// Save through a durable staging file and atomically replace the target.
    /// A valid prior image is retained as `<path>.bak` for recovery.
    bool SaveWorld(const World& world, const std::string& path);
    /// Load the primary image, falling back to `<path>.bak` when it is missing
    /// or invalid. Current and legacy `sceneVersion: 1` inputs are supported;
    /// ambiguous or unknown versions fail closed.
    bool LoadWorld(World& world, const std::string& path);

} // namespace Spark
