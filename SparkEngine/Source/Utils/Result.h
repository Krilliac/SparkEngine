/**
 * @file Result.h
 * @brief Type-safe error handling via Result<T>
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides a Result<T> type for functions that can fail, replacing
 * the mix of HRESULT, bool, and catch-all exception handlers.
 *
 * ## Usage
 * @code
 *   Result<Texture> LoadTexture(const std::string& path) {
 *       if (!FileExists(path))
 *           return Err("Texture not found: " + path);
 *       return Ok(texture);
 *   }
 *
 *   auto result = LoadTexture("brick.tga");
 *   if (result) {
 *       Use(result.Value());
 *   } else {
 *       Log(result.Error());
 *   }
 * @endcode
 */

#pragma once

#include <string>
#include <variant>
#include <optional>
#include <utility>

namespace Spark
{

    /**
 * @brief Error information for Result<T>
 */
    struct Error
    {
        std::string message;
        int code = 0;

        Error() = default;
        explicit Error(std::string msg, int c = 0) : message(std::move(msg)), code(c) {}
    };

    /**
 * @brief Result type for operations that can fail.
 *
 * Holds either a value of type T or an Error.
 * For void-returning functions, use Result<void>.
 */
    template <typename T> class Result
    {
      public:
        /// Construct a success result
        Result(T value) : m_data(std::move(value)) {}

        /// Construct an error result
        Result(Error error) : m_data(std::move(error)) {}

        /// Check if the result holds a value
        bool IsOk() const { return std::holds_alternative<T>(m_data); }

        /// Check if the result holds an error
        bool IsErr() const { return std::holds_alternative<Error>(m_data); }

        /// Boolean conversion — true if Ok
        explicit operator bool() const { return IsOk(); }

        /// Access the value (undefined if IsErr)
        T& Value() { return std::get<T>(m_data); }
        const T& Value() const { return std::get<T>(m_data); }

        /// Access the value with a fallback
        T ValueOr(T fallback) const
        {
            if (IsOk())
                return std::get<T>(m_data);
            return fallback;
        }

        /// Access the error (undefined if IsOk)
        const Error& GetError() const { return std::get<Error>(m_data); }

        /// Get the error message (empty string if Ok)
        std::string ErrorMessage() const
        {
            if (IsErr())
                return std::get<Error>(m_data).message;
            return {};
        }

      private:
        std::variant<T, Error> m_data;
    };

    /**
 * @brief Specialization for void results (success/failure with no value)
 */
    template <> class Result<void>
    {
      public:
        /// Construct a success result
        Result() : m_error(std::nullopt) {}

        /// Construct an error result
        Result(Error error) : m_error(std::move(error)) {}

        bool IsOk() const { return !m_error.has_value(); }
        bool IsErr() const { return m_error.has_value(); }
        explicit operator bool() const { return IsOk(); }

        const Error& GetError() const { return m_error.value(); }
        std::string ErrorMessage() const
        {
            if (m_error)
                return m_error->message;
            return {};
        }

      private:
        std::optional<Error> m_error;
    };

    /// Helper to create a success Result
    template <typename T> Result<T> Ok(T value)
    {
        return Result<T>(std::move(value));
    }

    /// Helper to create a void success Result
    inline Result<void> Ok()
    {
        return Result<void>();
    }

    /// Helper to create an error Result
    inline Error Err(std::string message, int code = 0)
    {
        return Error(std::move(message), code);
    }

} // namespace Spark
