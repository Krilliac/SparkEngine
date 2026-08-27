/**
 * @file TestConsoleVariables.cpp
 * @brief Tests for ConsoleVariable: CVar registration, type parsing,
 *        change callbacks, range validation, and registry management.
 *
 * Standalone reimplementation — mirrors Spark::CVar and Spark::CVarRegistry
 * without engine dependencies for CI/Linux compatibility.
 */

#include "TestFramework.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{

    enum class CVarFlags : uint32_t
    {
        None = 0,
        ReadOnly = 1 << 0,
        Save = 1 << 1,
        Cheat = 1 << 2,
        Hidden = 1 << 3,
        RequiresRestart = 1 << 4
    };

    inline CVarFlags operator|(CVarFlags a, CVarFlags b)
    {
        return static_cast<CVarFlags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
    }

    inline bool HasFlag(CVarFlags flags, CVarFlags flag)
    {
        return (static_cast<uint32_t>(flags) & static_cast<uint32_t>(flag)) != 0;
    }

    enum class CVarType
    {
        Int,
        Float,
        Bool,
        String
    };

    // --- ICVar interface ---

    class ICVar
    {
      public:
        virtual ~ICVar() = default;
        virtual const std::string& GetName() const = 0;
        virtual const std::string& GetDescription() const = 0;
        virtual CVarType GetType() const = 0;
        virtual CVarFlags GetFlags() const = 0;
        virtual std::string GetValueString() const = 0;
        virtual std::string GetDefaultString() const = 0;
        virtual bool SetFromString(const std::string& value) = 0;
        virtual void ResetToDefault() = 0;
        virtual bool IsModified() const = 0;
    };

    // --- CVar<T> ---

    template <typename T> class CVar : public ICVar
    {
      public:
        using ChangeCallback = std::function<void(const T& oldValue, const T& newValue)>;

        CVar(std::string name, T defaultVal, CVarFlags flags = CVarFlags::None, std::string description = "")
            : m_name(std::move(name)), m_description(std::move(description)), m_value(defaultVal),
              m_defaultValue(defaultVal), m_flags(flags), m_hasRange(false)
        {
        }

        CVar(std::string name, T defaultVal, CVarFlags flags, std::string description, T minVal, T maxVal)
            : m_name(std::move(name)), m_description(std::move(description)), m_value(defaultVal),
              m_defaultValue(defaultVal), m_flags(flags), m_hasRange(true), m_minVal(minVal), m_maxVal(maxVal)
        {
        }

        const std::string& GetName() const override { return m_name; }
        const std::string& GetDescription() const override { return m_description; }
        CVarFlags GetFlags() const override { return m_flags; }

        CVarType GetType() const override
        {
            if constexpr (std::is_same_v<T, int>)
                return CVarType::Int;
            else if constexpr (std::is_same_v<T, float>)
                return CVarType::Float;
            else if constexpr (std::is_same_v<T, bool>)
                return CVarType::Bool;
            else
                return CVarType::String;
        }

        const T& Get() const { return m_value; }

        void Set(const T& newValue)
        {
            if (HasFlag(m_flags, CVarFlags::ReadOnly))
                return;

            T clamped = newValue;
            if constexpr (std::is_arithmetic_v<T>)
            {
                if (m_hasRange)
                    clamped = std::clamp(clamped, m_minVal, m_maxVal);
            }

            if (m_value != clamped)
            {
                T oldValue = m_value;
                m_value = clamped;
                for (auto& cb : m_callbacks)
                    cb(oldValue, clamped);
            }
        }

        void OnChange(ChangeCallback callback) { m_callbacks.push_back(std::move(callback)); }

        std::string GetValueString() const override
        {
            if constexpr (std::is_same_v<T, int>)
                return std::to_string(m_value);
            else if constexpr (std::is_same_v<T, float>)
                return std::to_string(m_value);
            else if constexpr (std::is_same_v<T, bool>)
                return m_value ? "true" : "false";
            else
                return m_value;
        }

        std::string GetDefaultString() const override
        {
            if constexpr (std::is_same_v<T, int>)
                return std::to_string(m_defaultValue);
            else if constexpr (std::is_same_v<T, float>)
                return std::to_string(m_defaultValue);
            else if constexpr (std::is_same_v<T, bool>)
                return m_defaultValue ? "true" : "false";
            else
                return m_defaultValue;
        }

        bool SetFromString(const std::string& value) override
        {
            try
            {
                if constexpr (std::is_same_v<T, int>)
                    Set(std::stoi(value));
                else if constexpr (std::is_same_v<T, float>)
                    Set(std::stof(value));
                else if constexpr (std::is_same_v<T, bool>)
                    Set(value == "true" || value == "1");
                else
                    Set(value);
                return true;
            }
            catch (...)
            {
                return false;
            }
        }

        void ResetToDefault() override { m_value = m_defaultValue; }

        bool IsModified() const override { return m_value != m_defaultValue; }

      private:
        std::string m_name;
        std::string m_description;
        T m_value;
        T m_defaultValue;
        CVarFlags m_flags;
        bool m_hasRange;
        T m_minVal{};
        T m_maxVal{};
        std::vector<ChangeCallback> m_callbacks;
    };

    // --- CVarRegistry ---

    class CVarRegistry
    {
      public:
        void Register(ICVar* cvar)
        {
            if (cvar)
                m_cvars[cvar->GetName()] = cvar;
        }

        void Unregister(const std::string& name) { m_cvars.erase(name); }

        ICVar* Find(const std::string& name) const
        {
            auto it = m_cvars.find(name);
            return (it != m_cvars.end()) ? it->second : nullptr;
        }

        std::vector<ICVar*> GetAll() const
        {
            std::vector<ICVar*> result;
            for (auto& [name, cvar] : m_cvars)
                result.push_back(cvar);
            return result;
        }

        std::vector<ICVar*> FindByPrefix(const std::string& prefix) const
        {
            std::vector<ICVar*> result;
            for (auto& [name, cvar] : m_cvars)
            {
                if (name.substr(0, prefix.size()) == prefix)
                    result.push_back(cvar);
            }
            return result;
        }

        void ResetAll()
        {
            for (auto& [name, cvar] : m_cvars)
                cvar->ResetToDefault();
        }

        size_t Count() const { return m_cvars.size(); }

        bool AreCheatsEnabled() const { return m_cheatsEnabled; }
        void SetCheatsEnabled(bool enabled) { m_cheatsEnabled = enabled; }

      private:
        std::unordered_map<std::string, ICVar*> m_cvars;
        bool m_cheatsEnabled = false;
    };

} // anonymous namespace

