/**
 * @file ConsoleVariable.h
 * @brief Typed Console Variable (CVar) system for runtime configuration
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides a type-safe CVar system that automatically registers get/set
 * console commands for bound variables. Supports int, float, bool, and
 * string types with optional range clamping and change callbacks.
 *
 * Usage:
 * @code
 *   // Register a cvar with automatic console commands
 *   static Spark::CVar<float> cv_gravity("physics.gravity", -20.0f,
 *       Spark::CVarFlags::Save, "World gravity", -100.0f, 0.0f);
 *
 *   // Read/write in code
 *   float g = cv_gravity.Get();
 *   cv_gravity.Set(-9.81f);
 *
 *   // Console auto-generates commands:
 *   //   physics.gravity       -> prints current value
 *   //   physics.gravity -9.81 -> sets new value
 *   //   cvar_list             -> lists all registered cvars
 *   //   cvar_reset_all        -> resets all cvars to defaults
 * @endcode
 */

#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <functional>
#include <mutex>
#include <sstream>
#include <algorithm>
#include <type_traits>
#include <cstdint>

namespace Spark
{

    // ============================================================================
    // CVar Flags
    // ============================================================================

    enum class CVarFlags : uint32_t
    {
        None = 0,
        ReadOnly = 1 << 0,  ///< Cannot be modified at runtime via console
        Save = 1 << 1,      ///< Persisted to config file
        Cheat = 1 << 2,     ///< Only modifiable when cheats are enabled
        Hidden = 1 << 3,    ///< Not shown in cvar_list output
        RequiresRestart = 1 << 4  ///< Change takes effect after engine restart
    };

    inline CVarFlags operator|(CVarFlags a, CVarFlags b)
    {
        return static_cast<CVarFlags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
    }

    inline bool operator&(CVarFlags a, CVarFlags b)
    {
        return (static_cast<uint32_t>(a) & static_cast<uint32_t>(b)) != 0;
    }

    // ============================================================================
    // CVar Type Enum
    // ============================================================================

    enum class CVarType : uint8_t
    {
        Int,
        Float,
        Bool,
        String
    };

    inline const char* CVarTypeToString(CVarType type)
    {
        switch (type)
        {
        case CVarType::Int:    return "int";
        case CVarType::Float:  return "float";
        case CVarType::Bool:   return "bool";
        case CVarType::String: return "string";
        default:               return "unknown";
        }
    }

    // ============================================================================
    // ICVar - Type-erased interface for the CVar registry
    // ============================================================================

    class ICVar
    {
    public:
        virtual ~ICVar() = default;

        virtual const std::string& GetName() const = 0;
        virtual const std::string& GetDescription() const = 0;
        virtual CVarType GetType() const = 0;
        virtual CVarFlags GetFlags() const = 0;

        /** @brief Get the current value as a display string */
        virtual std::string GetValueString() const = 0;

        /** @brief Get the default value as a display string */
        virtual std::string GetDefaultString() const = 0;

        /** @brief Set value from a string. Returns true on success. */
        virtual bool SetFromString(const std::string& value) = 0;

        /** @brief Reset to the default value */
        virtual void ResetToDefault() = 0;

        /** @brief Check if current value differs from default */
        virtual bool IsModified() const = 0;
    };

    // ============================================================================
    // CVarRegistry - Global registry of all console variables
    // ============================================================================

    class CVarRegistry
    {
    public:
        static CVarRegistry& Get()
        {
            static CVarRegistry instance;
            return instance;
        }

        void Register(ICVar* cvar)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_cvars[cvar->GetName()] = cvar;
        }

