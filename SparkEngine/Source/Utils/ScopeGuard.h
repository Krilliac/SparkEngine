/**
 * @file ScopeGuard.h
 * @brief RAII scope-exit guards that execute cleanup code on scope exit
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides three flavours of scope guard:
 * - `ScopeExit`   — runs the callback unconditionally when the scope ends.
 * - `ScopeSuccess` — runs the callback only if the scope exits normally (no exception).
 * - `ScopeFail`   — runs the callback only if the scope exits via an exception.
 *
 * Use the factory helpers `MakeScopeExit`, `MakeScopeSuccess`, and `MakeScopeFail`
 * to avoid repeating the lambda type. Disable a guard (prevent it from firing)
 * by calling `Dismiss()`.
 *
 * ## Design notes
 * - Guards are move-constructible but not copyable.
 * - Moved-from guards are automatically dismissed.
 * - The callback is stored by value; capture by reference if the callback must
 *   reference locals, but be careful about lifetime.
 *
 * ## Usage
 * @code
 *   FILE* f = fopen("data.bin", "rb");
 *   auto closeFile = Spark::MakeScopeExit([&]{ if (f) fclose(f); });
 *
 *   // COM / D3D pattern
 *   ID3D11Buffer* buf = nullptr;
 *   device->CreateBuffer(&desc, nullptr, &buf);
 *   auto releaseBuf = Spark::MakeScopeExit([&]{ if (buf) buf->Release(); });
 *
 *   // Only roll back on failure
 *   db.BeginTransaction();
 *   auto rollback = Spark::MakeScopeFail([&]{ db.Rollback(); });
 *   DoWork();
 *   db.Commit();
 *   rollback.Dismiss(); // commit succeeded — don't roll back
 *
 *   // Dismiss prevents firing
 *   auto g = Spark::MakeScopeExit([&]{ CleanUp(); });
 *   g.Dismiss(); // CleanUp() will NOT be called
 * @endcode
 */

#pragma once

#include <exception>
#include <type_traits>
#include <utility>

namespace Spark
{

    // =========================================================================
    // ScopeExit — always runs on scope exit
    // =========================================================================

    /**
     * @class ScopeExit
     * @brief Executes a callable unconditionally when the guard goes out of scope.
     *
     * @tparam F  Callable type stored in the guard (typically a lambda).
     */
    template <typename F> class ScopeExit
    {
      public:
        /**
         * @brief Construct a guard wrapping `func`.
         * @param func  Callable to invoke on scope exit.
         */
        explicit ScopeExit(F func) noexcept(std::is_nothrow_move_constructible_v<F>)
            : m_func(std::move(func)), m_active(true)
        {
        }

        ScopeExit(ScopeExit&& other) noexcept(std::is_nothrow_move_constructible_v<F>)
            : m_func(std::move(other.m_func)), m_active(other.m_active)
        {
            other.m_active = false;
        }

        ScopeExit(const ScopeExit&) = delete;
        ScopeExit& operator=(const ScopeExit&) = delete;
        ScopeExit& operator=(ScopeExit&&) = delete;

        /**
         * @brief Destroy the guard, invoking the callback if still active.
         */
        ~ScopeExit() noexcept
        {
            if (m_active)
                m_func();
        }

        /**
         * @brief Cancel the guard so the callback will not be called on destruction.
         */
        void Dismiss() noexcept { m_active = false; }

        /**
         * @brief Re-arm a previously dismissed guard.
         */
        void Arm() noexcept { m_active = true; }

        /**
         * @brief Return whether the guard is currently active.
         * @return  true if the callback will fire on destruction.
         */
        [[nodiscard]] bool IsActive() const noexcept { return m_active; }

      private:
        F m_func;
        bool m_active;
    };

    // =========================================================================
    // ScopeSuccess — runs only on successful (non-exception) scope exit
    // =========================================================================