// ============================================================================
// CVar Creation Tests
// ============================================================================

TEST(CVar_IntDefault)
{
    CVar<int> fov("r.fov", 90);
    EXPECT_EQ(fov.Get(), 90);
    EXPECT_EQ(fov.GetName(), std::string("r.fov"));
    EXPECT_EQ(static_cast<int>(fov.GetType()), static_cast<int>(CVarType::Int));
}

TEST(CVar_FloatDefault)
{
    CVar<float> sensitivity("input.sensitivity", 1.0f);
    EXPECT_NEAR(sensitivity.Get(), 1.0f, 0.01f);
    EXPECT_EQ(static_cast<int>(sensitivity.GetType()), static_cast<int>(CVarType::Float));
}

TEST(CVar_BoolDefault)
{
    CVar<bool> vsync("r.vsync", true);
    EXPECT_TRUE(vsync.Get());
    EXPECT_EQ(static_cast<int>(vsync.GetType()), static_cast<int>(CVarType::Bool));
}

TEST(CVar_StringDefault)
{
    CVar<std::string> name("player.name", "Player1");
    EXPECT_EQ(name.Get(), std::string("Player1"));
    EXPECT_EQ(static_cast<int>(name.GetType()), static_cast<int>(CVarType::String));
}

// ============================================================================
// CVar Set/Get Tests
// ============================================================================

TEST(CVar_SetInt)
{
    CVar<int> fov("r.fov", 90);
    fov.Set(110);
    EXPECT_EQ(fov.Get(), 110);
}