        void Unregister(const std::string& name)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_cvars.erase(name);
        }

        ICVar* Find(const std::string& name) const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_cvars.find(name);
            return (it != m_cvars.end()) ? it->second : nullptr;
        }

        /** @brief Get all registered cvars sorted by name */
        std::vector<ICVar*> GetAll() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            std::vector<ICVar*> result;
            result.reserve(m_cvars.size());
            for (const auto& pair : m_cvars)
            {
                result.push_back(pair.second);
            }
            std::sort(result.begin(), result.end(),
                      [](const ICVar* a, const ICVar* b) { return a->GetName() < b->GetName(); });
            return result;
        }

        /** @brief Find all cvars matching a prefix */
        std::vector<ICVar*> FindByPrefix(const std::string& prefix) const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            std::vector<ICVar*> result;
            for (const auto& pair : m_cvars)
            {
                if (pair.first.size() >= prefix.size() &&
                    pair.first.compare(0, prefix.size(), prefix) == 0)
                {
                    result.push_back(pair.second);
                }
            }
            std::sort(result.begin(), result.end(),
                      [](const ICVar* a, const ICVar* b) { return a->GetName() < b->GetName(); });
            return result;
        }

        /** @brief Reset all non-readonly cvars to their defaults */
        void ResetAll()
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (auto& pair : m_cvars)
            {
                if (!(pair.second->GetFlags() & CVarFlags::ReadOnly))
                {
                    pair.second->ResetToDefault();
                }
            }
        }

        /** @brief Get count of registered cvars */
        size_t Count() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_cvars.size();
        }

        /** @brief Check if cheats are enabled (controls Cheat-flagged cvars) */
        bool AreCheatsEnabled() const { return m_cheatsEnabled; }
        void SetCheatsEnabled(bool enabled) { m_cheatsEnabled = enabled; }

    private:
        CVarRegistry() = default;
        ~CVarRegistry() = default;

        mutable std::mutex m_mutex;
        std::unordered_map<std::string, ICVar*> m_cvars;
        bool m_cheatsEnabled = false;
    };

    // ============================================================================
    // CVar<T> - Typed console variable
    // ============================================================================

    template <typename T>
    class CVar : public ICVar
    {
    public:
        using ChangeCallback = std::function<void(const T& oldValue, const T& newValue)>;

        /**
         * @brief Construct and auto-register a console variable
         * @param name        Dotted name (e.g., "physics.gravity")
         * @param defaultVal  Default value
         * @param flags       Behavioral flags
         * @param description Human-readable description
         */
        CVar(std::string name, T defaultVal, CVarFlags flags = CVarFlags::None,
             std::string description = "")
            : m_name(std::move(name))
            , m_description(std::move(description))
            , m_flags(flags)
            , m_value(defaultVal)
            , m_defaultValue(defaultVal)
            , m_hasRange(false)
            , m_min{}
            , m_max{}
        {
            CVarRegistry::Get().Register(this);
        }

        /**
         * @brief Construct with range clamping (numeric types only)
         */
        template <typename U = T, typename = std::enable_if_t<std::is_arithmetic_v<U>>>
        CVar(std::string name, T defaultVal, CVarFlags flags,
             std::string description, T minVal, T maxVal)
            : m_name(std::move(name))
            , m_description(std::move(description))
            , m_flags(flags)
            , m_value(defaultVal)
            , m_defaultValue(defaultVal)
            , m_hasRange(true)
            , m_min(minVal)
            , m_max(maxVal)
        {
            CVarRegistry::Get().Register(this);
        }

        ~CVar() override
        {
            CVarRegistry::Get().Unregister(m_name);
        }

        // Non-copyable, non-movable (registered by pointer)
        CVar(const CVar&) = delete;
        CVar& operator=(const CVar&) = delete;
        CVar(CVar&&) = delete;
        CVar& operator=(CVar&&) = delete;

        // ---- Value access ----

        const T& Get() const { return m_value; }

        void Set(const T& newValue)
        {
            if (m_flags & CVarFlags::ReadOnly)
            {
                return;
            }

            T clamped = newValue;
            if constexpr (std::is_arithmetic_v<T>)
            {
                if (m_hasRange)
                {
                    clamped = std::clamp(clamped, m_min, m_max);
                }
            }

            T oldValue = m_value;
            m_value = clamped;

            if (m_onChange)
            {
                m_onChange(oldValue, m_value);
            }
        }

        /** @brief Register a callback invoked whenever the value changes */
        void OnChange(ChangeCallback callback) { m_onChange = std::move(callback); }

        // ---- ICVar interface ----

        const std::string& GetName() const override { return m_name; }
        const std::string& GetDescription() const override { return m_description; }
        CVarFlags GetFlags() const override { return m_flags; }

        CVarType GetType() const override
        {
            if constexpr (std::is_same_v<T, int>)         return CVarType::Int;
            else if constexpr (std::is_same_v<T, float>)  return CVarType::Float;
            else if constexpr (std::is_same_v<T, bool>)   return CVarType::Bool;
            else                                           return CVarType::String;
        }

        std::string GetValueString() const override
        {
            return ToString(m_value);
        }

        std::string GetDefaultString() const override
        {
            return ToString(m_defaultValue);
        }

        bool SetFromString(const std::string& str) override
        {
            if (m_flags & CVarFlags::ReadOnly)
            {
                return false;
            }

            if ((m_flags & CVarFlags::Cheat) && !CVarRegistry::Get().AreCheatsEnabled())
            {
                return false;
            }

            T parsed;
            if (!FromString(str, parsed))
            {
                return false;
            }

            Set(parsed);
            return true;
        }

        void ResetToDefault() override
        {
            m_value = m_defaultValue;
        }

        bool IsModified() const override
        {
            return !(m_value == m_defaultValue);
        }

        /** @brief Get range info string (empty if no range) */
        std::string GetRangeString() const
        {
            if constexpr (std::is_arithmetic_v<T>)
            {
                if (m_hasRange)
                {
                    return "[" + ToString(m_min) + ", " + ToString(m_max) + "]";
                }
            }
            return "";
        }

    private:
        static std::string ToString(const T& val)
        {
            if constexpr (std::is_same_v<T, bool>)
            {
                return val ? "true" : "false";
            }
            else if constexpr (std::is_same_v<T, std::string>)
            {
                return val;
            }
            else if constexpr (std::is_same_v<T, float>)
            {
                std::ostringstream oss;
                oss << val;
                return oss.str();
            }
            else
            {
                return std::to_string(val);
            }
        }

        static bool FromString(const std::string& str, T& out)
        {
            if constexpr (std::is_same_v<T, bool>)
            {
                std::string lower = str;
                std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
                if (lower == "true" || lower == "1" || lower == "on" || lower == "yes")
                {
                    out = true;
                    return true;
                }
                if (lower == "false" || lower == "0" || lower == "off" || lower == "no")
                {
                    out = false;
                    return true;
                }
                return false;
            }
            else if constexpr (std::is_same_v<T, int>)
            {
                try { out = std::stoi(str); return true; }
                catch (...) { return false; }
            }
            else if constexpr (std::is_same_v<T, float>)
            {
                try { out = std::stof(str); return true; }
                catch (...) { return false; }
            }
            else if constexpr (std::is_same_v<T, std::string>)
            {
                out = str;
                return true;
            }
            else
            {
                return false;
            }
        }

        std::string m_name;
        std::string m_description;
        CVarFlags m_flags;
        T m_value;
        T m_defaultValue;
        bool m_hasRange;
        T m_min;
        T m_max;
        ChangeCallback m_onChange;
    };

} // namespace Spark
