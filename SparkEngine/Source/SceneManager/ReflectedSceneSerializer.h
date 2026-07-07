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
    bool SaveWorld(const World& world, const std::string& path);
    bool LoadWorld(World& world, const std::string& path);

} // namespace Spark
