/**
 * @file SecureMemory.h
 * @brief Small, portable helpers for promptly erasing live credential buffers.
 */
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Spark
{
    /**
     * Erase a live byte range so the zeroing cannot be removed as a dead store.
     *
     * GCC and Clang zero the range with one memset followed by an empty asm
     * statement that takes the pointer and clobbers memory: the compiler must
     * assume the asm reads the zeroed bytes, so the memset is never elided even
     * when the object is not read again (the BoringSSL OPENSSL_cleanse pattern).
     * One bulk store also keeps the per-round erasure in the PBKDF2 loops cheap
     * under ASan/TSan, whose interceptors check a memset range once instead of
     * instrumenting every byte. Other compilers use volatile-qualified stores.
     * The caller must still own a valid writable range for the duration of the call.
     */
    inline void SecureErase(void* data, size_t size) noexcept
    {
        if (!data || size == 0)
        {
            return;
        }
#if defined(__GNUC__) || defined(__clang__)
        std::memset(data, 0, size);
        __asm__ __volatile__("" : : "r"(data) : "memory");
#else
        auto* bytes = static_cast<volatile unsigned char*>(data);
        while (size > 0)
        {
            *bytes++ = 0;
            --size;
        }
        std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
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

    inline void SecureClear(std::vector<uint8_t>& value) noexcept
    {
        if (!value.empty())
            SecureErase(value.data(), value.size());
        value.clear();
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
