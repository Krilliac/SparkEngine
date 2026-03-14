/**
 * @file EnumUtils.h
 * @brief Utilities for enum operations, validation, and string conversion
 * @author Spark Engine Team
 * @date 2025
 * 
 * This file provides comprehensive utilities for working with enumerations
 * including type-safe validation, string conversion, iteration support,
 * and enum reflection capabilities.
 */

#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <type_traits>
#include <cassert>
#include <stdexcept>
#include <chrono>
#include "Utils/TypeTraits.h"

namespace SparkEditor
{

    /**
 * @brief Base template for enum utilities
 * 
 * Provides common functionality for enum validation and conversion.
 * Specialized for specific enum types to provide custom behavior.
 */
    template <Spark::Enum EnumType> class EnumUtils
    {
      public:
        /**
     * @brief Check if an enum value is valid
     * @param value Enum value to validate
     * @return true if the value is within valid range
     */
        static bool IsValid(EnumType value) { return IsValidImpl(value); }

        /**
     * @brief Convert enum to string representation
     * @param value Enum value to convert
     * @return String representation of the enum value
     */
        static std::string ToString(EnumType value) { return ToStringImpl(value); }

        /**
     * @brief Convert string to enum value
     * @param str String representation
     * @return Enum value, or default if conversion fails
     */
        static EnumType FromString(const std::string& str) { return FromStringImpl(str); }

        /**
     * @brief Get all valid enum values
     * @return Vector containing all valid enum values
     */
        static std::vector<EnumType> GetAllValues() { return GetAllValuesImpl(); }

        /**
     * @brief Get the count of valid enum values
     * @return Number of valid enum values
     */
        static size_t GetCount() { return GetAllValues().size(); }

        /**
     * @brief Get the underlying integer value
     * @param value Enum value
     * @return Integer representation
     */
        static int ToInt(EnumType value) { return static_cast<int>(value); }

        /**
     * @brief Convert integer to enum value with validation
     * @param intValue Integer value
     * @return Enum value if valid, default otherwise
     */
        static EnumType FromInt(int intValue)
        {
            EnumType value = static_cast<EnumType>(intValue);
            return IsValid(value) ? value : GetDefault();
        }

        /**
     * @brief Get the default enum value
     * @return Default enum value (typically the first valid value)
     */
        static EnumType GetDefault()
        {
            auto values = GetAllValues();
            return values.empty() ? static_cast<EnumType>(0) : values[0];
        }

      private:
        // Default implementations - should be specialized for specific enums
        static bool IsValidImpl(EnumType value)
        {
            // Default: assume sequential values starting from 0
            int intValue = static_cast<int>(value);
            return intValue >= 0 && intValue < static_cast<int>(GetAllValuesImpl().size());
        }

        static std::string ToStringImpl(EnumType value) { return "Unknown"; }

        static EnumType FromStringImpl(const std::string& str) { return GetDefault(); }

        static std::vector<EnumType> GetAllValuesImpl() { return {}; }
    };

/**
 * @brief Macro to help define enum utility specializations
 * 
 * Usage: DEFINE_ENUM_UTILS(MyEnum, { ENUM_VAL1, ENUM_VAL2, ... }, "DefaultString")
 */
#define DEFINE_ENUM_UTILS(EnumType, ValueList, DefaultStr)                                                             \
    template <> class EnumUtils<EnumType>                                                                              \
    {                                                                                                                  \
      public:                                                                                                          \
        static bool IsValid(EnumType value)                                                                            \
        {                                                                                                              \
            const auto& values = GetAllValues();                                                                       \
            return std::find(values.begin(), values.end(), value) != values.end();                                     \
        }                                                                                                              \
                                                                                                                       \
        static std::string ToString(EnumType value);                                                                   \
        static EnumType FromString(const std::string& str);                                                            \
        static std::vector<EnumType> GetAllValues()                                                                    \
        {                                                                                                              \
            return ValueList;                                                                                          \
        }                                                                                                              \
                                                                                                                       \
        static size_t GetCount()                                                                                       \
        {                                                                                                              \
            return GetAllValues().size();                                                                              \
        }                                                                                                              \
                                                                                                                       \
        static int ToInt(EnumType value)                                                                               \
        {                                                                                                              \
            return static_cast<int>(value);                                                                            \
        }                                                                                                              \
                                                                                                                       \
        static EnumType FromInt(int intValue)                                                                          \
        {                                                                                                              \
            EnumType value = static_cast<EnumType>(intValue);                                                          \
            return IsValid(value) ? value : GetDefault();                                                              \
        }                                                                                                              \
                                                                                                                       \
        static EnumType GetDefault()                                                                                   \
        {                                                                                                              \
            auto values = GetAllValues();                                                                              \
            return values.empty() ? static_cast<EnumType>(0) : values[0];                                              \
        }                                                                                                              \
    };

    /**
 * @brief Enum iteration helper
 * 
 * Provides range-based for loop support for enums.
 * Usage: for (auto value : EnumIterator<MyEnum>()) { ... }
 */
    template <Spark::Enum EnumType> class EnumIterator
    {
      public:
        class Iterator
        {
          public:
            Iterator(const std::vector<EnumType>& values, size_t index) : m_values(values), m_index(index) {}

            EnumType operator*() const { return m_values[m_index]; }

            Iterator& operator++()
            {
                ++m_index;
                return *this;
            }

            bool operator!=(const Iterator& other) const { return m_index != other.m_index; }

          private:
            const std::vector<EnumType>& m_values;
            size_t m_index;
        };

        EnumIterator() : m_values(EnumUtils<EnumType>::GetAllValues()) {}

        Iterator begin() const { return Iterator(m_values, 0); }
        Iterator end() const { return Iterator(m_values, m_values.size()); }

      private:
        std::vector<EnumType> m_values;
    };

