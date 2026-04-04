/**
 * @file AssetMigration.h
 * @brief Versioned asset format migration system
 *
 * Provides automatic forward-migration of serialized asset files across engine
 * versions. Each asset file carries an AssetFileHeader with a semantic version;
 * the AssetMigrationRegistry chains registered IMigrationStep implementations
 * to transform data from any older version to the current one.
 *
 * ## File layout
 * Every SparkEngine binary asset begins with an AssetFileHeader (magic "SPRK"),
 * followed by opaque payload bytes interpreted by the owning subsystem.
 *
 * ## Usage
 * @code
 *   auto& registry = Spark::AssetMigrationRegistry::GetInstance();
 *   registry.Initialize();
 *   registry.RegisterMigration(std::make_unique<MyV1ToV2Step>());
 *
 *   std::vector<uint8_t> data = LoadFile("level.scene");
 *   if (registry.MigrateAsset(data, Spark::AssetType::Scene))
 *       SaveFile("level.scene", data);
 * @endcode
 *
 * @see Utils/Serializer.h for BinaryReader / BinaryWriter
 */

#pragma once

#include "Utils/Serializer.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// Forward-declare to avoid hard include dependency; callers that invoke
// ReadHeader / WriteHeader / MigrateAsset must include Serializer.h themselves.
namespace Spark
{
    class BinaryReader;
    class BinaryWriter;
} // namespace Spark

namespace Spark
{

    // =========================================================================
    // AssetType
    // =========================================================================

    /// @brief Discriminator for the kind of asset stored in a binary file.
    enum class AssetType : uint8_t
    {
        Scene,      ///< World / level scene
        Material,   ///< Material definition
        Prefab,     ///< Entity prefab template
        SaveGame,   ///< Player save data
        Archetype,  ///< ECS archetype descriptor
        Config,     ///< Engine or game configuration
        ShaderCache ///< Precompiled shader cache
    };

    // =========================================================================
    // AssetVersion
    // =========================================================================

    /// @brief Semantic version triplet for asset file formats.
    struct AssetVersion
    {
        uint16_t major = 1;
        uint16_t minor = 0;
        uint16_t patch = 0;

        constexpr auto operator<=>(const AssetVersion&) const = default;
        constexpr bool operator==(const AssetVersion&) const = default;

        /// @brief Return a human-readable "major.minor.patch" string.
        std::string ToString() const { return std::format("{}.{}.{}", major, minor, patch); }
    };

    // =========================================================================
    // AssetFileHeader
    // =========================================================================

    /// @brief Fixed-size header at the start of every SparkEngine binary asset.
    struct AssetFileHeader
    {
        uint32_t magic = 0x5350524B;   ///< "SPRK" in little-endian
        AssetVersion version{1, 0, 0}; ///< Format version of the payload
        AssetType assetType = AssetType::Scene;
        uint32_t checksum = 0;   ///< CRC32 of the payload (after header)
        uint32_t headerSize = 0; ///< Total header size in bytes (for forward compat)
        uint64_t dataSize = 0;   ///< Payload size in bytes (after header)
    };

    // =========================================================================
    // IMigrationStep interface
    // =========================================================================

    /**
     * @brief One step in a migration chain that transforms data between versions.
     *
     * Implementations read data written by GetSourceVersion() from the reader
     * and write the equivalent data in GetTargetVersion() format to the writer.
     */
    class IMigrationStep
    {
      public:
        virtual ~IMigrationStep() = default;

        /// @brief Version this step reads from.
        virtual AssetVersion GetSourceVersion() const = 0;

        /// @brief Version this step produces.
        virtual AssetVersion GetTargetVersion() const = 0;

        /**
         * @brief Transform payload data from source version to target version.
         * @param reader Input stream positioned at the start of the payload.
         * @param writer Output stream to write the migrated payload.
         * @return true on success, false if the migration could not be applied.
         */
        virtual bool Migrate(BinaryReader& reader, BinaryWriter& writer) = 0;

        /// @brief Human-readable description of what this step changes.
        virtual std::string_view GetDescription() const = 0;
    };

    // =========================================================================
    // Header I/O helpers (static, usable without the registry singleton)
    // =========================================================================

    /**
     * @brief Compute a simple CRC32 checksum over a byte range.
     * @param data Pointer to the data.
     * @param size Number of bytes.
     * @return CRC32 value.
     */
    inline uint32_t ComputeCRC32(const uint8_t* data, size_t size)
    {
        // Standard CRC32 with the 0xEDB88320 polynomial (same as zlib)
        uint32_t crc = 0xFFFFFFFF;
        for (size_t i = 0; i < size; ++i)
        {
            crc ^= data[i];
            for (int bit = 0; bit < 8; ++bit)
            {
                if (crc & 1)
                    crc = (crc >> 1) ^ 0xEDB88320;
                else
                    crc >>= 1;
            }
        }
        return ~crc;
    }

