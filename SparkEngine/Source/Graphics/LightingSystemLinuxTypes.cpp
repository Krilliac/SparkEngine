/**
 * @file LightingSystemLinuxTypes.cpp
 * @brief Linux Light class implementation and light/shadow type string conversions
 *
 * Split from LightingSystemLinux.cpp, which keeps the LightingSystem class.
 * The Windows counterpart lives in LightingSystemWindows.cpp.
 */
#include "Core/Platform.h"
#include "LightingSystem.h"
#include "Utils/MathUtils.h"
#ifndef SPARK_PLATFORM_WINDOWS


#include "../Utils/Hash.h"
#include <sstream>
#include <algorithm>
#include <cmath>

// ============================================================================
// Light (Linux stub)
// ============================================================================

Light::Light(LightType type) : m_type(type)
{
    switch (type)
    {
    case LightType::Directional:
        m_position = {0.0f, 10.0f, 0.0f};
        m_direction = {0.0f, -1.0f, 0.0f};
        m_intensity = 3.0f;
        m_range = 1000.0f;
        break;
    case LightType::Point:
        m_position = {0.0f, 2.0f, 0.0f};
        m_direction = {0.0f, -1.0f, 0.0f};
        m_intensity = 10.0f;
        m_range = 10.0f;
        break;
    case LightType::Spot:
        m_position = {0.0f, 5.0f, 0.0f};
        m_direction = {0.0f, -1.0f, 0.0f};
        m_intensity = 15.0f;
        m_range = 15.0f;
        m_spotAngle = 30.0f;
        break;
    case LightType::Area:
        m_position = {0.0f, 3.0f, 0.0f};
        m_direction = {0.0f, -1.0f, 0.0f};
        m_intensity = 8.0f;
        m_range = 12.0f;
        break;
    case LightType::Environment:
        m_intensity = 1.0f;
        m_castShadows = false;
        break;
    }
    m_dirty = true;
}

XMMATRIX Light::GetLightMatrix() const
{
    // Linux stub: a default-constructed XMMATRIX is all zeros.
    return XMMATRIX{};
}

XMMATRIX Light::GetShadowMatrix() const
{
    return XMMATRIX{};
}

LightData Light::GetShaderData() const
{
    LightData data{};
    data.position = XMFLOAT4(m_position.x, m_position.y, m_position.z, static_cast<float>(m_type));
    data.direction = XMFLOAT4(m_direction.x, m_direction.y, m_direction.z, MathUtils::DegreesToRadians(m_spotAngle));
    data.color = XMFLOAT4(m_color.x, m_color.y, m_color.z, m_intensity);
    data.attenuation = XMFLOAT4(m_attenuation.x, m_attenuation.y, m_attenuation.z, m_range);
    data.shadowParams = XMFLOAT4(m_castShadows ? 1.0f : 0.0f, m_shadowBias, 0.0f, 0.0f);
    return data;
}

std::string Light::GetInfo() const
{
    std::stringstream ss;
    ss << "Light Type: " << static_cast<int>(m_type) << "\n";
    ss << "Position: (" << m_position.x << ", " << m_position.y << ", " << m_position.z << ")\n";
    ss << "Direction: (" << m_direction.x << ", " << m_direction.y << ", " << m_direction.z << ")\n";
    ss << "Color: (" << m_color.x << ", " << m_color.y << ", " << m_color.z << ")\n";
    ss << "Intensity: " << m_intensity << "\n";
    ss << "Range: " << m_range << "\n";
    ss << "Enabled: " << (m_enabled ? "Yes" : "No") << "\n";
    ss << "Cast Shadows: " << (m_castShadows ? "Yes" : "No") << "\n";
    return ss.str();
}

void Light::Console_SetProperty(const std::string& property, float value)
{
    if (property == "intensity")
        SetIntensity(value);
    else if (property == "range")
        SetRange(value);
    else if (property == "spotangle")
        SetSpotAngle(value);
    else if (property == "shadowbias")
        SetShadowBias(value);
}

void Light::Console_SetColor(float r, float g, float b)
{
    SetColor({std::max(0.0f, std::min(1.0f, r)), std::max(0.0f, std::min(1.0f, g)), std::max(0.0f, std::min(1.0f, b))});
}

// ============================================================================
// Utility functions
// ============================================================================

std::string LightTypeToString(LightType type)
{
    switch (type)
    {
    case LightType::Directional:
        return "directional";
    case LightType::Point:
        return "point";
    case LightType::Spot:
        return "spot";
    case LightType::Area:
        return "area";
    case LightType::Environment:
        return "environment";
    default:
        return "unknown";
    }
}

LightType StringToLightType(const std::string& str)
{
    using namespace Spark::HashLiterals;
    switch (Spark::FNV1a64(str))
    {
    case "directional"_hash64:
        return LightType::Directional;
    case "point"_hash64:
        return LightType::Point;
    case "spot"_hash64:
        return LightType::Spot;
    case "area"_hash64:
        return LightType::Area;
    case "environment"_hash64:
        return LightType::Environment;
    default:
        return LightType::Directional;
    }
}

std::string ShadowTechniqueToString(ShadowTechnique technique)
{
    switch (technique)
    {
    case ShadowTechnique::None:
        return "none";
    case ShadowTechnique::Basic:
        return "basic";
    case ShadowTechnique::PCF:
        return "pcf";
    case ShadowTechnique::VSM:
        return "vsm";
    case ShadowTechnique::CSM:
        return "csm";
    case ShadowTechnique::PCSS:
        return "pcss";
    default:
        return "unknown";
    }
}

ShadowTechnique StringToShadowTechnique(const std::string& str)
{
    using namespace Spark::HashLiterals;
    switch (Spark::FNV1a64(str))
    {
    case "none"_hash64:
        return ShadowTechnique::None;
    case "basic"_hash64:
        return ShadowTechnique::Basic;
    case "pcf"_hash64:
        return ShadowTechnique::PCF;
    case "vsm"_hash64:
        return ShadowTechnique::VSM;
    case "csm"_hash64:
        return ShadowTechnique::CSM;
    case "pcss"_hash64:
        return ShadowTechnique::PCSS;
    default:
        return ShadowTechnique::PCF;
    }
}


#endif // !SPARK_PLATFORM_WINDOWS