    /**
 * @brief Type-safe enum flag operations
 * 
 * Provides bitwise operations for enum flags with type safety.
 */
    template <Spark::Enum EnumType> class EnumFlags
    {

      public:
        using UnderlyingType = std::underlying_type_t<EnumType>;

        EnumFlags() : m_value(0) {}
        EnumFlags(EnumType flag) : m_value(static_cast<UnderlyingType>(flag)) {}
        EnumFlags(UnderlyingType value) : m_value(value) {}

        // Bitwise operations
        EnumFlags operator|(EnumType flag) const { return EnumFlags(m_value | static_cast<UnderlyingType>(flag)); }

        EnumFlags operator&(EnumType flag) const { return EnumFlags(m_value & static_cast<UnderlyingType>(flag)); }

        EnumFlags operator^(EnumType flag) const { return EnumFlags(m_value ^ static_cast<UnderlyingType>(flag)); }

        EnumFlags operator~() const { return EnumFlags(~m_value); }

        EnumFlags& operator|=(EnumType flag)
        {
            m_value |= static_cast<UnderlyingType>(flag);
            return *this;
        }

        EnumFlags& operator&=(EnumType flag)
        {
            m_value &= static_cast<UnderlyingType>(flag);
            return *this;
        }

        EnumFlags& operator^=(EnumType flag)
        {
            m_value ^= static_cast<UnderlyingType>(flag);
            return *this;
        }

        // Test operations
        bool HasFlag(EnumType flag) const { return (m_value & static_cast<UnderlyingType>(flag)) != 0; }

        bool HasAllFlags(EnumFlags flags) const { return (m_value & flags.m_value) == flags.m_value; }

        bool HasAnyFlag(EnumFlags flags) const { return (m_value & flags.m_value) != 0; }

        // Utility
        void SetFlag(EnumType flag, bool enabled = true)
        {
            if (enabled)
            {
                m_value |= static_cast<UnderlyingType>(flag);
            }
            else
            {
                m_value &= ~static_cast<UnderlyingType>(flag);
            }
        }

        void ClearFlag(EnumType flag) { SetFlag(flag, false); }

        void Clear() { m_value = 0; }

        UnderlyingType GetValue() const { return m_value; }

        bool IsEmpty() const { return m_value == 0; }

        explicit operator bool() const { return !IsEmpty(); }

        bool operator==(const EnumFlags& other) const { return m_value == other.m_value; }

        bool operator!=(const EnumFlags& other) const { return m_value != other.m_value; }

      private:
        UnderlyingType m_value;
    };

    /**
 * @brief Enum validation helper
 * 
 * Provides runtime validation for enum values with detailed error reporting.
 */
    template <Spark::Enum EnumType> class EnumValidator
    {
      public:
        struct ValidationResult
        {
            bool isValid;
            std::string errorMessage;
            EnumType correctedValue;

            ValidationResult(bool valid, const std::string& error = "", EnumType corrected = {})
                : isValid(valid), errorMessage(error), correctedValue(corrected)
            {
            }
        };

        /**
     * @brief Validate enum value with detailed result
     * @param value Enum value to validate
     * @param allowCorrection Whether to provide corrected value
     * @return Validation result with error details
     */
        static ValidationResult Validate(EnumType value, bool allowCorrection = true)
        {
            if (EnumUtils<EnumType>::IsValid(value))
            {
                return ValidationResult(true);
            }

            std::string error = "Invalid enum value: " + std::to_string(static_cast<int>(value));
            EnumType corrected = allowCorrection ? EnumUtils<EnumType>::GetDefault() : value;

            return ValidationResult(false, error, corrected);
        }

        /**
     * @brief Validate and throw exception on invalid value
     * @param value Enum value to validate
     * @throws std::invalid_argument if value is invalid
     */
        static void ValidateOrThrow(EnumType value)
        {
            auto result = Validate(value, false);
            if (!result.isValid)
            {
                throw std::invalid_argument(result.errorMessage);
            }
        }

        /**
     * @brief Validate and correct invalid value
     * @param value Enum value to validate and potentially correct
     * @return Valid enum value (original if valid, default if invalid)
     */
        static EnumType ValidateOrCorrect(EnumType value)
        {
            auto result = Validate(value, true);
            return result.isValid ? value : result.correctedValue;
        }
    };

/**
 * @brief Convenience macros for enum operations
 */
#define ENUM_TO_STRING(enumValue) EnumUtils<decltype(enumValue)>::ToString(enumValue)
#define ENUM_FROM_STRING(enumType, str) EnumUtils<enumType>::FromString(str)
#define ENUM_IS_VALID(enumValue) EnumUtils<decltype(enumValue)>::IsValid(enumValue)
#define ENUM_VALIDATE(enumValue) EnumValidator<decltype(enumValue)>::ValidateOrCorrect(enumValue)

} // namespace SparkEditor