    /**
     * @class ScopeSuccess
     * @brief Executes a callable only when the scope exits without an active exception.
     *
     * Useful for commit actions that should only run after all operations succeeded.
     *
     * @tparam F  Callable type stored in the guard.
     */
    template <typename F> class ScopeSuccess
    {
      public:
        /**
         * @brief Construct the guard.
         * @param func  Callable to invoke on successful scope exit.
         */
        explicit ScopeSuccess(F func) noexcept(std::is_nothrow_move_constructible_v<F>)
            : m_func(std::move(func)), m_active(true), m_exceptions(std::uncaught_exceptions())
        {
        }

        ScopeSuccess(ScopeSuccess&& other) noexcept(std::is_nothrow_move_constructible_v<F>)
            : m_func(std::move(other.m_func)), m_active(other.m_active), m_exceptions(other.m_exceptions)
        {
            other.m_active = false;
        }

        ScopeSuccess(const ScopeSuccess&) = delete;
        ScopeSuccess& operator=(const ScopeSuccess&) = delete;
        ScopeSuccess& operator=(ScopeSuccess&&) = delete;

        ~ScopeSuccess() noexcept(noexcept(m_func()))
        {
            if (m_active && std::uncaught_exceptions() == m_exceptions)
                m_func();
        }

        /**
         * @brief Prevent the callback from firing.
         */
        void Dismiss() noexcept { m_active = false; }

        /**
         * @brief Re-arm a previously dismissed guard.
         */
        void Arm() noexcept { m_active = true; }

        /**
         * @brief Return whether the guard is currently active.
         */
        [[nodiscard]] bool IsActive() const noexcept { return m_active; }

      private:
        F m_func;
        bool m_active;
        int m_exceptions;
    };

    // =========================================================================
    // ScopeFail — runs only when the scope exits via exception
    // =========================================================================

    /**
     * @class ScopeFail
     * @brief Executes a callable only when the scope exits due to an exception.
     *
     * Useful for rollback / undo actions that should only fire on failure.
     *
     * @tparam F  Callable type stored in the guard.
     */
    template <typename F> class ScopeFail
    {
      public:
        /**
         * @brief Construct the guard.
         * @param func  Callable to invoke on exception-triggered scope exit.
         */
        explicit ScopeFail(F func) noexcept(std::is_nothrow_move_constructible_v<F>)
            : m_func(std::move(func)), m_active(true), m_exceptions(std::uncaught_exceptions())
        {
        }

        ScopeFail(ScopeFail&& other) noexcept(std::is_nothrow_move_constructible_v<F>)
            : m_func(std::move(other.m_func)), m_active(other.m_active), m_exceptions(other.m_exceptions)
        {
            other.m_active = false;
        }

        ScopeFail(const ScopeFail&) = delete;
        ScopeFail& operator=(const ScopeFail&) = delete;
        ScopeFail& operator=(ScopeFail&&) = delete;

        ~ScopeFail() noexcept
        {
            if (m_active && std::uncaught_exceptions() > m_exceptions)
                m_func();
        }

        /**
         * @brief Prevent the callback from firing.
         */
        void Dismiss() noexcept { m_active = false; }

        /**
         * @brief Re-arm a previously dismissed guard.
         */
        void Arm() noexcept { m_active = true; }

        /**
         * @brief Return whether the guard is currently active.
         */
        [[nodiscard]] bool IsActive() const noexcept { return m_active; }

      private:
        F m_func;
        bool m_active;
        int m_exceptions;
    };

    // =========================================================================
    // Factory helpers
    // =========================================================================

    /**
     * @brief Create a `ScopeExit` guard from a callable.
     *
     * @param func  Callable to run unconditionally at the end of the scope.
     * @return      `ScopeExit<F>` guard wrapping `func`.
     *
     * @code
     *   auto g = Spark::MakeScopeExit([&]{ CleanUp(); });
     * @endcode
     */
    template <typename F>
    [[nodiscard]] ScopeExit<std::decay_t<F>> MakeScopeExit(F&& func) noexcept(
        std::is_nothrow_constructible_v<std::decay_t<F>, F>)
    {
        return ScopeExit<std::decay_t<F>>(std::forward<F>(func));
    }

    /**
     * @brief Create a `ScopeSuccess` guard from a callable.
     *
     * @param func  Callable to run only on normal (non-exception) scope exit.
     * @return      `ScopeSuccess<F>` guard wrapping `func`.
     */
    template <typename F>
    [[nodiscard]] ScopeSuccess<std::decay_t<F>> MakeScopeSuccess(F&& func) noexcept(
        std::is_nothrow_constructible_v<std::decay_t<F>, F>)
    {
        return ScopeSuccess<std::decay_t<F>>(std::forward<F>(func));
    }

    /**
     * @brief Create a `ScopeFail` guard from a callable.
     *
     * @param func  Callable to run only when the scope exits via an exception.
     * @return      `ScopeFail<F>` guard wrapping `func`.
     */
    template <typename F>
    [[nodiscard]] ScopeFail<std::decay_t<F>> MakeScopeFail(F&& func) noexcept(
        std::is_nothrow_constructible_v<std::decay_t<F>, F>)
    {
        return ScopeFail<std::decay_t<F>>(std::forward<F>(func));
    }

} // namespace Spark
