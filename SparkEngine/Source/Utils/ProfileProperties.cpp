/**
 * @file ProfileProperties.cpp
 * @brief Implementation of frame-resetting typed profile properties
 */

#include "ProfileProperties.h"
#include "Validate.h"

#include <format>

namespace Spark
{

    ProfileProperties& ProfileProperties::GetInstance()
    {
        static ProfileProperties instance;
        return instance;
    }

    uint32_t ProfileProperties::RegisterProperty(const std::string& name, ProfilePropertyFlags flags)
    {
        uint32_t id = static_cast<uint32_t>(m_properties.size());
        SPARK_LOG_DEBUG(Spark::LogCategory::Core, "ProfileProperties::RegisterProperty id=%u name='%s'", id,
                        name.c_str());
        m_properties.push_back({name, 0.0f, flags});
        return id;
    }

    void ProfileProperties::SetFloat(uint32_t id, float value)
    {
        if (id < static_cast<uint32_t>(m_properties.size()))
        {
            m_properties[id].value = value;
        }
    }

    void ProfileProperties::AddFloat(uint32_t id, float delta)
    {
        if (id < static_cast<uint32_t>(m_properties.size()))
        {
            m_properties[id].value += delta;
        }
    }

    void ProfileProperties::SetUInt(uint32_t id, uint32_t value)
    {
        if (id < static_cast<uint32_t>(m_properties.size()))
        {
            m_properties[id].value = static_cast<float>(value);
        }
    }

    void ProfileProperties::AddUInt(uint32_t id, uint32_t delta)
    {
        if (id < static_cast<uint32_t>(m_properties.size()))
        {
            m_properties[id].value += static_cast<float>(delta);
        }
    }

    float ProfileProperties::GetFloat(uint32_t id) const
    {
        if (id < static_cast<uint32_t>(m_properties.size()))
        {
            return m_properties[id].value;
        }
        return 0.0f;
    }

    uint32_t ProfileProperties::GetUInt(uint32_t id) const
    {
        if (id < static_cast<uint32_t>(m_properties.size()))
        {
            return static_cast<uint32_t>(m_properties[id].value);
        }
        return 0;
    }

    void ProfileProperties::ResetFrameProperties()
    {
        for (auto& prop : m_properties)
        {
            if ((prop.flags & ProfilePropertyFlags::FrameReset) != ProfilePropertyFlags::None)
            {
                prop.value = 0.0f;
            }
        }
    }

    const std::vector<ProfilePropertyEntry>& ProfileProperties::GetAllProperties() const
    {
        return m_properties;
    }

    std::string ProfileProperties::Console_GetStatus() const
    {
        std::string result = std::format("ProfileProperties: {} properties registered\n", m_properties.size());

        for (uint32_t i = 0; i < static_cast<uint32_t>(m_properties.size()); ++i)
        {
            const auto& prop = m_properties[i];
            const char* flagStr =
                (prop.flags & ProfilePropertyFlags::FrameReset) != ProfilePropertyFlags::None ? " [FrameReset]" : "";

            result += std::format("  [{}] {}: {:.2f}{}\n", i, prop.name, prop.value, flagStr);
        }

        return result;
    }

    void ProfileProperties::Shutdown()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Core, "ProfileProperties::Shutdown — clearing %zu properties",
                       m_properties.size());
        m_properties.clear();
    }

} // namespace Spark
