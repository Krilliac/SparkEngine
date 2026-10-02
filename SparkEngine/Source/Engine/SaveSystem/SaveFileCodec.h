/**
 * @file SaveFileCodec.h
 * @brief Byte-level `.spark_save` codec: envelope, metadata block, entity records, v3->v4 migration
 *
 * Pure bytes in, bytes out, with no file I/O and no World access. SaveSystem owns reading the
 * file snapshot, durable writing and `.bak` recovery, and delegates every byte decision here,
 * so the parser a libFuzzer target exercises (FuzzerTests/FuzzSaveSystem.cpp) is exactly the
 * one Load(), QuickLoad(), GetSaveSlots() and the WriteToFile() retained-copy check run.
 *
 * Contract:
 *  - Thread affinity: async-safe. Every function is stateless and reentrant.
 *  - Ownership: callers own every buffer; outputs are written only on success.
 *  - Allocation: bounded by SaveRepresentationLimits. A declared length or count is checked
 *    against the bytes actually present (and the shared limits) before anything is sized from it.
 *  - Scalability: linear in the file size; a single pass plus one CRC-32 over the payload.
 */

#pragma once

#include "SaveSystemTypes.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Spark
{
    /// Whether @p version is inside the N-1..N window this build reads (owner decision OD-03).
    [[nodiscard]] bool IsSupportedSaveVersion(uint32_t version) noexcept;

    /**
     * @brief Whether @p fileData is a save written by a newer SparkEngine build.
     *
     * True only for an `SPRK` file declaring a version above kCurrentSaveVersion whose CRC-32
     * trailer does not verify under kCurrentSaveVersion. A file this build sealed whose version
     * field alone was damaged verifies there and is corruption, not a newer format.
     *
     * @param outVersion Set to the declared newer version when this returns true, else 0.
     */
    [[nodiscard]] bool IsNewerFormatSaveBytes(const std::vector<uint8_t>& fileData, uint32_t& outVersion) noexcept;

    /**
     * @brief Migrate an in-memory snapshot from any supported version to kCurrentSaveVersion.
     * @return false, leaving @p data unchanged, when its version is outside the supported window.
     */
    bool MigrateSaveDataToCurrentVersion(SaveData& data);

    /**
     * @brief Check an in-memory snapshot against every rule a save file must satisfy.
     *
     * The metadata must be encodable (no embedded newline, finite numbers), every length and
     * count must fit the wire format and the shared aggregate budget, and the world structure
     * must be restorable (no explicit NameComponent, no duplicate component type, Transform
     * parents that name a distinct saved entity with a Transform).
     *
     * @param operation Caller name used as the prefix of every diagnostic.
     */
    [[nodiscard]] bool ValidateSaveDataShape(const SaveData& data, const char* operation);

    /// The single Transform record of a serialized entity, or nullptr.
    [[nodiscard]] const SerializedComponent* FindTransformRecord(const SerializedEntity& entity);

    /**
     * @brief Decode a serialized Transform's parent as a saved-entity index (-1 = root).
     * @return false when the property is present but is not a decimal index >= -1.
     */
    [[nodiscard]] bool ParseTransformParentIndex(const SerializedComponent& transform, long long& outIndex);

    /**
     * @brief Decode a complete `.spark_save` file image into a kCurrentSaveVersion snapshot.
     *
     * Verifies the magic, the supported version window and the v4 CRC-32 trailer, parses every
     * record under the shared representation budget, rejects trailing bytes, duplicate keys and
     * an unrestorable world structure, and migrates a v3 file in memory.
     *
     * @param sourceName Label for diagnostics (the file path).
     * @param out        Written only on success; untouched when this returns false.
     */
    [[nodiscard]] bool DecodeSaveFileBytes(const std::vector<uint8_t>& fileData, const std::string& sourceName,
                                           SaveData& out);

    /**
     * @brief Decode only the envelope and metadata block of a `.spark_save` file image.
     *
     * Verifies the same envelope as DecodeSaveFileBytes (including the whole-file CRC-32) but
     * stops before the entity records, so slot enumeration never parses entity data.
     *
     * @param out Written only on success; untouched when this returns false.
     */
    [[nodiscard]] bool DecodeSaveMetadataBytes(const std::vector<uint8_t>& fileData, const std::string& sourceName,
                                               SaveMetadata& out);

    /**
     * @brief Encode a kCurrentSaveVersion snapshot as a complete `.spark_save` file image.
     *
     * Refuses any other version and any snapshot ValidateSaveDataShape rejects, so every image
     * it produces decodes again.
     *
     * @param outBytes Written only on success.
     */
    [[nodiscard]] bool EncodeSaveFileBytes(const SaveData& data, std::string& outBytes);

} // namespace Spark