TEST(CVar_SetFloat)
{
    CVar<float> sensitivity("input.sensitivity", 1.0f);
    sensitivity.Set(2.5f);
    EXPECT_NEAR(sensitivity.Get(), 2.5f, 0.01f);
}

TEST(CVar_SetBool)
{
    CVar<bool> vsync("r.vsync", true);
    vsync.Set(false);
    EXPECT_FALSE(vsync.Get());
}

// ============================================================================
// Range Validation Tests
// ============================================================================

TEST(CVar_IntRangeClamped)
{
    CVar<int> fov("r.fov", 90, CVarFlags::None, "Field of view", 60, 120);
    fov.Set(150);
    EXPECT_EQ(fov.Get(), 120);

    fov.Set(30);
    EXPECT_EQ(fov.Get(), 60);
}

TEST(CVar_FloatRangeClamped)
{
    CVar<float> volume("audio.volume", 1.0f, CVarFlags::None, "Volume", 0.0f, 1.0f);
    volume.Set(1.5f);
    EXPECT_NEAR(volume.Get(), 1.0f, 0.01f);

    volume.Set(-0.5f);
    EXPECT_NEAR(volume.Get(), 0.0f, 0.01f);
}

TEST(CVar_InRangeNotClamped)
{
    CVar<int> fov("r.fov", 90, CVarFlags::None, "FOV", 60, 120);
    fov.Set(100);
    EXPECT_EQ(fov.Get(), 100);
}

// ============================================================================
// ReadOnly Tests
// ============================================================================

TEST(CVar_ReadOnlyIgnoresSet)
{
    CVar<int> version("engine.version", 42, CVarFlags::ReadOnly);
    version.Set(99);
    EXPECT_EQ(version.Get(), 42); // unchanged
}

// ============================================================================
// Callback Tests
// ============================================================================

TEST(CVar_ChangeCallback)
{
    CVar<int> fov("r.fov", 90);

    int oldVal = 0;
    int newVal = 0;
    fov.OnChange(
        [&](const int& old, const int& nw)
        {
            oldVal = old;
            newVal = nw;
        });

    fov.Set(110);
    EXPECT_EQ(oldVal, 90);
    EXPECT_EQ(newVal, 110);
}

TEST(CVar_MultipleCallbacks)
{
    CVar<float> volume("audio.volume", 1.0f);

    int callCount = 0;
    volume.OnChange([&](const float&, const float&) { callCount++; });
    volume.OnChange([&](const float&, const float&) { callCount++; });

    volume.Set(0.5f);
    EXPECT_EQ(callCount, 2);
}

TEST(CVar_NoCallbackOnSameValue)
{
    CVar<int> fov("r.fov", 90);

    int callCount = 0;
    fov.OnChange([&](const int&, const int&) { callCount++; });

    fov.Set(90); // same value
    EXPECT_EQ(callCount, 0);
}

// ============================================================================
// String Conversion Tests
// ============================================================================

TEST(CVar_SetFromStringInt)
{
    CVar<int> fov("r.fov", 90);
    EXPECT_TRUE(fov.SetFromString("120"));
    EXPECT_EQ(fov.Get(), 120);
}

TEST(CVar_SetFromStringFloat)
{
    CVar<float> volume("audio.volume", 1.0f);
    EXPECT_TRUE(volume.SetFromString("0.75"));
    EXPECT_NEAR(volume.Get(), 0.75f, 0.01f);
}

TEST(CVar_SetFromStringBool)
{
    CVar<bool> vsync("r.vsync", false);
    EXPECT_TRUE(vsync.SetFromString("true"));
    EXPECT_TRUE(vsync.Get());

    EXPECT_TRUE(vsync.SetFromString("0"));
    EXPECT_FALSE(vsync.Get());
}

TEST(CVar_SetFromStringInvalid)
{
    CVar<int> fov("r.fov", 90);
    EXPECT_FALSE(fov.SetFromString("not_a_number"));
    EXPECT_EQ(fov.Get(), 90); // unchanged
}

TEST(CVar_GetValueString)
{
    CVar<int> fov("r.fov", 90);
    EXPECT_EQ(fov.GetValueString(), std::string("90"));

    CVar<bool> vsync("r.vsync", true);
    EXPECT_EQ(vsync.GetValueString(), std::string("true"));
}

