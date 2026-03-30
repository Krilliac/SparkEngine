/**
 * @file StringUtils.h
 * @brief String manipulation utilities for Spark Engine
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides common string operations used throughout the engine:
 * case conversion, trimming, splitting/joining, search, replace,
 * and type conversion with error handling.
 *
 * ## Usage
 * @code
 *   using namespace Spark::StringUtils;
 *
 *   auto parts = Split("key=value", '=');
 *   std::string lower = ToLower("Hello World");
 *   auto num = ParseInt("42");  // std::optional<int>(42)
 *   std::string formatted = Format("FPS: %d", 60);
 * @endcode
 */

#pragma once

#include <string>
#include <vector>
#include <algorithm>
#include <optional>
#include <cstdio>
#include <cctype>
#include <cstdarg>
#include <sstream>
#include <cstdint>

namespace Spark
{
    namespace StringUtils
    {

        // =============================================================================
        // Case Conversion
        // =============================================================================

        inline std::string ToLower(std::string str)
        {
            std::transform(str.begin(), str.end(), str.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return str;
        }

        inline std::string ToUpper(std::string str)
        {
            std::transform(str.begin(), str.end(), str.begin(),
                           [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            return str;
        }

        // =============================================================================
        // Trimming
        // =============================================================================

        inline std::string TrimLeft(const std::string& str)
        {
            size_t start = str.find_first_not_of(" \t\n\r\f\v");
            return (start == std::string::npos) ? "" : str.substr(start);
        }

        inline std::string TrimRight(const std::string& str)
        {
            size_t end = str.find_last_not_of(" \t\n\r\f\v");
            return (end == std::string::npos) ? "" : str.substr(0, end + 1);
        }

        inline std::string Trim(const std::string& str)
        {
            return TrimLeft(TrimRight(str));
        }

        // =============================================================================
        // Query
        // =============================================================================

        inline bool StartsWith(const std::string& str, const std::string& prefix)
        {
            if (prefix.size() > str.size())
                return false;
            return str.compare(0, prefix.size(), prefix) == 0;
        }

        inline bool EndsWith(const std::string& str, const std::string& suffix)
        {
            if (suffix.size() > str.size())
                return false;
            return str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
        }

        inline bool Contains(const std::string& str, const std::string& substr)
        {
            return str.contains(substr);
        }

        // =============================================================================
        // Split / Join
        // =============================================================================

        inline std::vector<std::string> Split(const std::string& str, char delimiter)
        {
            std::vector<std::string> tokens;
            std::string token;
            std::istringstream stream(str);
            while (std::getline(stream, token, delimiter))
            {
                tokens.push_back(token);
            }
            return tokens;
        }

        inline std::vector<std::string> Split(const std::string& str, const std::string& delimiter)
        {
            std::vector<std::string> tokens;
            if (delimiter.empty())
            {
                tokens.push_back(str);
                return tokens;
            }
            size_t start = 0;
            size_t end = str.find(delimiter);
            while (end != std::string::npos)
            {
                tokens.push_back(str.substr(start, end - start));
                start = end + delimiter.size();
                end = str.find(delimiter, start);
            }
            tokens.push_back(str.substr(start));
            return tokens;
        }

        inline std::string Join(const std::vector<std::string>& parts, const std::string& delimiter)
        {
            if (parts.empty())
                return "";
            std::string result = parts[0];
            for (size_t i = 1; i < parts.size(); ++i)
            {
                result += delimiter;
                result += parts[i];
            }
            return result;
        }

        // =============================================================================
        // Replace
        // =============================================================================

        inline std::string Replace(const std::string& str, const std::string& from, const std::string& to)
        {
            if (from.empty())
                return str;
            size_t pos = str.find(from);
            if (pos == std::string::npos)
                return str;
            std::string result = str;
            result.replace(pos, from.size(), to);
            return result;
        }

        inline std::string ReplaceAll(const std::string& str, const std::string& from, const std::string& to)
        {
            if (from.empty())
                return str;
            std::string result = str;
            size_t pos = 0;
            while ((pos = result.find(from, pos)) != std::string::npos)
            {
                result.replace(pos, from.size(), to);
                pos += to.size();
            }
            return result;
        }

        // =============================================================================
        // Type Conversion
        // =============================================================================

        inline std::string ToString(int value)
        {
            return std::to_string(value);
        }
        inline std::string ToString(long value)
        {
            return std::to_string(value);
        }
        inline std::string ToString(long long value)
        {
            return std::to_string(value);
        }
        inline std::string ToString(unsigned int value)
        {
            return std::to_string(value);
        }
        inline std::string ToString(unsigned long value)
        {
            return std::to_string(value);
        }
        inline std::string ToString(unsigned long long value)
        {
            return std::to_string(value);
        }
        inline std::string ToString(float value)
        {
            return std::to_string(value);
        }
        inline std::string ToString(double value)
        {
            return std::to_string(value);
        }
        inline std::string ToString(bool value)
        {
            return value ? "true" : "false";
        }

        inline std::optional<int> ParseInt(const std::string& str)
        {
            try
            {
                size_t pos = 0;
                int val = std::stoi(str, &pos);
                if (pos == str.size())
                    return val;
            }
            catch (...)
            {
            }
            return std::nullopt;
        }

        inline std::optional<float> ParseFloat(const std::string& str)
        {
            try
            {
                size_t pos = 0;
                float val = std::stof(str, &pos);
                if (pos == str.size())
                    return val;
            }
            catch (...)
            {
            }
            return std::nullopt;
        }

        inline std::optional<bool> ParseBool(const std::string& str)
        {
            std::string lower = ToLower(Trim(str));
            if (lower == "true" || lower == "1" || lower == "yes" || lower == "on")
                return true;
            if (lower == "false" || lower == "0" || lower == "no" || lower == "off")
                return false;
            return std::nullopt;
        }

        // =============================================================================
        // Formatting
        // =============================================================================

        inline std::string Format(const char* fmt, ...)
        {
            va_list args;
            va_start(args, fmt);
            va_list args_copy;
            va_copy(args_copy, args);
            int size = std::vsnprintf(nullptr, 0, fmt, args);
            va_end(args);
            if (size < 0)
            {
                va_end(args_copy);
                return "";
            }
            std::string result(static_cast<size_t>(size), '\0');
            std::vsnprintf(result.data(), static_cast<size_t>(size) + 1, fmt, args_copy);
            va_end(args_copy);
            return result;
        }

    } // namespace StringUtils
} // namespace Spark
