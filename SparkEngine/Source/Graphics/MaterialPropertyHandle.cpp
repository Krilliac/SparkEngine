/**
 * @file MaterialPropertyHandle.cpp
 * @brief Handle-based material uniform access — eliminates string lookups at runtime
 */

#include "MaterialPropertyHandle.h"

namespace Spark::Graphics
{

    MaterialPropertyHandle MaterialPropertyRegistry::RegisterProperty(const std::string& name, PropertyType type,
                                                                      uint16_t bindingIndex, uint16_t offset)
    {
        auto it = m_nameToHandle.find(name);
        if (it != m_nameToHandle.end())
        {
            return it->second;
        }

        MaterialPropertyHandle handle;
        handle.encoded = (static_cast<uint32_t>(bindingIndex) & 0x3FF) | ((static_cast<uint32_t>(type) & 0x3F) << 10) |
                         ((static_cast<uint32_t>(offset) & 0xFFFF) << 16);

        m_nameToHandle[name] = handle;
        m_handleToName[handle.encoded] = name;
        m_handleToType[handle.encoded] = type;
        return handle;
    }

    MaterialPropertyHandle MaterialPropertyRegistry::FindProperty(const std::string& name) const
    {
        auto it = m_nameToHandle.find(name);
        if (it != m_nameToHandle.end())
            return it->second;
        return MaterialPropertyHandle::Invalid();
    }

    const std::string& MaterialPropertyRegistry::GetPropertyName(MaterialPropertyHandle handle) const
    {
        static const std::string empty;
        auto it = m_handleToName.find(handle.encoded);
        return (it != m_handleToName.end()) ? it->second : empty;
    }

    PropertyType MaterialPropertyRegistry::GetPropertyType(MaterialPropertyHandle handle) const
    {
        auto it = m_handleToType.find(handle.encoded);
        return (it != m_handleToType.end()) ? it->second : PropertyType::Float;
    }

    uint32_t MaterialPropertyRegistry::GetRegisteredPropertyCount() const
    {
        return static_cast<uint32_t>(m_nameToHandle.size());
    }

    std::string MaterialPropertyRegistry::Console_GetStatus() const
    {
        std::string status = "MaterialPropertyRegistry:\n";
        status += "  Registered: " + std::to_string(m_nameToHandle.size()) + "\n";
        for (const auto& [name, handle] : m_nameToHandle)
        {
            status += "    " + name + " [binding=" + std::to_string(handle.GetBindingIndex()) +
                      " offset=" + std::to_string(handle.GetOffset()) + "]\n";
        }
        return status;
    }

    void MaterialPropertyRegistry::Shutdown()
    {
        m_nameToHandle.clear();
        m_handleToName.clear();
        m_handleToType.clear();
    }

} // namespace Spark::Graphics