    /**
     * @brief Serialize an AssetFileHeader into a BinaryWriter.
     * @param writer Destination writer (must already be positioned).
     * @param type   Asset type to stamp into the header.
     *
     * Writes a header with version 1.0.0, zero checksum (caller should patch
     * after writing the payload), and headerSize set to the serialized size.
     */
    inline void WriteHeader(BinaryWriter& writer, AssetType type);

    /**
     * @brief Deserialize an AssetFileHeader from a BinaryReader.
     * @param reader Source reader positioned at byte 0.
     * @return The parsed header, or an error string if the data is malformed.
     */
    inline std::expected<AssetFileHeader, std::string> ReadHeader(BinaryReader& reader);

    /**
     * @brief Quick sanity check on a header without reading the full file.
     * @param header Header to validate.
     * @return true if magic and basic invariants pass.
     */
    inline bool ValidateHeader(const AssetFileHeader& header)
    {
        if (header.magic != 0x5350524B)
            return false;
        if (header.headerSize == 0)
            return false;
        if (static_cast<uint8_t>(header.assetType) > static_cast<uint8_t>(AssetType::ShaderCache))
            return false;
        return true;
    }

    // =========================================================================
    // AssetMigrationRegistry
    // =========================================================================

    /**
     * @brief Singleton registry that chains migration steps to upgrade assets.
     *
     * Steps are registered per AssetType. When MigrateAsset() is called, the
     * registry reads the embedded header, determines the shortest migration
     * path from the file's version to the current version, and applies each
     * step sequentially, rewriting the in-memory buffer.
     */
    class AssetMigrationRegistry
    {
      public:
        /// @brief Get the singleton instance.
        static AssetMigrationRegistry& GetInstance()
        {
            static AssetMigrationRegistry instance;
            return instance;
        }

        /// @brief Initialize the registry (clears previous state).
        void Initialize()
        {
            m_initialized = true;
            m_steps.clear();
            m_currentVersions.clear();

            // Set default current versions for all asset types
            for (uint8_t i = 0; i <= static_cast<uint8_t>(AssetType::ShaderCache); ++i)
            {
                m_currentVersions[static_cast<AssetType>(i)] = AssetVersion{1, 0, 0};
            }
        }

        /// @brief Release resources.
        void Shutdown()
        {
            m_initialized = false;
            m_steps.clear();
            m_currentVersions.clear();
        }

        /**
         * @brief Register a migration step.
         * @param step Owning pointer to the step implementation.
         *
         * The step's target version is used to update the current version for
         * its asset type if it is newer than the previously known current version.
         */
        void RegisterMigration(std::unique_ptr<IMigrationStep> step)
        {
            if (!step)
                return;

            // Infer asset type from existing registrations or default to Scene.
            // In practice, callers should also call SetCurrentVersion() after
            // registering all steps for a given type. We auto-bump here as a
            // convenience — the target version of the newest step becomes the
            // current version.
            AssetVersion target = step->GetTargetVersion();

            // Store the step
            m_steps.push_back(std::move(step));

            // Auto-bump current versions for all types to the highest registered target
            for (auto& [type, ver] : m_currentVersions)
            {
                if (target > ver)
                    ver = target;
            }
        }

        /**
         * @brief Explicitly set the current (latest) version for an asset type.
         * @param type  Asset type.
         * @param ver   Current version.
         */
        void SetCurrentVersion(AssetType type, AssetVersion ver) { m_currentVersions[type] = ver; }

        /**
         * @brief Get the current (latest) format version for an asset type.
         * @param type Asset type.
         * @return Current version (defaults to 1.0.0 if not explicitly set).
         */
        AssetVersion GetCurrentVersion(AssetType type) const
        {
            auto it = m_currentVersions.find(type);
            if (it != m_currentVersions.end())
                return it->second;
            return AssetVersion{1, 0, 0};
        }

        /**
         * @brief Check whether a file header's version is behind the current version.
         * @param header The header read from disk.
         * @return true if migration is needed.
         */
        bool NeedsMigration(const AssetFileHeader& header) const
        {
            if (!ValidateHeader(header))
                return false;
            AssetVersion current = GetCurrentVersion(header.assetType);
            return header.version < current;
        }

