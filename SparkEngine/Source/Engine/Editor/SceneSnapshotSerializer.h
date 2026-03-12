/**
 * @file SceneSnapshotSerializer.h
 * @brief Binary ECS state serializer for play-in-editor snapshot save/restore
 *
 * Provides complete ECS registry serialization so the editor can capture the
 * entire scene state before entering play mode and restore it exactly when
 * exiting. Every component type known to the engine is handled via a
 * type-erased visitor pattern, ensuring new components only need a single
 * registration call to participate in snapshot round-trips.
 */

#pragma once

#include "../../Core/Platform.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace Spark::Editor
{

    // ========================================================================
    // Binary buffer helpers
    // ========================================================================

    class SnapshotWriter
    {
      public:
        void WriteU32(uint32_t v)
        {
            const auto* p = reinterpret_cast<const uint8_t*>(&v);
            m_data.insert(m_data.end(), p, p + sizeof(v));
        }

        void WriteU64(uint64_t v)
        {
            const auto* p = reinterpret_cast<const uint8_t*>(&v);
            m_data.insert(m_data.end(), p, p + sizeof(v));
        }

        void WriteFloat(float v)
        {
            const auto* p = reinterpret_cast<const uint8_t*>(&v);
            m_data.insert(m_data.end(), p, p + sizeof(v));
        }

        void WriteBool(bool v) { m_data.push_back(v ? 1 : 0); }

        void WriteString(const std::string& s)
        {
            WriteU32(static_cast<uint32_t>(s.size()));
            m_data.insert(m_data.end(), s.begin(), s.end());
        }

        void WriteFloat3(float x, float y, float z)
        {
            WriteFloat(x);
            WriteFloat(y);
            WriteFloat(z);
        }

        void WriteBytes(const void* data, size_t size)
        {
            const auto* p = reinterpret_cast<const uint8_t*>(data);
            m_data.insert(m_data.end(), p, p + size);
        }

        const std::vector<uint8_t>& GetData() const { return m_data; }
        std::vector<uint8_t> TakeData() { return std::move(m_data); }
        size_t Size() const { return m_data.size(); }

      private:
        std::vector<uint8_t> m_data;
    };

    class SnapshotReader
    {
      public:
        explicit SnapshotReader(const std::vector<uint8_t>& data) : m_data(data) {}

        bool HasRemaining(size_t bytes) const { return m_offset + bytes <= m_data.size(); }

        uint32_t ReadU32()
        {
            uint32_t v = 0;
            if (HasRemaining(sizeof(v)))
            {
                std::memcpy(&v, m_data.data() + m_offset, sizeof(v));
                m_offset += sizeof(v);
            }
            return v;
        }

        uint64_t ReadU64()
        {
            uint64_t v = 0;
            if (HasRemaining(sizeof(v)))
            {
                std::memcpy(&v, m_data.data() + m_offset, sizeof(v));
                m_offset += sizeof(v);
            }
            return v;
        }

        float ReadFloat()
        {
            float v = 0.0f;
            if (HasRemaining(sizeof(v)))
            {
                std::memcpy(&v, m_data.data() + m_offset, sizeof(v));
                m_offset += sizeof(v);
            }
            return v;
        }

        bool ReadBool()
        {
            if (HasRemaining(1))
                return m_data[m_offset++] != 0;
            return false;
        }

        std::string ReadString()
        {
            uint32_t len = ReadU32();
            if (!HasRemaining(len))
                return {};
            std::string s(reinterpret_cast<const char*>(m_data.data() + m_offset), len);
            m_offset += len;
            return s;
        }

        void ReadFloat3(float& x, float& y, float& z)
        {
            x = ReadFloat();
            y = ReadFloat();
            z = ReadFloat();
        }

        bool ReadBytes(void* out, size_t size)
        {
            if (!HasRemaining(size))
                return false;
            std::memcpy(out, m_data.data() + m_offset, size);
            m_offset += size;
            return true;
        }

        size_t GetOffset() const { return m_offset; }
        bool IsValid() const { return m_offset <= m_data.size(); }

      private:
        const std::vector<uint8_t>& m_data;
        size_t m_offset = 0;
    };

    // ========================================================================
    // Component serializer registry
    // ========================================================================

    /// @brief Type-erased serialization functions for a single component type.
    struct ComponentSerializerEntry
    {
        std::string typeName;
        uint32_t typeId;

        /// Serialize all instances of this component into the writer.
        /// Arguments: writer, opaque registry pointer
        std::function<uint32_t(SnapshotWriter&, const void*)> serialize;

        /// Deserialize instances from the reader into the registry.
        /// Arguments: reader, opaque registry pointer, entity count
        std::function<bool(SnapshotReader&, void*, uint32_t)> deserialize;
    };

    /**
     * @brief Registry of component serializers
     *
     * Engine subsystems register their component serializers at startup.
     * The snapshot system iterates all registered serializers when saving
     * and restoring.
     */
    class ComponentSerializerRegistry
    {
      public:
        static ComponentSerializerRegistry& Instance()
        {
            static ComponentSerializerRegistry s_instance;
            return s_instance;
        }

        void Register(ComponentSerializerEntry entry) { m_entries[entry.typeId] = std::move(entry); }

        const std::unordered_map<uint32_t, ComponentSerializerEntry>& GetEntries() const { return m_entries; }

        const ComponentSerializerEntry* Find(uint32_t typeId) const
        {
            auto it = m_entries.find(typeId);
            return it != m_entries.end() ? &it->second : nullptr;
        }

        size_t Count() const { return m_entries.size(); }

      private:
        ComponentSerializerRegistry() = default;
        std::unordered_map<uint32_t, ComponentSerializerEntry> m_entries;
    };

    // ========================================================================
    // Scene snapshot serializer
    // ========================================================================

    /// @brief File-format magic and version for snapshot validation.
    static constexpr uint32_t SNAPSHOT_MAGIC = 0x53504B53; // "SPKS"
    static constexpr uint32_t SNAPSHOT_VERSION = 1;

    /**
     * @brief Serializes and deserializes complete ECS scene state
     *
     * The binary format is:
     *   [magic:u32] [version:u32] [entityCount:u32] [componentTypeCount:u32]
     *   For each component type:
     *     [typeId:u32] [typeNameLen:u32] [typeName:bytes]
     *     [instanceCount:u32] [componentData:bytes...]
     *
     * Entity IDs are stored as uint32_t indices. The deserializer creates
     * fresh entities and remaps references during restore.
     */
    class SceneSnapshotSerializer
    {
      public:
        /**
         * @brief Serialize the current ECS state into a binary blob.
         * @param registry  Opaque pointer to the EnTT registry.
         * @param entityCount  Number of entities to serialize.
         * @return Serialized bytes, or empty on failure.
         */
        static std::vector<uint8_t> Serialize(const void* registry, uint32_t entityCount)
        {
            SnapshotWriter writer;

            // Header
            writer.WriteU32(SNAPSHOT_MAGIC);
            writer.WriteU32(SNAPSHOT_VERSION);
            writer.WriteU32(entityCount);

            const auto& entries = ComponentSerializerRegistry::Instance().GetEntries();
            writer.WriteU32(static_cast<uint32_t>(entries.size()));

            // Serialize each registered component type
            for (const auto& [typeId, entry] : entries)
            {
                writer.WriteU32(typeId);
                writer.WriteString(entry.typeName);

                // Let the component serializer write its data
                uint32_t instanceCount = 0;
                if (entry.serialize)
                {
                    instanceCount = entry.serialize(writer, registry);
                }
                // instanceCount is written inside the serializer
            }

            return writer.TakeData();
        }

        /**
         * @brief Deserialize binary data back into the ECS registry.
         * @param data  Binary snapshot data.
         * @param registry  Opaque pointer to the EnTT registry.
         * @return True if deserialization succeeded.
         */
        static bool Deserialize(const std::vector<uint8_t>& data, void* registry)
        {
            if (data.size() < 16)
                return false;

            SnapshotReader reader(data);

            uint32_t magic = reader.ReadU32();
            if (magic != SNAPSHOT_MAGIC)
                return false;

            uint32_t version = reader.ReadU32();
            if (version != SNAPSHOT_VERSION)
                return false;

            uint32_t entityCount = reader.ReadU32();
            uint32_t componentTypeCount = reader.ReadU32();

            for (uint32_t i = 0; i < componentTypeCount; ++i)
            {
                uint32_t typeId = reader.ReadU32();
                std::string typeName = reader.ReadString();

                const auto* entry = ComponentSerializerRegistry::Instance().Find(typeId);
                if (entry && entry->deserialize)
                {
                    if (!entry->deserialize(reader, registry, entityCount))
                        return false;
                }
            }

            return reader.IsValid();
        }

        /**
         * @brief Validate snapshot data without fully deserializing.
         * @param data  Binary snapshot data.
         * @return True if the header is valid.
         */
        static bool Validate(const std::vector<uint8_t>& data)
        {
            if (data.size() < 16)
                return false;

            SnapshotReader reader(data);
            return reader.ReadU32() == SNAPSHOT_MAGIC && reader.ReadU32() == SNAPSHOT_VERSION;
        }

        /**
         * @brief Get a human-readable summary of the snapshot.
         * @param data  Binary snapshot data.
         * @return Description string.
         */
        static std::string GetSnapshotInfo(const std::vector<uint8_t>& data)
        {
            if (data.size() < 16)
                return "Invalid snapshot";

            SnapshotReader reader(data);
            uint32_t magic = reader.ReadU32();
            uint32_t version = reader.ReadU32();
            uint32_t entityCount = reader.ReadU32();
            uint32_t componentTypeCount = reader.ReadU32();

            if (magic != SNAPSHOT_MAGIC)
                return "Invalid snapshot (bad magic)";

            std::ostringstream oss;
            oss << "Snapshot v" << version << ": " << entityCount << " entities, " << componentTypeCount
                << " component types, " << data.size() << " bytes";
            return oss.str();
        }
    };

} // namespace Spark::Editor
