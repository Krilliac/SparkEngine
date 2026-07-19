/**
 * @file LightingSystemWindowsTypes.cpp
 * @brief Windows Light class implementation for LightingSystem
 *
 * Light split out of LightingSystemWindows.cpp, which keeps the
 * LightingSystem class lifecycle, per-frame update, data binding, and
 * shadow map rendering. The Linux counterpart lives in
 * LightingSystemLinuxTypes.cpp.
 */
#include "Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS

#include "LightingSystem.h"

#include <algorithm>
#include <sstream>
#include <string>

using namespace DirectX;

// ============================================================================
// LIGHT CLASS IMPLEMENTATION
// ============================================================================

Light::Light(LightType type) : m_type(type)
{
    // Initialize light based on type
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
    XMVECTOR position = XMLoadFloat3(&m_position);
    XMVECTOR direction = XMLoadFloat3(&m_direction);
    XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

    // Create look-at matrix for light
    XMVECTOR target = XMVectorAdd(position, direction);
    return XMMatrixLookAtLH(position, target, up);
}

XMMATRIX Light::GetShadowMatrix() const
{
    switch (m_type)
    {
    case LightType::Directional:
        return XMMatrixOrthographicLH(20.0f, 20.0f, 0.1f, 100.0f);
    case LightType::Point:
        return XMMatrixPerspectiveFovLH(XM_PIDIV2, 1.0f, 0.1f, m_range);
    case LightType::Spot:
        return XMMatrixPerspectiveFovLH(XMConvertToRadians(m_spotAngle), 1.0f, 0.1f, m_range);
    case LightType::Area:
        return XMMatrixPerspectiveFovLH(XM_PIDIV4, 1.0f, 0.1f, m_range);
    default:
        return XMMatrixIdentity();
    }
}

LightData Light::GetShaderData() const
{
    LightData data = {};

    data.position = XMFLOAT4(m_position.x, m_position.y, m_position.z, static_cast<float>(m_type));
    data.direction = XMFLOAT4(m_direction.x, m_direction.y, m_direction.z, XMConvertToRadians(m_spotAngle));
    data.color = XMFLOAT4(m_color.x, m_color.y, m_color.z, m_intensity);
    data.attenuation = XMFLOAT4(m_attenuation.x, m_attenuation.y, m_attenuation.z, m_range);
    data.shadowParams = XMFLOAT4(m_castShadows ? 1.0f : 0.0f, m_shadowBias, 0.0f, 0.0f);
    data.lightMatrix = GetLightMatrix();
    data.shadowMatrix = GetShadowMatrix();

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
    {
        SetIntensity(value);
    }
    else if (property == "range")
    {
        SetRange(value);
    }
    else if (property == "spotangle")
    {
        SetSpotAngle(value);
    }
    else if (property == "shadowbias")
    {
        SetShadowBias(value);
    }
}

void Light::Console_SetColor(float r, float g, float b)
{
    SetColor({std::max(0.0f, std::min(1.0f, r)), std::max(0.0f, std::min(1.0f, g)), std::max(0.0f, std::min(1.0f, b))});
}

#endif // SPARK_PLATFORM_WINDOWS
