/**
 * @file FogSystem.cpp
 * @brief Implementation of distance fog, height fog, and volumetric fog rendering
 */

#include "FogSystem.h"
#include "../Utils/LogMacros.h"
#include "../Utils/SparkConsole.h"

#include <algorithm>
#include <cmath>

namespace Spark
{
    namespace Graphics
    {

        // ============================================================================
        // Lifecycle
        // ============================================================================

        bool FogSystem::Initialize()
        {
            m_initialized = true;
            SPARK_LOG_INFO(Spark::LogCategory::Graphics, "FogSystem initialized");
            return true;
        }

        void FogSystem::Shutdown()
        {
            SPARK_LOG_INFO(Spark::LogCategory::Graphics, "FogSystem shutting down");
            m_initialized = false;
        }

        // ============================================================================
        // Mode and Settings
        // ============================================================================

        void FogSystem::SetMode(FogMode mode)
        {
            if (m_mode != mode)
            {
                static const char* modeNames[] = {"None",   "Linear",    "Exponential", "ExpSquared",
                                                  "Height", "ExpHeight", "Volumetric"};
                SPARK_LOG_INFO(Spark::LogCategory::Graphics, "FogSystem mode changed: %s -> %s",
                               modeNames[static_cast<int>(m_mode)], modeNames[static_cast<int>(mode)]);
            }
            m_mode = mode;
        }

        void FogSystem::SetDensity(float density)
        {
            m_exponentialSettings.density = std::max(density, 0.0f);
            m_heightSettings.baseDensity = std::max(density, 0.0f);
        }

        void FogSystem::SetLinearRange(float start, float end)
        {
            m_linearSettings.start = start;
            m_linearSettings.end = std::max(end, start + 0.01f);
        }

        // ============================================================================
        // Fog Computation
        // ============================================================================

        float FogSystem::ComputeFogFactor(float distance) const
        {
            if (!m_enabled || m_mode == FogMode::None)
            {
                return 0.0f;
            }

            switch (m_mode)
            {
            case FogMode::Linear:
                return FogMath::LinearFog(distance, m_linearSettings.start, m_linearSettings.end);
            case FogMode::Exponential:
                return FogMath::ExponentialFog(distance, m_exponentialSettings.density);
            case FogMode::ExponentialSquared:
                return FogMath::ExponentialSquaredFog(distance, m_exponentialSettings.density);
            default:
                return FogMath::ExponentialFog(distance, m_exponentialSettings.density);
            }
        }

        float FogSystem::ComputeHeightFogFactor(float cameraY, float fragmentY, float distance) const
        {
            if (!m_enabled)
            {
                return 0.0f;
            }

            if (m_mode == FogMode::Height || m_mode == FogMode::ExponentialHeight)
            {
                return FogMath::IntegratedHeightFog(cameraY, fragmentY, distance, m_heightSettings.baseHeight,
                                                    m_heightSettings.heightFalloff, m_heightSettings.baseDensity);
            }

            return ComputeFogFactor(distance);
        }

        FogColor FogSystem::ApplyFog(const FogColor& sceneColor, float fogFactor) const
        {
            float f = std::clamp(fogFactor, 0.0f, 1.0f);
            return sceneColor.Lerp(m_color, f);
        }

        // ============================================================================
        // Metrics and Console
        // ============================================================================

        FogMetrics FogSystem::GetMetrics() const
        {
            FogMetrics m;
            m.activeMode = static_cast<int>(m_mode);
            m.currentDensity = m_exponentialSettings.density;
            m.maxFogDistance = (m_mode == FogMode::Linear) ? m_linearSettings.end : m_volumetricSettings.maxDistance;
            m.volumetricSamples = (m_mode == FogMode::Volumetric) ? m_volumetricSettings.rayMarchSteps : 0;
            return m;
        }

        std::string FogSystem::Console_GetStatus() const
        {
            static const char* modeNames[] = {"None",   "Linear",    "Exponential", "ExpSquared",
                                              "Height", "ExpHeight", "Volumetric"};
            std::string s = "Fog System:\n";
            s += "  Enabled: " + std::string(m_enabled ? "yes" : "no") + "\n";
            s += "  Mode: " + std::string(modeNames[static_cast<int>(m_mode)]) + "\n";
            s += "  Color: (" + std::to_string(m_color.r) + ", " + std::to_string(m_color.g) + ", " +
                 std::to_string(m_color.b) + ")\n";
            if (m_mode == FogMode::Linear)
            {
                s += "  Range: " + std::to_string(m_linearSettings.start) + " - " +
                     std::to_string(m_linearSettings.end) + "\n";
            }
            else
            {
                s += "  Density: " + std::to_string(m_exponentialSettings.density) + "\n";
            }
            if (m_mode == FogMode::Height || m_mode == FogMode::ExponentialHeight)
            {
                s += "  Base height: " + std::to_string(m_heightSettings.baseHeight) + "\n";
                s += "  Height falloff: " + std::to_string(m_heightSettings.heightFalloff) + "\n";
            }
            return s;
        }

    } // namespace Graphics
} // namespace Spark
