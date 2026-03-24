/**
 * @file SkyAtmosphere.cpp
 * @brief Implementation of the Preetham analytical sky model
 * @author Spark Engine Team
 * @date 2026
 *
 * Coefficients sourced from Preetham et al. 1999, "A Practical Analytic Model for Daylight".
 * The Perez distribution function models the luminance at any point in the sky hemisphere
 * as a function of zenith angle, sun angle, and atmospheric turbidity.
 */

#include "SkyAtmosphere.h"
#include "../Utils/AngleUtils.h"

#include <algorithm>
#include <cmath>

namespace Spark::Graphics
{

    // =========================================================================
    // Constants
    // =========================================================================

    static constexpr float kPI = Spark::AngleUtils::PI;

    // =========================================================================
    // Initialization
    // =========================================================================

    bool SkyAtmosphereSystem::Initialize()
    {
        if (m_initialized)
        {
            return true;
        }

        RecalculateCoefficients();
        ComputeZenithValues();
        m_initialized = true;
        return true;
    }

    void SkyAtmosphereSystem::Shutdown()
    {
        m_initialized = false;
    }

    // =========================================================================
    // Settings
    // =========================================================================

    void SkyAtmosphereSystem::SetSettings(const SkySettings& settings)
    {
        m_settings = settings;
        m_settings.turbidity = std::clamp(m_settings.turbidity, 1.0f, 10.0f);
        RecalculateCoefficients();
        ComputeZenithValues();
    }

    void SkyAtmosphereSystem::SetSunDirection(const XMFLOAT3& dir)
    {
        m_settings.sunDirection = NormalizeDirection(dir);
        ComputeZenithValues();
    }

    void SkyAtmosphereSystem::SetTurbidity(float turbidity)
    {
        m_settings.turbidity = std::clamp(turbidity, 1.0f, 10.0f);
        RecalculateCoefficients();
        ComputeZenithValues();
    }

    // =========================================================================
    // Perez Coefficient Computation
    // =========================================================================

    void SkyAtmosphereSystem::RecalculateCoefficients()
    {
        float t = m_settings.turbidity;

        // Perez coefficients for luminance (Y) — from Preetham Table 1
        m_perezY.A = 0.1787f * t - 1.4630f;
        m_perezY.B = -0.3554f * t + 0.4275f;
        m_perezY.C = -0.0227f * t + 5.3251f;
        m_perezY.D = 0.1206f * t - 2.5771f;
        m_perezY.E = -0.0670f * t + 0.3703f;

        // Perez coefficients for chrominance x
        m_perezx.A = -0.0193f * t - 0.2592f;
        m_perezx.B = -0.0665f * t + 0.0008f;
        m_perezx.C = -0.0004f * t + 0.2125f;
        m_perezx.D = -0.0641f * t - 0.8989f;
        m_perezx.E = -0.0033f * t + 0.0452f;

        // Perez coefficients for chrominance y
        m_perezy.A = -0.0167f * t - 0.2608f;
        m_perezy.B = -0.0950f * t + 0.0092f;
        m_perezy.C = -0.0079f * t + 0.2102f;
        m_perezy.D = -0.0441f * t - 1.6537f;
        m_perezy.E = -0.0109f * t + 0.0529f;
    }

    // =========================================================================
    // Zenith Values
    // =========================================================================

    void SkyAtmosphereSystem::ComputeZenithValues()
    {
        XMFLOAT3 sunDir = NormalizeDirection(m_settings.sunDirection);

        // Sun zenith angle: angle between sun direction and up vector (0,1,0)
        // sunDir.y is the cosine of the zenith angle (since up = (0,1,0))
        float cosTheta = std::clamp(sunDir.y, -1.0f, 1.0f);
        m_sunTheta = std::acos(cosTheta);

        float t = m_settings.turbidity;
        float theta = m_sunTheta;
        float theta2 = theta * theta;
        float theta3 = theta2 * theta;
        float t2 = t * t;

        // Zenith luminance (Preetham Equation 5)
        float chi = (4.0f / 9.0f - t / 120.0f) * (kPI - 2.0f * theta);
        m_zenithY = (4.0453f * t - 4.9710f) * std::tan(chi) - 0.2155f * t + 2.4192f;
        m_zenithY = std::max(m_zenithY, 0.0f);

        // Zenith chrominance x (Preetham Table 2)
        m_zenithx = (0.00166f * theta3 - 0.00375f * theta2 + 0.00209f * theta + 0.0f) * t2 +
                    (-0.02903f * theta3 + 0.06377f * theta2 - 0.03202f * theta + 0.00394f) * t +
                    (0.11693f * theta3 - 0.21196f * theta2 + 0.06052f * theta + 0.25886f);

        // Zenith chrominance y (Preetham Table 2)
        m_zenithy = (0.00275f * theta3 - 0.00610f * theta2 + 0.00317f * theta + 0.0f) * t2 +
                    (-0.04214f * theta3 + 0.08970f * theta2 - 0.04153f * theta + 0.00516f) * t +
                    (0.15346f * theta3 - 0.26756f * theta2 + 0.06670f * theta + 0.26688f);
    }

    // =========================================================================
    // Perez Distribution Evaluation
    // =========================================================================

