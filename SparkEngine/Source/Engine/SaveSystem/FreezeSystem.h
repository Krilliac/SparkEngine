/**
 * @file FreezeSystem.h
 * @brief Tag-validated, per-subsystem save state serialization
 *
 * Provides subsystem-level save state registration, tag-based integrity
 * checking, and backward-compatible struct evolution via FreezeLegacy.
 */

#pragma once

#include <cstdint>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace Spark
{

    /**
     * @brief Binary serialization buffer with tag validation markers.
     *
     * Supports Freeze<T>/Thaw<T> for trivially-copyable types,
     * FreezeTag() for checkpoint validation, and FreezeLegacy<T>()
     * for backward-compatible struct evolution.
     */
    class FreezeBuffer
    {
      public:
        enum class Mode
        {
            Write,
            Read
        };

        explicit FreezeBuffer(Mode mode) : m_mode(mode) {}

        FreezeBuffer(Mode mode, std::vector<uint8_t> data) : m_mode(mode), m_data(std::move(data)) {}

        Mode GetMode() const { return m_mode; }

        /**
         * @brief Serialize or deserialize a trivially-copyable type.
         */
        template <typename T> bool Freeze(T& value)
        {
            static_assert(std::is_trivially_copyable_v<T>, "Freeze<T> requires trivially copyable type");
            if (m_mode == Mode::Write)
            {
                const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
                m_data.insert(m_data.end(), bytes, bytes + sizeof(T));
                return true;
            }
            else
            {
                if (m_readPos + sizeof(T) > m_data.size())
                {
                    m_error = true;
                    return false;
                }
                std::memcpy(&value, m_data.data() + m_readPos, sizeof(T));
                m_readPos += sizeof(T);
                return true;
            }
        }

        /**
         * @brief Serialize/deserialize with backward compatibility.
         *
         * When reading, if the saved size is smaller than sizeof(T),
         * zero-fills the remainder (handles struct growth between versions).
         */
        template <typename T> bool FreezeLegacy(T& value, size_t savedSize)
        {
            static_assert(std::is_trivially_copyable_v<T>, "FreezeLegacy<T> requires trivially copyable type");
            if (m_mode == Mode::Write)
            {
                const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
                m_data.insert(m_data.end(), bytes, bytes + sizeof(T));
                return true;
            }
            else
            {
                size_t readAmount = std::min(savedSize, sizeof(T));
                if (m_readPos + savedSize > m_data.size())
                {
                    m_error = true;
                    return false;
                }
                std::memset(&value, 0, sizeof(T));
                std::memcpy(&value, m_data.data() + m_readPos, readAmount);
                m_readPos += savedSize;
                return true;
            }
        }

        /**
         * @brief Insert or validate a named checkpoint tag.
         *
         * On write: stores the tag string. On read: verifies the stored
         * tag matches the expected value. Mismatches indicate save file
         * corruption or serialization order changes.
         */
        bool FreezeTag(const std::string& tag)
        {
            if (m_mode == Mode::Write)
            {
                uint32_t len = static_cast<uint32_t>(tag.size());
                Freeze(len);
                const auto* bytes = reinterpret_cast<const uint8_t*>(tag.data());
                m_data.insert(m_data.end(), bytes, bytes + len);
                return true;
            }
            else
            {
                uint32_t len = 0;
                if (!Freeze(len))
                    return false;
                if (m_readPos + len > m_data.size())
                {
                    m_error = true;
                    return false;
                }
                std::string stored(reinterpret_cast<const char*>(m_data.data() + m_readPos), len);
                m_readPos += len;
                if (stored != tag)
                {
                    m_tagMismatch = true;
                    m_mismatchExpected = tag;
                    m_mismatchFound = stored;
                    return false;
                }
                return true;
            }
        }

        /**
         * @brief Freeze a std::string (length-prefixed).
         */
        bool FreezeString(std::string& value)
        {
            uint32_t len = static_cast<uint32_t>(value.size());
            if (!Freeze(len))
                return false;
            if (m_mode == Mode::Write)
            {
                const auto* bytes = reinterpret_cast<const uint8_t*>(value.data());
                m_data.insert(m_data.end(), bytes, bytes + len);
            }
            else
            {
                if (m_readPos + len > m_data.size())
                {
                    m_error = true;
                    return false;
                }
                value.assign(reinterpret_cast<const char*>(m_data.data() + m_readPos), len);
                m_readPos += len;
            }
            return true;
        }

        bool HasError() const { return m_error; }
        bool HasTagMismatch() const { return m_tagMismatch; }
        const std::string& GetMismatchExpected() const { return m_mismatchExpected; }
        const std::string& GetMismatchFound() const { return m_mismatchFound; }
        const std::vector<uint8_t>& GetData() const { return m_data; }
        size_t GetReadPosition() const { return m_readPos; }
        size_t GetSize() const { return m_data.size(); }

      private:
        Mode m_mode;
        std::vector<uint8_t> m_data;
        size_t m_readPos = 0;
        bool m_error = false;
        bool m_tagMismatch = false;
        std::string m_mismatchExpected;
        std::string m_mismatchFound;
    };

    /**
     * @brief Interface for subsystems that participate in save state serialization.
     */
    class IFreezable
    {
      public:
        virtual ~IFreezable() = default;

        /** @brief Unique tag name for this subsystem (e.g. "Physics", "Audio"). */
        virtual const char* GetFreezeTag() const = 0;

        /** @brief Serialize or deserialize subsystem state to/from the buffer. */
        virtual bool Freeze(FreezeBuffer& buffer) = 0;
    };

    /**
     * @brief Registry and orchestrator for per-subsystem freeze/thaw operations.
     *
     * Each subsystem registers itself via RegisterSubsystem(). FreezeAll()
     * serializes every subsystem in registration order with tag validation
     * markers between each. ThawAll() restores in the same order and catches
     * misalignment via tag checks.
     */
    class FreezeSystem
    {
      public:
        static FreezeSystem& GetInstance()
        {
            static FreezeSystem instance;
            return instance;
        }

        void Initialize() { m_initialized = true; }

        void Shutdown()
        {
            m_subsystems.clear();
            m_initialized = false;
        }

        /**
         * @brief Register a subsystem for save state participation.
         *
         * @param subsystem Non-owning pointer to a subsystem implementing IFreezable.
         *                  Must remain valid for the lifetime of the FreezeSystem.
         */
        void RegisterSubsystem(IFreezable* subsystem)
        {
            if (subsystem)
                m_subsystems.push_back(subsystem);
        }

        /**
         * @brief Serialize all registered subsystems into a binary buffer.
         *
         * Inserts a version header and tag markers between each subsystem
         * for deserialization validation.
         */
        std::vector<uint8_t> FreezeAll()
        {
            FreezeBuffer buffer(FreezeBuffer::Mode::Write);

            uint32_t version = m_saveVersion;
            buffer.Freeze(version);

            uint32_t count = static_cast<uint32_t>(m_subsystems.size());
            buffer.Freeze(count);

            for (auto* sub : m_subsystems)
            {
                buffer.FreezeTag(sub->GetFreezeTag());
                sub->Freeze(buffer);
            }

            buffer.FreezeTag("END");
            return buffer.GetData();
        }

        /**
         * @brief Restore all registered subsystems from a binary buffer.
         *
         * @return true if all subsystems thawed successfully with valid tags.
         */
        bool ThawAll(const std::vector<uint8_t>& data)
        {
            FreezeBuffer buffer(FreezeBuffer::Mode::Read, data);

            uint32_t version = 0;
            buffer.Freeze(version);
            if (version != m_saveVersion)
                return false;

            uint32_t count = 0;
            buffer.Freeze(count);

            if (count != static_cast<uint32_t>(m_subsystems.size()))
                return false;

            for (auto* sub : m_subsystems)
            {
                if (!buffer.FreezeTag(sub->GetFreezeTag()))
                    return false;
                if (!sub->Freeze(buffer))
                    return false;
            }

            return buffer.FreezeTag("END") && !buffer.HasError();
        }

        uint32_t GetSaveVersion() const { return m_saveVersion; }
        void SetSaveVersion(uint32_t version) { m_saveVersion = version; }
        size_t GetSubsystemCount() const { return m_subsystems.size(); }
        bool IsInitialized() const { return m_initialized; }

      private:
        FreezeSystem() = default;

        std::vector<IFreezable*> m_subsystems;
        uint32_t m_saveVersion = 1;
        bool m_initialized = false;
    };

} // namespace Spark
