/**
 * @file Expected.h
 * @brief Feature-test bridge for std::expected (C++23).
 *
 * libc++ < 17 ships <expected> but does not define std::expected. CI runners
 * exercise both libraries, so a check that accepts either stdlib is required.
 *
 * Use Spark::Daemon::Expected<T, E> and Spark::Daemon::Unexpected<E>. They
 * alias std::expected / std::unexpected when available; otherwise a minimal
 * in-line polyfill is used that implements the subset this codebase uses
 * (has_value, operator bool, error(), value(), operator*, operator->).
 */

#pragma once

#include <string>
#include <type_traits>
#include <utility>

#if defined(__has_include)
#if __has_include(<version>)
#include <version>
#endif
#endif

#if defined(__cpp_lib_expected) && __cpp_lib_expected >= 202202L
#include <expected>
namespace Spark::Daemon
{
    template <class T, class E> using Expected = std::expected<T, E>;
    template <class E> using Unexpected = std::unexpected<E>;
} // namespace Spark::Daemon
#else
namespace Spark::Daemon
{
    template <class E> class Unexpected
    {
      public:
        explicit Unexpected(E e) : m_error(std::move(e)) {}
        const E& error() const& { return m_error; }
        E&& error() && { return std::move(m_error); }

      private:
        E m_error;
    };

    // Minimal polyfill — only the operations used in this codebase.
    template <class T, class E> class Expected
    {
      public:
        using value_type = T;
        using error_type = E;

        Expected() : m_hasValue(true), m_value() {}
        Expected(const T& v) : m_hasValue(true), m_value(v) {}
        Expected(T&& v) : m_hasValue(true), m_value(std::move(v)) {}
        Expected(const Unexpected<E>& u) : m_hasValue(false), m_error(u.error()) {}
        Expected(Unexpected<E>&& u) : m_hasValue(false), m_error(std::move(u).error()) {}

        Expected(const Expected&) = default;
        Expected(Expected&&) noexcept = default;
        Expected& operator=(const Expected&) = default;
        Expected& operator=(Expected&&) noexcept = default;

        bool has_value() const noexcept { return m_hasValue; }
        explicit operator bool() const noexcept { return m_hasValue; }

        T& value() & { return m_value; }
        const T& value() const& { return m_value; }
        T&& value() && { return std::move(m_value); }

        T& operator*() & { return m_value; }
        const T& operator*() const& { return m_value; }
        T&& operator*() && { return std::move(m_value); }
        T* operator->() { return &m_value; }
        const T* operator->() const { return &m_value; }

        const E& error() const& { return m_error; }
        E&& error() && { return std::move(m_error); }

      private:
        bool m_hasValue{false};
        T m_value{};
        E m_error{};
    };

    // void specialization — value() / operator* are disallowed.
    template <class E> class Expected<void, E>
    {
      public:
        using value_type = void;
        using error_type = E;

        Expected() : m_hasValue(true) {}
        Expected(const Unexpected<E>& u) : m_hasValue(false), m_error(u.error()) {}
        Expected(Unexpected<E>&& u) : m_hasValue(false), m_error(std::move(u).error()) {}

        Expected(const Expected&) = default;
        Expected(Expected&&) noexcept = default;
        Expected& operator=(const Expected&) = default;
        Expected& operator=(Expected&&) noexcept = default;

        bool has_value() const noexcept { return m_hasValue; }
        explicit operator bool() const noexcept { return m_hasValue; }

        void value() const {}

        const E& error() const& { return m_error; }
        E&& error() && { return std::move(m_error); }

      private:
        bool m_hasValue{false};
        E m_error{};
    };
} // namespace Spark::Daemon
#endif
