/**
 * @file SecureMemory.h
 * @brief Small, portable helpers for promptly erasing live credential buffers.
 */
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Spark
{
    /**
     * Erase a live byte range through volatile-qualified stores.
     *
     * Unlike memset, these stores are observable side effects and therefore
     * cannot be removed merely because the object is not read again. The
     * caller must still own a valid writable range for the duration of the call.
     */
    inline void SecureErase(void* data, size_t size) noexcept
    {
        auto* bytes = static_cast<volatile unsigned char*>(data);
        while (bytes && size > 0)
        {
            *bytes++ = 0;
            --size;
        }
        std::atomic_signal_fence(std::memory_order_seq_cst);
    }

    inline void SecureClear(std::string& value) noexcept
    {
        if (!value.empty())
            SecureErase(value.data(), value.size());
        value.clear();
    }

    inline void SecureClear(std::vector<std::string>& values) noexcept
    {
        for (auto& value : values)
            SecureClear(value);
        values.clear();
    }

    /** Fixed-capacity, non-copyable text storage intended for UI credentials. */
    template <size_t Capacity> class SensitiveCharBuffer
    {
        static_assert(Capacity > 0, "A sensitive buffer must have storage");

      public:
        SensitiveCharBuffer() = default;
        ~SensitiveCharBuffer() { Clear(); }

        SensitiveCharBuffer(const SensitiveCharBuffer&) = delete;
        SensitiveCharBuffer& operator=(const SensitiveCharBuffer&) = delete;
        SensitiveCharBuffer(SensitiveCharBuffer&&) = delete;
        SensitiveCharBuffer& operator=(SensitiveCharBuffer&&) = delete;

        [[nodiscard]] char* data() noexcept { return m_storage.data(); }
        [[nodiscard]] const char* data() const noexcept { return m_storage.data(); }
        [[nodiscard]] static constexpr size_t capacity() noexcept { return Capacity; }

        [[nodiscard]] std::string_view View() const noexcept
        {
            size_t length = 0;
            while (length < Capacity && m_storage[length] != '\0')
                ++length;
            return {m_storage.data(), length};
        }

        void Clear() noexcept { SecureErase(m_storage.data(), m_storage.size()); }

        [[nodiscard]] bool IsCleared() const noexcept
        {
            for (const char value : m_storage)
            {
                if (value != '\0')
                    return false;
            }
            return true;
        }

        class ClearOnExit
        {
          public:
            explicit ClearOnExit(SensitiveCharBuffer& owner) noexcept : m_owner(&owner) {}
            ~ClearOnExit()
            {
                if (m_owner)
                    m_owner->Clear();
            }

            ClearOnExit(const ClearOnExit&) = delete;
            ClearOnExit& operator=(const ClearOnExit&) = delete;
            ClearOnExit(ClearOnExit&& other) noexcept : m_owner(std::exchange(other.m_owner, nullptr)) {}
            ClearOnExit& operator=(ClearOnExit&&) = delete;

          private:
            SensitiveCharBuffer* m_owner;
        };

        [[nodiscard]] ClearOnExit ClearOnScopeExit() noexcept { return ClearOnExit(*this); }

      private:
        std::array<char, Capacity> m_storage{};
    };
} // namespace Spark