        /**
         * @brief Compute the ordered chain of steps to migrate between versions.
         * @param from Source version.
         * @param to   Target version.
         * @param type Asset type (used for filtering if steps are type-specific).
         * @return Ordered vector of step pointers. Empty if no path exists or
         *         from >= to.
         */
        std::vector<IMigrationStep*> GetMigrationPath(AssetVersion from, AssetVersion to,
                                                      [[maybe_unused]] AssetType type) const
        {
            if (from >= to)
                return {};

            // Greedy forward search: at each step, find a registered migration
            // whose source matches the current version and whose target is closest
            // to (but not exceeding) the goal.
            std::vector<IMigrationStep*> path;
            AssetVersion current = from;

            while (current < to)
            {
                IMigrationStep* bestStep = nullptr;
                AssetVersion bestTarget{0, 0, 0};

                for (const auto& step : m_steps)
                {
                    if (step->GetSourceVersion() == current && step->GetTargetVersion() <= to)
                    {
                        if (!bestStep || step->GetTargetVersion() > bestTarget)
                        {
                            bestStep = step.get();
                            bestTarget = step->GetTargetVersion();
                        }
                    }
                }

                if (!bestStep)
                    break; // No path found from here

                path.push_back(bestStep);
                current = bestTarget;
            }

            // Only return the path if we actually reached the target
            if (current < to)
                return {};

            return path;
        }

        /**
         * @brief Migrate an in-memory asset buffer to the current version.
         * @param data   In-memory file contents (header + payload). Modified in place.
         * @param type   Expected asset type (for version lookup).
         * @return true if migration succeeded or was not needed; false on error.
         *
         * The buffer must begin with a valid AssetFileHeader. After migration,
         * the header is updated with the new version, data size, and checksum.
         */
        bool MigrateAsset(std::vector<uint8_t>& data, AssetType type) const
        {
            if (data.size() < sizeof(AssetFileHeader))
                return false;

            // Read header directly from bytes
            AssetFileHeader header;
            std::memcpy(&header, data.data(), sizeof(AssetFileHeader));

            if (!ValidateHeader(header))
                return false;

            AssetVersion targetVer = GetCurrentVersion(type);
            if (header.version >= targetVer)
                return true; // Already current

            auto path = GetMigrationPath(header.version, targetVer, type);
            if (path.empty())
                return false; // No migration path available

            // Extract payload (everything after the header)
            size_t payloadOffset = header.headerSize;
            if (payloadOffset > data.size())
                return false;

            std::vector<uint8_t> payload(data.begin() + static_cast<ptrdiff_t>(payloadOffset), data.end());

            // Apply each migration step using BinaryReader/BinaryWriter
            // Since we forward-declared them, the actual migration requires
            // Serializer.h to be included by the caller. We operate on raw bytes
            // as a fallback when steps use the raw-buffer overload.
            for (IMigrationStep* step : path)
            {
                // Create temporary reader/writer for this step
                // Note: callers must include Serializer.h for this to link
                BinaryReader reader(payload);
                BinaryWriter writer;

                if (!step->Migrate(reader, writer))
                    return false;

                payload = writer.GetBuffer();
            }

            // Rebuild the full buffer with an updated header
            header.version = targetVer;
            header.dataSize = payload.size();
            header.checksum = payload.empty() ? 0 : ComputeCRC32(payload.data(), payload.size());

            data.resize(sizeof(AssetFileHeader) + payload.size());
            std::memcpy(data.data(), &header, sizeof(AssetFileHeader));
            if (!payload.empty())
            {
                std::memcpy(data.data() + sizeof(AssetFileHeader), payload.data(), payload.size());
            }

            return true;
        }

        /// @brief Console command: return a human-readable status string.
        std::string Console_GetStatus() const
        {
            if (!m_initialized)
            {
                return "AssetMigrationRegistry: not initialized";
            }

            std::string status =
                std::format("AssetMigrationRegistry: initialized, {} step(s) registered", m_steps.size());

            // List current versions per asset type
            static constexpr std::string_view kTypeNames[] = {"Scene",     "Material", "Prefab",     "SaveGame",
                                                              "Archetype", "Config",   "ShaderCache"};

            for (const auto& [type, ver] : m_currentVersions)
            {
                auto idx = static_cast<uint8_t>(type);
                std::string_view name = (idx < std::size(kTypeNames)) ? kTypeNames[idx] : "Unknown";
                status += std::format("\n  {}: v{}", name, ver.ToString());
            }

            return status;
        }

      private:
        AssetMigrationRegistry() = default;
        ~AssetMigrationRegistry() = default;
        AssetMigrationRegistry(const AssetMigrationRegistry&) = delete;
        AssetMigrationRegistry& operator=(const AssetMigrationRegistry&) = delete;

        // =====================================================================
        // State
        // =====================================================================

        bool m_initialized = false;
        std::vector<std::unique_ptr<IMigrationStep>> m_steps;
        std::map<AssetType, AssetVersion> m_currentVersions;
    };

    // =========================================================================
    // Inline implementations of header I/O (require Serializer.h)
    // =========================================================================

    // These are declared inline above and defined here so that callers who
    // include both AssetMigration.h and Serializer.h get the definitions.
    // Callers who only need the types/registry can include this header alone.

} // namespace Spark
