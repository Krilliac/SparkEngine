/**
 * @file DatablockRegistry.h
 * @brief Immutable shared-data objects sent once at connection time (Torque3D-inspired)
 *
 * Datablocks are immutable definitions (weapon stats, vehicle configs, item templates)
 * that all clients must agree on. Instead of replicating these values every tick per
 * entity, the server sends the full datablock table once when a client connects.
 * Entities then reference datablocks by ID, consuming zero ongoing bandwidth for
 * shared configuration data.
 *
 * ## Usage
 * @code
 *   auto& registry = DatablockRegistry::Get();
 *
 *   // Server registers datablocks at startup
 *   registry.Register("weapon.rifle", {
 *       {"damage", 35.0f}, {"fireRate", 0.1f}, {"magSize", 30}
 *   });
 *
 *   // Serialize all datablocks for a new client
 *   auto buffer = registry.SerializeAll();
 *   // Send buffer to client...
 *
 *   // Client deserializes on connect
 *   registry.DeserializeAll(buffer);
 *
 *   // Runtime lookup by name (fast: string → ID → data)
 *   if (auto* db = registry.Find("weapon.rifle")) {
 *       float dmg = db->GetFloat("damage");
 *   }
 * @endcode
 */

#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace Spark::Net
{

    /// @brief A single named value within a datablock
    using DatablockValue = std::variant<int32_t, float, bool, std::string>;

    /// @brief Unique identifier for a datablock (assigned at registration time)
    using DatablockID = uint32_t;

    constexpr DatablockID INVALID_DATABLOCK = 0;

    /**
     * @brief An immutable set of named properties describing a game object template.
     *
     * Once registered, a datablock's contents do not change at runtime. Entities
     * reference the datablock by ID and inherit its values without replication cost.
     */
    struct Datablock
    {
        DatablockID id = INVALID_DATABLOCK;
        std::string name;
        std::unordered_map<std::string, DatablockValue> properties;

        /// @brief Get a float property. Returns defaultVal if not found or wrong type.
        float GetFloat(const std::string& key, float defaultVal = 0.0f) const
        {
            auto it = properties.find(key);
            if (it == properties.end())
                return defaultVal;
            if (auto* val = std::get_if<float>(&it->second))
                return *val;
            return defaultVal;
        }

        /// @brief Get an int property. Returns defaultVal if not found or wrong type.
        int32_t GetInt(const std::string& key, int32_t defaultVal = 0) const
        {
            auto it = properties.find(key);
            if (it == properties.end())
                return defaultVal;
            if (auto* val = std::get_if<int32_t>(&it->second))
                return *val;
            return defaultVal;
        }

        /// @brief Get a bool property. Returns defaultVal if not found or wrong type.
        bool GetBool(const std::string& key, bool defaultVal = false) const
        {
            auto it = properties.find(key);
            if (it == properties.end())
                return defaultVal;
            if (auto* val = std::get_if<bool>(&it->second))
                return *val;
            return defaultVal;
        }

        /// @brief Get a string property. Returns defaultVal if not found or wrong type.
        std::string GetString(const std::string& key, const std::string& defaultVal = "") const
        {
            auto it = properties.find(key);
            if (it == properties.end())
                return defaultVal;
            if (auto* val = std::get_if<std::string>(&it->second))
                return *val;
            return defaultVal;
        }
    };

    /**
     * @brief Central registry for immutable datablock definitions.
     *
     * Thread safety: Not thread-safe. Register all datablocks during initialization
     * before any network activity. Read-only access (Find/Get) is safe from multiple
     * threads after registration is complete.
     */
    class DatablockRegistry
    {
      public:
        static DatablockRegistry& Get()
        {
            static DatablockRegistry instance;
            return instance;
        }

        /**
         * @brief Register a new datablock with the given name and properties.
         * @param name       Unique name (e.g. "weapon.rifle", "vehicle.tank")
         * @param properties Map of property name → value
         * @return The assigned DatablockID, or INVALID_DATABLOCK if name already exists
         */
        DatablockID Register(const std::string& name, const std::unordered_map<std::string, DatablockValue>& properties)
        {
            if (m_nameToId.count(name) > 0)
                return INVALID_DATABLOCK;

            DatablockID id = m_nextId++;
            Datablock db;
            db.id = id;
            db.name = name;
            db.properties = properties;

            m_datablocks[id] = std::move(db);
            m_nameToId[name] = id;

            return id;
        }

        /// @brief Find a datablock by name. Returns nullptr if not found.
        const Datablock* Find(const std::string& name) const
        {
            auto it = m_nameToId.find(name);
            if (it == m_nameToId.end())
                return nullptr;
            return FindByID(it->second);
        }

        /// @brief Find a datablock by ID. Returns nullptr if not found.
        const Datablock* FindByID(DatablockID id) const
        {
            auto it = m_datablocks.find(id);
            if (it == m_datablocks.end())
                return nullptr;
            return &it->second;
        }

        /// @brief Resolve a name to its DatablockID.
        DatablockID GetID(const std::string& name) const
        {
            auto it = m_nameToId.find(name);
            return (it != m_nameToId.end()) ? it->second : INVALID_DATABLOCK;
        }

        /// @brief Get all registered datablocks.
        std::vector<const Datablock*> GetAll() const
        {
            std::vector<const Datablock*> result;
            result.reserve(m_datablocks.size());
            for (const auto& [id, db] : m_datablocks)
            {
                result.push_back(&db);
            }
            return result;
        }

        /// @brief Number of registered datablocks.
        size_t Count() const { return m_datablocks.size(); }

        /// @brief Remove all registered datablocks.
        void Clear()
        {
            m_datablocks.clear();
            m_nameToId.clear();
            m_nextId = 1;
        }

        // -- Serialization (for network transfer) --------------------------------

        /**
         * @brief Serialize all datablocks into a byte buffer for network transfer.
         *
         * Format: [count:u32] { [id:u32] [nameLen:u16] [name] [propCount:u16]
         *         { [keyLen:u16] [key] [typeTag:u8] [value] }... }...
         */
        std::vector<uint8_t> SerializeAll() const
        {
            std::vector<uint8_t> buffer;

            WriteU32(buffer, static_cast<uint32_t>(m_datablocks.size()));

            for (const auto& [id, db] : m_datablocks)
            {
                WriteU32(buffer, db.id);
                WriteString(buffer, db.name);
                WriteU16(buffer, static_cast<uint16_t>(db.properties.size()));

                for (const auto& [key, val] : db.properties)
                {
                    WriteString(buffer, key);
                    SerializeValue(buffer, val);
                }
            }

            return buffer;
        }

        /**
         * @brief Deserialize datablocks from a byte buffer (replaces current contents).
         * @return true if deserialization succeeded
         */
        bool DeserializeAll(const std::vector<uint8_t>& buffer)
        {
            size_t pos = 0;

            uint32_t count = ReadU32(buffer, pos);
            if (pos == 0 && count > 0)
                return false;

            Clear();

            for (uint32_t i = 0; i < count; ++i)
            {
                DatablockID id = ReadU32(buffer, pos);
                std::string name = ReadString(buffer, pos);
                uint16_t propCount = ReadU16(buffer, pos);

                if (pos > buffer.size())
                    return false;

                std::unordered_map<std::string, DatablockValue> props;
                for (uint16_t p = 0; p < propCount; ++p)
                {
                    std::string key = ReadString(buffer, pos);
                    DatablockValue val = DeserializeValue(buffer, pos);
                    if (pos > buffer.size())
                        return false;
                    props[key] = std::move(val);
                }

                Datablock db;
                db.id = id;
                db.name = name;
                db.properties = std::move(props);

                m_datablocks[id] = std::move(db);
                m_nameToId[name] = id;

                if (id >= m_nextId)
                    m_nextId = id + 1;
            }

            return true;
        }

      private:
        DatablockRegistry() = default;

        // -- Wire format helpers -------------------------------------------------

        static void WriteU16(std::vector<uint8_t>& buf, uint16_t val)
        {
            buf.push_back(static_cast<uint8_t>(val & 0xFF));
            buf.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
        }

        static void WriteU32(std::vector<uint8_t>& buf, uint32_t val)
        {
            buf.push_back(static_cast<uint8_t>(val & 0xFF));
            buf.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
            buf.push_back(static_cast<uint8_t>((val >> 16) & 0xFF));
            buf.push_back(static_cast<uint8_t>((val >> 24) & 0xFF));
        }

        static void WriteString(std::vector<uint8_t>& buf, const std::string& str)
        {
            WriteU16(buf, static_cast<uint16_t>(str.size()));
            buf.insert(buf.end(), str.begin(), str.end());
        }

        static void WriteFloat(std::vector<uint8_t>& buf, float val)
        {
            const auto* bytes = reinterpret_cast<const uint8_t*>(&val);
            buf.insert(buf.end(), bytes, bytes + sizeof(float));
        }

        static uint16_t ReadU16(const std::vector<uint8_t>& buf, size_t& pos)
        {
            if (pos + 2 > buf.size())
                return 0;
            uint16_t val = static_cast<uint16_t>(buf[pos]) | (static_cast<uint16_t>(buf[pos + 1]) << 8);
            pos += 2;
            return val;
        }

        static uint32_t ReadU32(const std::vector<uint8_t>& buf, size_t& pos)
        {
            if (pos + 4 > buf.size())
                return 0;
            uint32_t val = static_cast<uint32_t>(buf[pos]) | (static_cast<uint32_t>(buf[pos + 1]) << 8) |
                           (static_cast<uint32_t>(buf[pos + 2]) << 16) | (static_cast<uint32_t>(buf[pos + 3]) << 24);
            pos += 4;
            return val;
        }

        static std::string ReadString(const std::vector<uint8_t>& buf, size_t& pos)
        {
            uint16_t len = ReadU16(buf, pos);
            if (pos + len > buf.size())
                return "";
            std::string str(buf.begin() + static_cast<ptrdiff_t>(pos), buf.begin() + static_cast<ptrdiff_t>(pos + len));
            pos += len;
            return str;
        }

        static float ReadFloat(const std::vector<uint8_t>& buf, size_t& pos)
        {
            if (pos + sizeof(float) > buf.size())
                return 0.0f;
            float val = 0.0f;
            std::memcpy(&val, buf.data() + pos, sizeof(float));
            pos += sizeof(float);
            return val;
        }

        // Type tags for variant serialization
        static constexpr uint8_t TAG_INT = 0;
        static constexpr uint8_t TAG_FLOAT = 1;
        static constexpr uint8_t TAG_BOOL = 2;
        static constexpr uint8_t TAG_STRING = 3;

        static void SerializeValue(std::vector<uint8_t>& buf, const DatablockValue& val)
        {
            if (auto* v = std::get_if<int32_t>(&val))
            {
                buf.push_back(TAG_INT);
                WriteU32(buf, static_cast<uint32_t>(*v));
            }
            else if (auto* v = std::get_if<float>(&val))
            {
                buf.push_back(TAG_FLOAT);
                WriteFloat(buf, *v);
            }
            else if (auto* v = std::get_if<bool>(&val))
            {
                buf.push_back(TAG_BOOL);
                buf.push_back(*v ? 1 : 0);
            }
            else if (auto* v = std::get_if<std::string>(&val))
            {
                buf.push_back(TAG_STRING);
                WriteString(buf, *v);
            }
        }

        static DatablockValue DeserializeValue(const std::vector<uint8_t>& buf, size_t& pos)
        {
            if (pos >= buf.size())
                return int32_t(0);

            uint8_t tag = buf[pos++];
            switch (tag)
            {
            case TAG_INT:
                return static_cast<int32_t>(ReadU32(buf, pos));
            case TAG_FLOAT:
                return ReadFloat(buf, pos);
            case TAG_BOOL:
            {
                if (pos >= buf.size())
                    return false;
                bool val = (buf[pos] != 0);
                pos++;
                return val;
            }
            case TAG_STRING:
                return ReadString(buf, pos);
            default:
                return int32_t(0);
            }
        }

        std::unordered_map<DatablockID, Datablock> m_datablocks;
        std::unordered_map<std::string, DatablockID> m_nameToId;
        DatablockID m_nextId = 1;
    };

} // namespace Spark::Net
