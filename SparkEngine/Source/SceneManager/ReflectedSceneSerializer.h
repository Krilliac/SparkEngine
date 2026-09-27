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
    /// whole text (and then its JSON tree) is allocated. SaveWorld refuses to
    /// write a document over it, so a save can never produce a file LoadWorld
    /// rejects.
    inline constexpr uint64_t kMaxSceneDocumentBytes = 64ull * 1024ull * 1024ull;

    /// Most JSON values (as bounded by nlohmann::json::count_values_upper_bound)
    /// an untrusted scene document may hold. The byte cap alone does not bound
    /// breadth: "[0,0,0,..." is two bytes per value, and each parsed value is a
    /// full JSON node. SerializeWorld's indented output spends at least ~25 bytes
    /// per value, so any document under kMaxSceneDocumentBytes that the writer
    /// produces stays well inside this budget; SaveWorld checks it anyway.
    inline constexpr uint64_t kMaxSceneDocumentValues = 4000000ull;

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
     *
     * TrustedSnapshot is only for text this process just produced with
     * SerializeWorld and never wrote to or read from disk (editor undo/redo and
     * play-in-editor restore). It skips the byte and value caps, and a field
     * whose value cannot be applied (SerializeWorld writes a NaN or infinite
     * float as "nan"/"inf", which the reader refuses) keeps its component
     * default instead of failing the whole restore. Never pass it for file,
     * network or recovery-record input.
     */
    enum class SceneDeserializeMode : uint8_t
    {
        Permissive,
        StrictRecovery,
        TrustedSnapshot
    };

    /// Serialize/deserialize a World to a reflection-driven JSON scene.
    /// Every component that is registered in ComponentFactory + TypeRegistry is
    /// handled generically — no per-type code. Field types beyond the scalar/
    /// string/vector set the reflection layer round-trips are logged and skipped.
    /// The result is suitable for in-process snapshots (TrustedSnapshot); use
    /// TrySerializeWorld for anything that must load back through LoadWorld.
    std::string SerializeWorld(const World& world);
    /**
     * @brief Serialize @p world for persistence, failing closed when the reader would reject the result.
     *
     * Fails when a round-trippable Float, Double or Vector field holds NaN or infinity
     * (the reader accepts only finite values), when the document exceeds
     * kMaxSceneDocumentBytes, or when it holds more than kMaxSceneDocumentValues values.
     * @param world Source world.
     * @param out Receives the document on success; untouched on failure.
     * @param error When non-null, cleared on entry and set on failure to a reason naming
     *        the entity and field, or the size that was exceeded.
     * @return true when @p out holds a document LoadWorld will accept.
     */
    bool TrySerializeWorld(const World& world, std::string& out, std::string* error = nullptr);
    /**
     * @brief Deserialize a reflected scene document into @p world.
     * @param world Destination world.
     * @param json Scene document text.
     * @param mode Permissive for authored scenes, StrictRecovery for crash-recovery records,
     *        TrustedSnapshot only for in-process SerializeWorld output (see SceneDeserializeMode).
     * @param error When non-null, cleared on entry and set on rejection to an actionable
     *        reason. Version rejections name the file's version field and value and the
     *        supported read/write window.
     * @return true when the document was applied.
     */
    bool DeserializeInto(World& world, const std::string& json,
                         SceneDeserializeMode mode = SceneDeserializeMode::Permissive, std::string* error = nullptr);
    /// Save through a durable staging file and atomically replace the target.
    /// A valid prior image is retained as `<path>.bak` for recovery. The world is
    /// serialized with TrySerializeWorld first: a world LoadWorld could not read
    /// back (non-finite field, over the size or value caps) is refused without
    /// touching the target or its backup. When @p error is non-null it is cleared
    /// on entry and set on failure to the reason.
    bool SaveWorld(const World& world, const std::string& path, std::string* error = nullptr);
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
