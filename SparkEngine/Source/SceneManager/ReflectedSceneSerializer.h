/**
 * @file ReflectedSceneSerializer.h
 * @brief Reflection-driven JSON scene serialization for World objects.
 */

#pragma once
#include <cstdint>
#include <string>

class World;

namespace Spark
{

    /// Largest scene document LoadWorld, SaveWorld's previous-image check and
    /// DeserializeInto accept. The editor's contained project reader applies the
    /// same bound, so every scene entry point rejects an oversized file before the
    /// whole text (and then its JSON tree) is allocated.
    inline constexpr uint64_t kMaxSceneDocumentBytes = 64ull * 1024ull * 1024ull;

    /**
     * Permissive loading preserves legacy authored scenes (`sceneVersion: 1`,
     * whose inline values are converted leniently) and tolerates schema drift in
     * current documents: unregistered component types are skipped and missing
     * fields keep their defaults. A current-version field that IS present must
     * still be a string the reflection layer can apply; a wrong JSON type or an
     * unparsable value rejects the document, so LoadWorld falls back to the
     * previous-good backup instead of installing (and later re-saving) defaults.
     *
     * StrictRecovery is reserved for crash-recovery records: every
     * current-schema entity, component, and reflected field must be understood
     * and restored.
     */
    enum class SceneDeserializeMode : uint8_t
    {
        Permissive,
        StrictRecovery
    };

    /// Serialize/deserialize a World to a reflection-driven JSON scene.
    /// Every component that is registered in ComponentFactory + TypeRegistry is
    /// handled generically — no per-type code. Field types beyond the scalar/
    /// string/vector set the reflection layer round-trips are logged and skipped.
    std::string SerializeWorld(const World& world);
    /**
     * @brief Deserialize a reflected scene document into @p world.
     * @param world Destination world.
     * @param json Scene document text.
     * @param mode Permissive for authored scenes, StrictRecovery for crash-recovery records.
     * @param error When non-null, cleared on entry and set on rejection to an actionable
     *        reason. Version rejections name the file's version field and value and the
     *        supported read/write window.
     * @return true when the document was applied.
     */
    bool DeserializeInto(World& world, const std::string& json,
                         SceneDeserializeMode mode = SceneDeserializeMode::Permissive, std::string* error = nullptr);
    /// Save through a durable staging file and atomically replace the target.
    /// A valid prior image is retained as `<path>.bak` for recovery.
    bool SaveWorld(const World& world, const std::string& path);
    /**
     * @brief Load the primary image, falling back to `<path>.bak` when it is missing or invalid.
     *
     * Current `version: 1` and legacy editor `sceneVersion: 1` inputs are supported;
     * ambiguous or unknown versions fail closed and leave @p world untouched.
     * @param error When non-null, set on failure to a diagnostic naming the file, why the
     *        primary image was rejected (including its version and the supported window),
     *        and why the backup could not be used.
     * @return true when the primary or the backup image was loaded.
     */
    bool LoadWorld(World& world, const std::string& path, std::string* error = nullptr);

} // namespace Spark