// ============================================================================
// IsModified / ResetToDefault Tests
// ============================================================================

TEST(CVar_IsModified)
{
    CVar<int> fov("r.fov", 90);
    EXPECT_FALSE(fov.IsModified());

    fov.Set(110);
    EXPECT_TRUE(fov.IsModified());
}

TEST(CVar_ResetToDefault)
{
    CVar<float> volume("audio.volume", 1.0f);
    volume.Set(0.3f);
    EXPECT_TRUE(volume.IsModified());

    volume.ResetToDefault();
    EXPECT_NEAR(volume.Get(), 1.0f, 0.01f);
    EXPECT_FALSE(volume.IsModified());
}

// ============================================================================
// Registry Tests
// ============================================================================

TEST(CVarRegistry_RegisterAndFind)
{
    CVarRegistry registry;
    CVar<int> fov("r.fov", 90);

    registry.Register(&fov);
    auto* found = registry.Find("r.fov");
    ASSERT_TRUE(found != nullptr);
    EXPECT_EQ(found->GetName(), std::string("r.fov"));
}

TEST(CVarRegistry_FindMissing)
{
    CVarRegistry registry;
    EXPECT_TRUE(registry.Find("nonexistent") == nullptr);
}

TEST(CVarRegistry_Unregister)
{
    CVarRegistry registry;
    CVar<int> fov("r.fov", 90);
    registry.Register(&fov);

    registry.Unregister("r.fov");
    EXPECT_TRUE(registry.Find("r.fov") == nullptr);
}

TEST(CVarRegistry_Count)
{
    CVarRegistry registry;
    CVar<int> fov("r.fov", 90);
    CVar<float> vol("audio.volume", 1.0f);
    CVar<bool> vsync("r.vsync", true);

    registry.Register(&fov);
    registry.Register(&vol);
    registry.Register(&vsync);
    EXPECT_EQ(registry.Count(), (size_t)3);
}

TEST(CVarRegistry_FindByPrefix)
{
    CVarRegistry registry;
    CVar<int> fov("r.fov", 90);
    CVar<bool> vsync("r.vsync", true);
    CVar<float> vol("audio.volume", 1.0f);

    registry.Register(&fov);
    registry.Register(&vsync);
    registry.Register(&vol);

    auto renderVars = registry.FindByPrefix("r.");
    EXPECT_EQ(renderVars.size(), (size_t)2);
}

TEST(CVarRegistry_ResetAll)
{
    CVarRegistry registry;
    CVar<int> fov("r.fov", 90);
    CVar<float> vol("audio.volume", 1.0f);

    registry.Register(&fov);
    registry.Register(&vol);

    fov.Set(110);
    vol.Set(0.3f);

    registry.ResetAll();
    EXPECT_EQ(fov.Get(), 90);
    EXPECT_NEAR(vol.Get(), 1.0f, 0.01f);
}

// ============================================================================
// Cheat Flag Tests
// ============================================================================

TEST(CVarRegistry_CheatsDisabledByDefault)
{
    CVarRegistry registry;
    EXPECT_FALSE(registry.AreCheatsEnabled());
}

TEST(CVarRegistry_EnableCheats)
{
    CVarRegistry registry;
    registry.SetCheatsEnabled(true);
    EXPECT_TRUE(registry.AreCheatsEnabled());
}

// ============================================================================
// Flags Tests
// ============================================================================

TEST(CVar_FlagsPreserved)
{
    CVar<int> fov("r.fov", 90, CVarFlags::Save | CVarFlags::RequiresRestart, "Field of view");
    EXPECT_TRUE(HasFlag(fov.GetFlags(), CVarFlags::Save));
    EXPECT_TRUE(HasFlag(fov.GetFlags(), CVarFlags::RequiresRestart));
    EXPECT_FALSE(HasFlag(fov.GetFlags(), CVarFlags::ReadOnly));
}

TEST(CVar_DescriptionPreserved)
{
    CVar<int> fov("r.fov", 90, CVarFlags::None, "Field of view in degrees");
    EXPECT_EQ(fov.GetDescription(), std::string("Field of view in degrees"));
}
