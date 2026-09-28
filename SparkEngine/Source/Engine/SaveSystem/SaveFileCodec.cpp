/**
 * @file SaveFileCodec.cpp
 * @brief Byte-level `.spark_save` codec shared by SaveSystem and its libFuzzer target
 */

#include "SaveFileCodec.h"
#include "../../Utils/CRC32.h"
#include "../../Utils/LogMacros.h"
#include <array>
#include <charconv>
#include <cmath>
#include <cstring>
#include <exception>
#include <sstream>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace Spark
{
    namespace
    {
        constexpr uint32_t kChecksummedSaveVersion = 4;
        constexpr size_t kSaveChecksumBytes = sizeof(uint32_t);

        uint16_t DecodeLittleEndian16(const uint8_t* bytes) noexcept
        {
            return static_cast<uint16_t>(bytes[0]) | (static_cast<uint16_t>(bytes[1]) << 8u);
        }

        uint32_t DecodeLittleEndian32(const uint8_t* bytes) noexcept
        {
            return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8u) |
                   (static_cast<uint32_t>(bytes[2]) << 16u) | (static_cast<uint32_t>(bytes[3]) << 24u);
        }

        std::array<uint8_t, 2> EncodeLittleEndian16(uint16_t value) noexcept
        {
            return {static_cast<uint8_t>(value & 0xFFu), static_cast<uint8_t>((value >> 8u) & 0xFFu)};
        }

        std::array<uint8_t, 4> EncodeLittleEndian32(uint32_t value) noexcept
        {
            return {static_cast<uint8_t>(value & 0xFFu), static_cast<uint8_t>((value >> 8u) & 0xFFu),
                    static_cast<uint8_t>((value >> 16u) & 0xFFu), static_cast<uint8_t>((value >> 24u) & 0xFFu)};
        }

        /// @brief Whether the file's CRC-32 trailer verifies once its header version field
        ///        is replaced by @p headerVersion.
        ///
        /// Recognizes a checksummed file whose version field alone was damaged after the
        /// writer sealed it: the trailer still covers the original header bytes.
        bool TrailerVerifiesWithHeaderVersion(const std::vector<uint8_t>& fileData, uint32_t headerVersion) noexcept
        {
            if (fileData.size() < 8u + kSaveChecksumBytes)
                return false;
            const size_t candidatePayloadEnd = fileData.size() - kSaveChecksumBytes;
            CRC32 candidate;
            candidate.Update(fileData.data(), 4u);
            const auto encodedVersion = EncodeLittleEndian32(headerVersion);
            candidate.Update(encodedVersion.data(), encodedVersion.size());
            if (candidatePayloadEnd > 8u)
                candidate.Update(fileData.data() + 8u, candidatePayloadEnd - 8u);
            return candidate.Finalize() == DecodeLittleEndian32(fileData.data() + candidatePayloadEnd);
        }

        void LogUnsupportedSaveVersion(const std::string& filepath, uint32_t version, const char* operation)
        {
            if (version > kCurrentSaveVersion)
            {
                SPARK_LOG_WARN(Spark::LogCategory::Save,
                               "%s: save '%s' uses version %u, but this build supports versions %u..%u; "
                               "load it with a newer SparkEngine build",
                               operation, filepath.c_str(), version, kOldestSupportedSaveVersion, kCurrentSaveVersion);
            }
            else
            {
                SPARK_LOG_WARN(Spark::LogCategory::Save,
                               "%s: save '%s' uses version %u, but this build supports versions %u..%u; "
                               "restore or convert the save with a compatible older build",
                               operation, filepath.c_str(), version, kOldestSupportedSaveVersion, kCurrentSaveVersion);
            }
        }

        bool ValidateSaveEnvelope(const std::vector<uint8_t>& fileData, const std::string& filepath,
                                  const char* operation, uint32_t& outVersion, size_t& outPayloadEnd)
        {
            if (fileData.size() < 8u)
            {
                SPARK_LOG_WARN(Spark::LogCategory::Save, "%s: save file '%s' too small (%zu bytes, need at least 8)",
                               operation, filepath.c_str(), fileData.size());
                return false;
            }
            if (std::memcmp(fileData.data(), "SPRK", 4) != 0)
            {
                SPARK_LOG_WARN(Spark::LogCategory::Save, "%s: invalid magic in '%s' (expected 'SPRK')", operation,
                               filepath.c_str());
                return false;
            }

            const uint32_t version = DecodeLittleEndian32(fileData.data() + 4);
            if (!IsSupportedSaveVersion(version))
            {
                LogUnsupportedSaveVersion(filepath, version, operation);
                return false;
            }

            size_t payloadEnd = fileData.size();
            if (version >= kChecksummedSaveVersion)
            {
                if (fileData.size() < 8u + kSaveChecksumBytes)
                {
                    SPARK_LOG_WARN(Spark::LogCategory::Save, "%s: version %u save '%s' has no CRC32 trailer", operation,
                                   version, filepath.c_str());
                    return false;
                }
                payloadEnd -= kSaveChecksumBytes;
                const uint32_t expected = DecodeLittleEndian32(fileData.data() + payloadEnd);
                const uint32_t actual = ComputeCRC32(fileData.data(), payloadEnd);
                if (expected != actual)
                {
                    SPARK_LOG_WARN(Spark::LogCategory::Save, "%s: CRC32 mismatch in '%s' (expected %08x, actual %08x)",
                                   operation, filepath.c_str(), static_cast<unsigned>(expected),
                                   static_cast<unsigned>(actual));
                    return false;
                }
            }
            else if (TrailerVerifiesWithHeaderVersion(fileData, kChecksummedSaveVersion))
            {
                // A damaged v4 version field must not downgrade into the legacy
                // metadata-only path and bypass integrity verification. Reconstruct
                // the v4 header and recognize its still-present checksum trailer.
                SPARK_LOG_WARN(Spark::LogCategory::Save, "%s: save '%s' has a corrupted v4 version field", operation,
                               filepath.c_str());
                return false;
            }

            outVersion = version;
            outPayloadEnd = payloadEnd;
            return true;
        }

        bool ParseMetadataBlock(uint32_t sourceVersion, const std::string& metadataBlock, SaveMetadata& outMetadata)
        {
            SaveMetadata parsedMetadata;
            parsedMetadata.version = sourceVersion;

            std::istringstream stream(metadataBlock);
            if (!std::getline(stream, parsedMetadata.saveName) || !std::getline(stream, parsedMetadata.sceneName) ||
                !std::getline(stream, parsedMetadata.playerClass))
            {
                return false;
            }

            if (!std::getline(stream, parsedMetadata.screenshotPath))
                return false;

            stream >> parsedMetadata.timestamp;
            stream >> parsedMetadata.playTime;
            stream >> parsedMetadata.playerHealth;
            stream >> parsedMetadata.playerArmor;
            stream >> parsedMetadata.playerPosition.x >> parsedMetadata.playerPosition.y >>
                parsedMetadata.playerPosition.z;
            stream >> parsedMetadata.playerKills;
            stream >> parsedMetadata.playerDeaths;
            if (!stream)
                return false;
            if (!std::isfinite(parsedMetadata.playTime) || !std::isfinite(parsedMetadata.playerHealth) ||
                !std::isfinite(parsedMetadata.playerArmor) || !std::isfinite(parsedMetadata.playerPosition.x) ||
                !std::isfinite(parsedMetadata.playerPosition.y) || !std::isfinite(parsedMetadata.playerPosition.z))
            {
                return false;
            }

            stream >> std::ws;
            if (!stream.eof())
                return false;

            outMetadata = std::move(parsedMetadata);
            return true;
        }

        bool BuildMetadataBlock(const SaveMetadata& metadata, const char* operation, std::string& outBlock)
        {
            auto rejectNewline = [&](const std::string& value, const char* field)
            {
                if (value.find_first_of("\n\r") == std::string::npos)
                    return false;
                SPARK_LOG_WARN(Spark::LogCategory::Save, "%s: metadata field '%s' contains an embedded newline",
                               operation, field);
                return true;
            };

            if (rejectNewline(metadata.saveName, "saveName") || rejectNewline(metadata.sceneName, "sceneName") ||
                rejectNewline(metadata.playerClass, "playerClass") ||
                rejectNewline(metadata.screenshotPath, "screenshotPath"))
            {
                return false;
            }
            if (!std::isfinite(metadata.playTime) || !std::isfinite(metadata.playerHealth) ||
                !std::isfinite(metadata.playerArmor) || !std::isfinite(metadata.playerPosition.x) ||
                !std::isfinite(metadata.playerPosition.y) || !std::isfinite(metadata.playerPosition.z))
            {
                SPARK_LOG_WARN(Spark::LogCategory::Save, "%s: metadata contains a non-finite numeric value", operation);
                return false;
            }

            std::ostringstream stream;
            stream << metadata.saveName << "\n";
            stream << metadata.sceneName << "\n";
            stream << metadata.playerClass << "\n";
            stream << metadata.screenshotPath << "\n";
            stream << metadata.timestamp << "\n";
            stream << metadata.playTime << "\n";
            stream << metadata.playerHealth << "\n";
            stream << metadata.playerArmor << "\n";
            stream << metadata.playerPosition.x << " " << metadata.playerPosition.y << " " << metadata.playerPosition.z
                   << "\n";
            stream << metadata.playerKills << "\n";
            stream << metadata.playerDeaths << "\n";
            outBlock = stream.str();
            return true;
        }

        bool AddLengthPrefixedString(SaveRepresentationBudget& budget, const std::string& value, const char* field,
                                     const char* operation)
        {
            if (!SaveRepresentationLimits::SupportsStringBytes(value.size()))
            {
                SPARK_LOG_WARN(Spark::LogCategory::Save, "%s: %s length %zu exceeds the uint16 wire limit %zu",
                               operation, field, value.size(), SaveRepresentationLimits::maxStringBytes);
                return false;
            }
            return budget.AddWireBytes(sizeof(uint16_t)) && budget.AddWireBytes(value.size());
        }

        bool ValidateSaveRepresentation(const SaveData& data, size_t metadataWireBytes, const char* operation)
        {
            if (!SaveRepresentationLimits::SupportsMetadataBytes(metadataWireBytes))
            {
                SPARK_LOG_WARN(Spark::LogCategory::Save, "%s: metadata block %zu exceeds limit %zu", operation,
                               metadataWireBytes, SaveRepresentationLimits::maxMetadataBytes);
                return false;
            }
            if (!SaveRepresentationLimits::SupportsEntityCount(data.entities.size()))
            {
                SPARK_LOG_WARN(Spark::LogCategory::Save, "%s: entity count %zu exceeds limit %zu", operation,
                               data.entities.size(), SaveRepresentationLimits::maxEntities);
                return false;
            }
            if (!SaveRepresentationLimits::SupportsCustomStateCount(data.customState.size()))
            {
                SPARK_LOG_WARN(Spark::LogCategory::Save, "%s: custom-state count %zu exceeds limit %zu", operation,
                               data.customState.size(), SaveRepresentationLimits::maxCustomStateEntries);
                return false;
            }

            SaveRepresentationBudget budget;
            const size_t fixedHeaderBytes =
                4u + sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t) +
                (data.metadata.version >= kChecksummedSaveVersion ? kSaveChecksumBytes : 0u);
            if (!budget.AddWireBytes(fixedHeaderBytes) || !budget.AddWireBytes(metadataWireBytes) ||
                !budget.AddCustomStateEntries(data.customState.size()))
            {
                SPARK_LOG_WARN(Spark::LogCategory::Save, "%s: save representation exceeds aggregate limits", operation);
                return false;
            }

            for (const auto& entity : data.entities)
            {
                if (!SaveRepresentationLimits::SupportsComponentCount(entity.components.size()))
                {
                    SPARK_LOG_WARN(Spark::LogCategory::Save, "%s: per-entity component count %zu exceeds limit %zu",
                                   operation, entity.components.size(),
                                   SaveRepresentationLimits::maxComponentsPerEntity);
                    return false;
                }
                if (!AddLengthPrefixedString(budget, entity.name, "entity.name", operation) ||
                    !budget.AddWireBytes(sizeof(uint16_t)) || !budget.AddComponents(entity.components.size()))
                {
                    SPARK_LOG_WARN(Spark::LogCategory::Save, "%s: entity records exceed aggregate limits", operation);
                    return false;
                }

                for (const auto& component : entity.components)
                {
                    if (!SaveRepresentationLimits::SupportsPropertyCount(component.properties.size()))
                    {
                        SPARK_LOG_WARN(Spark::LogCategory::Save,
                                       "%s: per-component property count %zu exceeds limit %zu", operation,
                                       component.properties.size(),
                                       SaveRepresentationLimits::maxPropertiesPerComponent);
                        return false;
                    }
                    if (!AddLengthPrefixedString(budget, component.typeName, "component.typeName", operation) ||
                        !budget.AddWireBytes(sizeof(uint16_t)) || !budget.AddProperties(component.properties.size()))
                    {
                        SPARK_LOG_WARN(Spark::LogCategory::Save, "%s: component records exceed aggregate limits",
                                       operation);
                        return false;
                    }
                    for (const auto& [key, value] : component.properties)
                    {
                        if (!AddLengthPrefixedString(budget, key, "property.key", operation) ||
                            !AddLengthPrefixedString(budget, value, "property.value", operation))
                        {
                            return false;
                        }
                    }
                }
            }

            for (const auto& [key, value] : data.customState)
            {
                if (!AddLengthPrefixedString(budget, key, "customState.key", operation) ||
                    !AddLengthPrefixedString(budget, value, "customState.value", operation))
                {
                    return false;
                }
            }
            return true;
        }

        bool ValidateSerializedWorldStructure(const SaveData& data, const char* operation)
        {
            for (size_t entityIndex = 0; entityIndex < data.entities.size(); ++entityIndex)
            {
                const auto& serializedEntity = data.entities[entityIndex];
                std::unordered_set<std::string> componentTypes;
                componentTypes.reserve(serializedEntity.components.size());
                for (const auto& component : serializedEntity.components)
                {
                    // NameComponent has one canonical representation on the wire:
                    // SerializedEntity::name. Accepting an explicit component would
                    // add it twice when CreateEntity(name) materializes the candidate.
                    if (component.typeName == "NameComponent")
                    {
                        SPARK_LOG_WARN(Spark::LogCategory::Save,
                                       "%s: entity %zu contains an explicit NameComponent record; names must use "
                                       "SerializedEntity::name",
                                       operation, entityIndex);
                        return false;
                    }
                    if (!componentTypes.insert(component.typeName).second)
                    {
                        SPARK_LOG_WARN(Spark::LogCategory::Save,
                                       "%s: entity %zu contains duplicate component type '%s'", operation, entityIndex,
                                       component.typeName.c_str());
                        return false;
                    }
                }
            }

            // Hierarchy edges travel as the Transform record's parent property, encoded
            // as an index into data.entities. A parent must be a distinct saved entity
            // that itself carries a Transform, otherwise the edge cannot be rebuilt.
            std::vector<const SerializedComponent*> transforms;
            transforms.reserve(data.entities.size());
            for (const SerializedEntity& entity : data.entities)
                transforms.push_back(FindTransformRecord(entity));

            for (size_t entityIndex = 0; entityIndex < transforms.size(); ++entityIndex)
            {
                if (!transforms[entityIndex])
                    continue;

                long long parentIndex = -1;
                if (!ParseTransformParentIndex(*transforms[entityIndex], parentIndex))
                {
                    SPARK_LOG_WARN(Spark::LogCategory::Save, "%s: entity %zu has a malformed Transform parent index",
                                   operation, entityIndex);
                    return false;
                }
                if (parentIndex < 0)
                    continue;

                const auto parent = static_cast<size_t>(parentIndex);
                if (parent >= transforms.size() || parent == entityIndex || !transforms[parent])
                {
                    SPARK_LOG_WARN(Spark::LogCategory::Save,
                                   "%s: entity %zu references parent index %lld, which is not a distinct saved "
                                   "entity with a Transform",
                                   operation, entityIndex, parentIndex);
                    return false;
                }
            }
            return true;
        }

        /// ReadFromFile's parse, without the exception boundary DecodeSaveFileBytes adds.
        bool DecodeSaveFileBytesUnguarded(const std::vector<uint8_t>& fileData, const std::string& filepath,
                                          SaveData& outData)
        {
            // Parse transactionally so a malformed file never leaves callers
            // with a partially populated SaveData object.
            SaveData parsedData;

            uint32_t version = 0;
            size_t payloadEnd = 0;
            if (!ValidateSaveEnvelope(fileData, filepath, "ReadFromFile", version, payloadEnd))
                return false;

            // Parse from the byte buffer using an offset cursor
            size_t offset = 8u;
            SaveRepresentationBudget parsedBudget;

            auto readBytes = [&](void* dest, size_t count) -> bool
            {
                if (offset > payloadEnd || count > payloadEnd - offset)
                    return false;
                std::memcpy(dest, fileData.data() + offset, count);
                offset += count;
                return true;
            };
            auto readUint16 = [&](uint16_t& value) -> bool
            {
                std::array<uint8_t, 2> encoded{};
                if (!readBytes(encoded.data(), encoded.size()))
                    return false;
                value = DecodeLittleEndian16(encoded.data());
                return true;
            };
            auto readUint32 = [&](uint32_t& value) -> bool
            {
                std::array<uint8_t, 4> encoded{};
                if (!readBytes(encoded.data(), encoded.size()))
                    return false;
                value = DecodeLittleEndian32(encoded.data());
                return true;
            };
            parsedData.metadata.version = version;

            // Read metadata
            uint32_t metaSize;
            if (!readUint32(metaSize))
                return false;
            if (!SaveRepresentationLimits::SupportsMetadataBytes(metaSize) || offset > payloadEnd ||
                metaSize > payloadEnd - offset)
                return false;
            std::string metaStr(reinterpret_cast<const char*>(fileData.data() + offset), metaSize);
            offset += metaSize;

            if (!ParseMetadataBlock(version, metaStr, parsedData.metadata))
            {
                SPARK_LOG_WARN(Spark::LogCategory::Save, "ReadFromFile: save '%s' has malformed version %u metadata",
                               filepath.c_str(), version);
                return false;
            }

            // Read entities
            uint32_t entityCount;
            if (!readUint32(entityCount))
                return false;

            // Sanity cap: prevent malformed files from causing huge allocations.
            if (!SaveRepresentationLimits::SupportsEntityCount(entityCount))
            {
                SPARK_LOG_WARN(Spark::LogCategory::Core, "Save file entity count %u exceeds limit %zu", entityCount,
                               SaveRepresentationLimits::maxEntities);
                return false;
            }

            for (uint32_t i = 0; i < entityCount; ++i)
            {
                SerializedEntity entity{};

                uint16_t nameLen;
                if (!readUint16(nameLen))
                    return false;
                // No tighter local cap: the uint16 prefix is the shared representation
                // boundary, so disk and in-memory inputs accept the same maximum name.
                entity.name.resize(nameLen);
                if (!readBytes(entity.name.data(), nameLen))
                    return false;

                uint16_t compCount;
                if (!readUint16(compCount))
                    return false;
                if (!parsedBudget.AddComponents(compCount))
                {
                    SPARK_LOG_WARN(Spark::LogCategory::Save,
                                   "ReadFromFile: aggregate component count exceeds limit %zu",
                                   SaveRepresentationLimits::maxTotalComponents);
                    return false;
                }

                std::unordered_set<std::string> componentTypes;
                componentTypes.reserve(compCount);

                for (uint16_t c = 0; c < compCount; ++c)
                {
                    SerializedComponent comp;

                    uint16_t typeLen;
                    if (!readUint16(typeLen))
                        return false;
                    comp.typeName.resize(typeLen);
                    if (!readBytes(comp.typeName.data(), typeLen))
                        return false;
                    if (comp.typeName == "NameComponent")
                    {
                        SPARK_LOG_WARN(Spark::LogCategory::Save,
                                       "ReadFromFile: entity %u contains an explicit NameComponent record", i);
                        return false;
                    }
                    if (!componentTypes.insert(comp.typeName).second)
                    {
                        SPARK_LOG_WARN(Spark::LogCategory::Save,
                                       "ReadFromFile: entity %u contains duplicate component type '%s'", i,
                                       comp.typeName.c_str());
                        return false;
                    }

                    uint16_t propCount;
                    if (!readUint16(propCount))
                        return false;
                    if (!parsedBudget.AddProperties(propCount))
                    {
                        SPARK_LOG_WARN(Spark::LogCategory::Save,
                                       "ReadFromFile: aggregate property count exceeds limit %zu",
                                       SaveRepresentationLimits::maxTotalProperties);
                        return false;
                    }

                    for (uint16_t p = 0; p < propCount; ++p)
                    {
                        uint16_t keyLen;
                        if (!readUint16(keyLen))
                            return false;
                        std::string key(keyLen, '\0');
                        if (!readBytes(key.data(), keyLen))
                            return false;

                        uint16_t valLen;
                        if (!readUint16(valLen))
                            return false;
                        std::string val(valLen, '\0');
                        if (!readBytes(val.data(), valLen))
                            return false;

                        if (!comp.properties.emplace(std::move(key), std::move(val)).second)
                        {
                            SPARK_LOG_WARN(Spark::LogCategory::Save,
                                           "ReadFromFile: entity %u component %u contains a duplicate property key", i,
                                           static_cast<unsigned>(c));
                            return false;
                        }
                    }

                    entity.components.push_back(std::move(comp));
                }

                parsedData.entities.push_back(std::move(entity));
            }

            // Every supported version ends with a custom-state count, even when zero.
            uint32_t customStateCount = 0;
            if (!readUint32(customStateCount))
                return false;

            if (!SaveRepresentationLimits::SupportsCustomStateCount(customStateCount) ||
                !parsedBudget.AddCustomStateEntries(customStateCount))
                return false;
            for (uint32_t i = 0; i < customStateCount; ++i)
            {
                uint16_t keyLen;
                if (!readUint16(keyLen))
                    return false;
                std::string key(keyLen, '\0');
                if (!readBytes(key.data(), keyLen))
                    return false;

                uint16_t valLen;
                if (!readUint16(valLen))
                    return false;
                std::string val(valLen, '\0');
                if (!readBytes(val.data(), valLen))
                    return false;

                if (!parsedData.customState.emplace(std::move(key), std::move(val)).second)
                {
                    SPARK_LOG_WARN(Spark::LogCategory::Save,
                                   "ReadFromFile: save '%s' contains a duplicate custom-state key", filepath.c_str());
                    return false;
                }
            }

            if (offset != payloadEnd)
                return false;

            if (!ValidateSaveRepresentation(parsedData, metaStr.size(), "ReadFromFile") ||
                !ValidateSerializedWorldStructure(parsedData, "ReadFromFile"))
            {
                return false;
            }

            if (version < kCurrentSaveVersion)
            {
                SPARK_LOG_INFO(Spark::LogCategory::Save, "ReadFromFile: migrating save '%s' from version %u to %u",
                               filepath.c_str(), version, kCurrentSaveVersion);
            }
            if (!MigrateSaveDataToCurrentVersion(parsedData))
                return false;

            outData = std::move(parsedData);
            return true;
        }

        /// ReadMetadataOnly's parse, without the exception boundary DecodeSaveMetadataBytes adds.
        bool DecodeSaveMetadataBytesUnguarded(const std::vector<uint8_t>& fileData, const std::string& filepath,
                                              SaveMetadata& outMetadata)
        {
            uint32_t version = 0;
            size_t payloadEnd = 0;
            if (!ValidateSaveEnvelope(fileData, filepath, "ReadMetadataOnly", version, payloadEnd))
                return false;

            SaveMetadata parsedMetadata;

            // Metadata is a length-prefixed text block immediately after the header.
            size_t offset = 8u;
            if (offset > payloadEnd || sizeof(uint32_t) > payloadEnd - offset)
                return false;
            const uint32_t metaSize = DecodeLittleEndian32(fileData.data() + offset);
            offset += sizeof(uint32_t);
            // Guard against a corrupt/oversized length before allocating.
            if (!SaveRepresentationLimits::SupportsMetadataBytes(metaSize) || offset > payloadEnd ||
                metaSize > payloadEnd - offset)
                return false;

            const std::string metaStr(reinterpret_cast<const char*>(fileData.data() + offset), metaSize);

            if (!ParseMetadataBlock(version, metaStr, parsedMetadata))
                return false;

            SaveData metadataOnly;
            metadataOnly.metadata = std::move(parsedMetadata);
            if (!MigrateSaveDataToCurrentVersion(metadataOnly))
                return false;

            outMetadata = std::move(metadataOnly.metadata);
            return true;
        }
    } // namespace

    bool IsSupportedSaveVersion(uint32_t version) noexcept
    {
        return version >= kOldestSupportedSaveVersion && version <= kCurrentSaveVersion;
    }

    bool IsNewerFormatSaveBytes(const std::vector<uint8_t>& fileData, uint32_t& outVersion) noexcept
    {
        outVersion = 0;
        if (fileData.size() < 8u || std::memcmp(fileData.data(), "SPRK", 4) != 0)
            return false;
        const uint32_t version = DecodeLittleEndian32(fileData.data() + 4);
        if (version <= kCurrentSaveVersion)
            return false;
        if (TrailerVerifiesWithHeaderVersion(fileData, kCurrentSaveVersion))
            return false;

        outVersion = version;
        return true;
    }

    bool MigrateSaveDataToCurrentVersion(SaveData& data)
    {
        if (!IsSupportedSaveVersion(data.metadata.version))
            return false;

        // Work on a copy so future multi-step migrations can retain the same
        // fail-without-mutation contract if any individual step rejects data.
        SaveData migrated = data;
        while (migrated.metadata.version < kCurrentSaveVersion)
        {
            switch (migrated.metadata.version)
            {
            case 3:
                // v4 changes only the disk integrity envelope. The in-memory
                // semantic payload is identical to v3.
                migrated.metadata.version = 4;
                break;
            default:
                return false;
            }
        }

        data = std::move(migrated);
        return true;
    }

    bool ValidateSaveDataShape(const SaveData& data, const char* operation)
    {
        std::string metadataBlock;
        return BuildMetadataBlock(data.metadata, operation, metadataBlock) &&
               ValidateSaveRepresentation(data, metadataBlock.size(), operation) &&
               ValidateSerializedWorldStructure(data, operation);
    }

    const SerializedComponent* FindTransformRecord(const SerializedEntity& entity)
    {
        for (const SerializedComponent& component : entity.components)
        {
            if (component.typeName == "Transform")
                return &component;
        }
        return nullptr;
    }

    bool ParseTransformParentIndex(const SerializedComponent& transform, long long& outIndex)
    {
        outIndex = -1;
        const auto it = transform.properties.find(kTransformParentProperty);
        if (it == transform.properties.end())
            return true;

        const std::string& value = it->second;
        long long parsed = 0;
        const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
        if (error != std::errc{} || end != value.data() + value.size() || parsed < -1)
            return false;
        outIndex = parsed;
        return true;
    }

    bool DecodeSaveFileBytes(const std::vector<uint8_t>& fileData, const std::string& sourceName, SaveData& out)
    {
        try
        {
            return DecodeSaveFileBytesUnguarded(fileData, sourceName, out);
        }
        catch (const std::exception& e)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Core, "Save system error (ReadFromFile): %s", e.what());
            return false;
        }
        catch (...)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Core, "Save system: unknown exception in ReadFromFile");
            return false;
        }
    }

    bool DecodeSaveMetadataBytes(const std::vector<uint8_t>& fileData, const std::string& sourceName, SaveMetadata& out)
    {
        try
        {
            return DecodeSaveMetadataBytesUnguarded(fileData, sourceName, out);
        }
        catch (const std::exception& e)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Core, "Save system error (ReadMetadataOnly): %s", e.what());
            return false;
        }
        catch (...)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Core, "Save system: unknown exception in ReadMetadataOnly");
            return false;
        }
    }

    bool EncodeSaveFileBytes(const SaveData& data, std::string& outBytes)
    {
        if (data.metadata.version != kCurrentSaveVersion)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Save,
                           "WriteToFile: refusing to emit version %u; this build writes version %u only",
                           data.metadata.version, kCurrentSaveVersion);
            return false;
        }

        std::string metaStr;
        if (!BuildMetadataBlock(data.metadata, "WriteToFile", metaStr) ||
            !ValidateSaveRepresentation(data, metaStr.size(), "WriteToFile") ||
            !ValidateSerializedWorldStructure(data, "WriteToFile"))
        {
            return false;
        }

        std::string encoded;
        CRC32 checksum;
        auto writeChecksummed = [&](const void* bytes, size_t count)
        {
            encoded.append(static_cast<const char*>(bytes), count);
            checksum.Update(bytes, count);
        };
        auto writeUint16 = [&](uint16_t value)
        {
            const auto encodedValue = EncodeLittleEndian16(value);
            writeChecksummed(encodedValue.data(), encodedValue.size());
        };
        auto writeUint32 = [&](uint32_t value)
        {
            const auto encodedValue = EncodeLittleEndian32(value);
            writeChecksummed(encodedValue.data(), encodedValue.size());
        };
        // Every length below was bounded by ValidateSaveRepresentation, so the narrowing
        // casts cannot truncate.
        auto writeString = [&](const std::string& value)
        {
            writeUint16(static_cast<uint16_t>(value.size()));
            writeChecksummed(value.data(), value.size());
        };

        // Header
        writeChecksummed("SPRK", 4);
        writeUint32(kCurrentSaveVersion);

        // Metadata block, already validated against the shared disk/in-memory budget.
        writeUint32(static_cast<uint32_t>(metaStr.size()));
        writeChecksummed(metaStr.data(), metaStr.size());

        writeUint32(static_cast<uint32_t>(data.entities.size()));
        for (const auto& entity : data.entities)
        {
            writeString(entity.name);
            writeUint16(static_cast<uint16_t>(entity.components.size()));
            for (const auto& comp : entity.components)
            {
                writeString(comp.typeName);
                writeUint16(static_cast<uint16_t>(comp.properties.size()));
                for (const auto& [key, value] : comp.properties)
                {
                    writeString(key);
                    writeString(value);
                }
            }
        }

        writeUint32(static_cast<uint32_t>(data.customState.size()));
        for (const auto& [key, value] : data.customState)
        {
            writeString(key);
            writeString(value);
        }

        // v4 appends a fixed little-endian CRC32 over every preceding byte,
        // including the magic and version. This detects accidental corruption;
        // it is not an authenticity or anti-tamper mechanism.
        const auto encodedChecksum = EncodeLittleEndian32(checksum.Finalize());
        encoded.append(reinterpret_cast<const char*>(encodedChecksum.data()), encodedChecksum.size());

        outBytes = std::move(encoded);
        return true;
    }

} // namespace Spark