    float SkyAtmosphereSystem::EvaluatePerez(float theta, float gamma, const PerezCoefficients& c)
    {
        float cosGamma = std::cos(gamma);
        float cosTheta = std::cos(theta);

        // Clamp cosTheta to avoid division issues near the horizon
        cosTheta = std::max(cosTheta, 0.001f);

        // F(theta, gamma) = (1 + A * exp(B / cos(theta))) * (1 + C * exp(D * gamma) + E * cos^2(gamma))
        float term1 = 1.0f + c.A * std::exp(c.B / cosTheta);
        float term2 = 1.0f + c.C * std::exp(c.D * gamma) + c.E * cosGamma * cosGamma;

        return term1 * term2;
    }

    // =========================================================================
    // Sky Color Computation
    // =========================================================================

    SkyColor SkyAtmosphereSystem::ComputeSkyColor(const XMFLOAT3& viewDir) const
    {
        if (!m_settings.enabled)
        {
            return {0.0f, 0.0f, 0.0f};
        }

        XMFLOAT3 dir = NormalizeDirection(viewDir);

        // Clamp view direction above the horizon
        dir.y = std::max(dir.y, 0.001f);
        float viewTheta = std::acos(std::clamp(dir.y, 0.0f, 1.0f));

        // Angle between view direction and sun direction
        XMFLOAT3 sunDir = NormalizeDirection(m_settings.sunDirection);
        float cosGamma = dir.x * sunDir.x + dir.y * sunDir.y + dir.z * sunDir.z;
        cosGamma = std::clamp(cosGamma, -1.0f, 1.0f);
        float gamma = std::acos(cosGamma);

        // Evaluate the Perez function for the view direction
        float fViewY = EvaluatePerez(viewTheta, gamma, m_perezY);
        float fViewx = EvaluatePerez(viewTheta, gamma, m_perezx);
        float fViewy = EvaluatePerez(viewTheta, gamma, m_perezy);

        // Evaluate the Perez function at the zenith (reference normalization)
        float fZenithY = EvaluatePerez(0.0f, m_sunTheta, m_perezY);
        float fZenithx = EvaluatePerez(0.0f, m_sunTheta, m_perezx);
        float fZenithy = EvaluatePerez(0.0f, m_sunTheta, m_perezy);

        // Avoid division by zero
        float yxyY = (fZenithY > 0.001f) ? m_zenithY * (fViewY / fZenithY) : 0.0f;
        float yxyx = (fZenithx > 0.001f) ? m_zenithx * (fViewx / fZenithx) : 0.0f;
        float yxyy = (fZenithy > 0.001f) ? m_zenithy * (fViewy / fZenithy) : 0.0f;

        // Convert Yxy to XYZ
        float X = 0.0f;
        float Y = yxyY;
        float Z = 0.0f;

        if (yxyy > 0.001f)
        {
            X = (yxyx / yxyy) * Y;
            Z = ((1.0f - yxyx - yxyy) / yxyy) * Y;
        }

        // Convert XYZ to linear RGB (sRGB primaries, D65 whitepoint)
        float linearR = 3.2404542f * X - 1.5371385f * Y - 0.4985314f * Z;
        float linearG = -0.9692660f * X + 1.8760108f * Y + 0.0415560f * Z;
        float linearB = 0.0556434f * X - 0.2040259f * Y + 1.0572252f * Z;

        // Apply exposure and sun intensity
        float scale = m_settings.exposure * m_settings.sunIntensity * 0.01f;
        linearR = std::max(linearR * scale, 0.0f);
        linearG = std::max(linearG * scale, 0.0f);
        linearB = std::max(linearB * scale, 0.0f);

        return {linearR, linearG, linearB};
    }

    SkyColor SkyAtmosphereSystem::ComputeSunColor() const
    {
        if (!m_settings.enabled)
        {
            return {0.0f, 0.0f, 0.0f};
        }

        // The sun disc color is the sky color in the sun direction, amplified
        SkyColor sunSkyColor = ComputeSkyColor(m_settings.sunDirection);
        float boost = m_settings.sunIntensity * 0.5f;
        return sunSkyColor * boost;
    }

    SkyColor SkyAtmosphereSystem::GetZenithColor() const
    {
        return ComputeSkyColor({0.0f, 1.0f, 0.0f});
    }

    SkyColor SkyAtmosphereSystem::GetHorizonColor() const
    {
        // Sample near the horizon, slightly above to avoid clamping artifacts
        return ComputeSkyColor({0.0f, 0.01f, 1.0f});
    }

    // =========================================================================
    // Update
    // =========================================================================

    void SkyAtmosphereSystem::Update([[maybe_unused]] float deltaTime)
    {
        // Placeholder for time-of-day animation.
        // When linked to a DayNightCycle system, this would advance the sun
        // direction and recompute zenith values each frame.
    }

    // =========================================================================
    // Utility
    // =========================================================================

    XMFLOAT3 SkyAtmosphereSystem::NormalizeDirection(const XMFLOAT3& v)
    {
        float len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
        if (len < 0.0001f)
        {
            return {0.0f, 1.0f, 0.0f};
        }
        float inv = 1.0f / len;
        return {v.x * inv, v.y * inv, v.z * inv};
    }

} // namespace Spark::Graphics
