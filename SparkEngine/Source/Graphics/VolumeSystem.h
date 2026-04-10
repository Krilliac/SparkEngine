/**
 * @file VolumeSystem.h
 * @brief Spatial parameter blending system (Unity-inspired Volume framework)
 * @author Spark Engine Team
 * @date 2025
 *
 * Volumes define spatial regions where rendering/gameplay parameters are
 * overridden. As the camera moves through volumes, parameters blend smoothly
 * based on weight, priority, and distance. Supports global (always-active)
 * and local (bounded region with blend distance) volumes.
 *
 * @see PostProcessingPipeline.h, FogSystem.h
 *
 * @note **Intentional reusable graphics utility** — Unity-style post-process volume blending (weighted profile blending by camera position). A future post-process stack will instantiate this per scene and query `GetBlendedProfile()` each frame. Covered by `Tests/TestGraphicsIntegration.cpp` against its public API so the blend math stays valid even without a live PP stack.
 *
 */

#pragma once

#include "../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Spark::Graphics
{

    // =========================================================================
    // Volume Parameter Types
    // =========================================================================

    /**
     * @brief Parameter categories that volumes can override
     */
    enum class VolumeParameterType : uint8_t
    {
        Exposure,
        Bloom,
        Tonemapping,
        ColorGrading,
        Fog,
        SSAO,
        Vignette,
        ChromaticAberration,
        DepthOfField,
        MotionBlur,
        FilmGrain,
        Custom,
        Count
    };

    /**
     * @brief A single overridable float parameter within a volume component
     */
    struct VolumeParameter
    {
        float value = 0.0f;
        bool overrideState = false; ///< Only blend this param if true
    };

    // =========================================================================
    // Volume Components (blendable parameter sets)
    // =========================================================================

    /**
     * @brief Base class for volume parameter groups
     */
    struct VolumeComponent
    {
        VolumeParameterType type = VolumeParameterType::Custom;
        bool active = true;

        virtual ~VolumeComponent() = default;

        /** @brief Interpolate all overridden parameters toward target */
        virtual void Interpolate(const VolumeComponent& target, float t) = 0;
    };

    /**
     * @brief Exposure/auto-exposure volume parameters
     */
    struct ExposureVolumeComponent : public VolumeComponent
    {
        VolumeParameter fixedExposure = {0.0f, false};
        VolumeParameter compensationEV = {0.0f, false};
        VolumeParameter minEV = {-2.0f, false};
        VolumeParameter maxEV = {14.0f, false};
        VolumeParameter adaptationSpeedUp = {3.0f, false};
        VolumeParameter adaptationSpeedDown = {1.0f, false};

        ExposureVolumeComponent() { type = VolumeParameterType::Exposure; }

        void Interpolate(const VolumeComponent& target, float t) override
        {
            const auto& other = static_cast<const ExposureVolumeComponent&>(target);
            if (other.fixedExposure.overrideState)
                fixedExposure.value = std::lerp(fixedExposure.value, other.fixedExposure.value, t);
            if (other.compensationEV.overrideState)
                compensationEV.value = std::lerp(compensationEV.value, other.compensationEV.value, t);
            if (other.minEV.overrideState)
                minEV.value = std::lerp(minEV.value, other.minEV.value, t);
            if (other.maxEV.overrideState)
                maxEV.value = std::lerp(maxEV.value, other.maxEV.value, t);
            if (other.adaptationSpeedUp.overrideState)
                adaptationSpeedUp.value = std::lerp(adaptationSpeedUp.value, other.adaptationSpeedUp.value, t);
            if (other.adaptationSpeedDown.overrideState)
                adaptationSpeedDown.value = std::lerp(adaptationSpeedDown.value, other.adaptationSpeedDown.value, t);
        }
    };

    /**
     * @brief Bloom volume parameters
     */
    struct BloomVolumeComponent : public VolumeComponent
    {
        VolumeParameter intensity = {1.0f, false};
        VolumeParameter threshold = {0.9f, false};
        VolumeParameter softKnee = {0.5f, false};
        VolumeParameter scatter = {0.7f, false};
        VolumeParameter anamorphicRatio = {0.0f, false};

        BloomVolumeComponent() { type = VolumeParameterType::Bloom; }

        void Interpolate(const VolumeComponent& target, float t) override
        {
            const auto& other = static_cast<const BloomVolumeComponent&>(target);
            if (other.intensity.overrideState)
                intensity.value = std::lerp(intensity.value, other.intensity.value, t);
            if (other.threshold.overrideState)
                threshold.value = std::lerp(threshold.value, other.threshold.value, t);
            if (other.softKnee.overrideState)
                softKnee.value = std::lerp(softKnee.value, other.softKnee.value, t);
            if (other.scatter.overrideState)
                scatter.value = std::lerp(scatter.value, other.scatter.value, t);
            if (other.anamorphicRatio.overrideState)
                anamorphicRatio.value = std::lerp(anamorphicRatio.value, other.anamorphicRatio.value, t);
        }
    };

    /**
     * @brief Color grading volume parameters
     */
    struct ColorGradingVolumeComponent : public VolumeComponent
    {
        // Lift / Gamma / Gain (shadows / midtones / highlights)
        VolumeParameter liftR = {0.0f, false};
        VolumeParameter liftG = {0.0f, false};
        VolumeParameter liftB = {0.0f, false};
        VolumeParameter gammaR = {1.0f, false};
        VolumeParameter gammaG = {1.0f, false};
        VolumeParameter gammaB = {1.0f, false};
        VolumeParameter gainR = {1.0f, false};
        VolumeParameter gainG = {1.0f, false};
        VolumeParameter gainB = {1.0f, false};

        VolumeParameter saturation = {1.0f, false};
        VolumeParameter contrast = {1.0f, false};
        VolumeParameter temperature = {0.0f, false}; ///< Color temperature shift in Kelvin offset
        VolumeParameter tint = {0.0f, false};        ///< Green-magenta tint shift

        ColorGradingVolumeComponent() { type = VolumeParameterType::ColorGrading; }

        void Interpolate(const VolumeComponent& target, float t) override
        {
            const auto& other = static_cast<const ColorGradingVolumeComponent&>(target);
            if (other.liftR.overrideState)
                liftR.value = std::lerp(liftR.value, other.liftR.value, t);
            if (other.liftG.overrideState)
                liftG.value = std::lerp(liftG.value, other.liftG.value, t);
            if (other.liftB.overrideState)
                liftB.value = std::lerp(liftB.value, other.liftB.value, t);
            if (other.gammaR.overrideState)
                gammaR.value = std::lerp(gammaR.value, other.gammaR.value, t);
            if (other.gammaG.overrideState)
                gammaG.value = std::lerp(gammaG.value, other.gammaG.value, t);
            if (other.gammaB.overrideState)
                gammaB.value = std::lerp(gammaB.value, other.gammaB.value, t);
            if (other.gainR.overrideState)
                gainR.value = std::lerp(gainR.value, other.gainR.value, t);
            if (other.gainG.overrideState)
                gainG.value = std::lerp(gainG.value, other.gainG.value, t);
            if (other.gainB.overrideState)
                gainB.value = std::lerp(gainB.value, other.gainB.value, t);
            if (other.saturation.overrideState)
                saturation.value = std::lerp(saturation.value, other.saturation.value, t);
            if (other.contrast.overrideState)
                contrast.value = std::lerp(contrast.value, other.contrast.value, t);
            if (other.temperature.overrideState)
                temperature.value = std::lerp(temperature.value, other.temperature.value, t);
            if (other.tint.overrideState)
                tint.value = std::lerp(tint.value, other.tint.value, t);
        }
    };

    /**
     * @brief Fog volume parameters
     */
    struct FogVolumeComponent : public VolumeComponent
    {
        VolumeParameter density = {0.01f, false};
        VolumeParameter heightFalloff = {0.2f, false};
        VolumeParameter maxOpacity = {1.0f, false};
        VolumeParameter startDistance = {0.0f, false};

        FogVolumeComponent() { type = VolumeParameterType::Fog; }

        void Interpolate(const VolumeComponent& target, float t) override
        {
            const auto& other = static_cast<const FogVolumeComponent&>(target);
            if (other.density.overrideState)
                density.value = std::lerp(density.value, other.density.value, t);
            if (other.heightFalloff.overrideState)
                heightFalloff.value = std::lerp(heightFalloff.value, other.heightFalloff.value, t);
            if (other.maxOpacity.overrideState)
                maxOpacity.value = std::lerp(maxOpacity.value, other.maxOpacity.value, t);
            if (other.startDistance.overrideState)
                startDistance.value = std::lerp(startDistance.value, other.startDistance.value, t);
        }
    };

    // =========================================================================
    // Volume Definition
    // =========================================================================

    /**
     * @brief Defines a spatial region with parameter overrides
     */
    struct Volume
    {
        std::string name;
        bool isGlobal = false; ///< Global volumes always apply (no bounds)
        int priority = 0;      ///< Higher priority wins on overlap
        float weight = 1.0f;   ///< Blend weight [0, 1]

        // Local volume bounds (AABB in world space)
        XMFLOAT3 boundsMin = {0.0f, 0.0f, 0.0f};
        XMFLOAT3 boundsMax = {10.0f, 10.0f, 10.0f};
        float blendDistance = 2.0f; ///< Distance over which to fade in

        // Parameter overrides for this volume
        std::vector<std::unique_ptr<VolumeComponent>> components;

        /** @brief Get a component by type, or nullptr */
        VolumeComponent* GetComponent(VolumeParameterType type) const
        {
            for (const auto& comp : components)
            {
                if (comp->type == type && comp->active)
                    return comp.get();
            }
            return nullptr;
        }

        /** @brief Add a component to this volume. Returns raw pointer for configuration. */
        template <typename T> T* AddComponent()
        {
            auto comp = std::make_unique<T>();
            T* raw = comp.get();
            components.push_back(std::move(comp));
            return raw;
        }

        /** @brief Calculate blend factor for a world position [0=outside, 1=fully inside] */
        float CalculateBlendFactor(const XMFLOAT3& position) const
        {
            if (isGlobal)
                return weight;

            // Signed distance from AABB (negative = inside)
            float dx = std::max({boundsMin.x - position.x, position.x - boundsMax.x, 0.0f});
            float dy = std::max({boundsMin.y - position.y, position.y - boundsMax.y, 0.0f});
            float dz = std::max({boundsMin.z - position.z, position.z - boundsMax.z, 0.0f});
            float distance = std::sqrt(dx * dx + dy * dy + dz * dz);

            if (distance > blendDistance)
                return 0.0f;
            if (distance <= 0.0f)
                return weight;

            // Smooth blend over the blend distance
            float t = 1.0f - (distance / blendDistance);
            return t * t * (3.0f - 2.0f * t) * weight; // Smoothstep
        }
    };

    // =========================================================================
    // Volume Stack (blended result)
    // =========================================================================

    /**
     * @brief Holds the current blended state of all volume parameters
     *
     * This is the "resolved" output — what the rendering systems actually read.
     */
    struct VolumeStack
    {
        ExposureVolumeComponent exposure;
        BloomVolumeComponent bloom;
        ColorGradingVolumeComponent colorGrading;
        FogVolumeComponent fog;
    };

    // =========================================================================
    // Volume Manager
    // =========================================================================

    /**
     * @brief Manages volumes and evaluates blended parameters per frame
     *
     * Volumes are registered by name. Each frame, Update() evaluates all active
     * volumes at the camera position, blending their parameters by priority and
     * weight into the VolumeStack.
     */
    class VolumeManager
    {
      public:
        VolumeManager() = default;
        ~VolumeManager() = default;

        void Initialize()
        {
            m_volumes.clear();
            m_stack = {};
            m_initialized = true;
        }

        void Shutdown()
        {
            m_volumes.clear();
            m_initialized = false;
        }

        /** @brief Register a named volume. Returns pointer for configuration. */
        Volume* CreateVolume(const std::string& name)
        {
            auto vol = std::make_unique<Volume>();
            vol->name = name;
            Volume* raw = vol.get();
            m_volumes.push_back(std::move(vol));
            return raw;
        }

        /** @brief Remove a volume by name */
        void RemoveVolume(const std::string& name)
        {
            m_volumes.erase(
                std::remove_if(m_volumes.begin(), m_volumes.end(), [&](const auto& v) { return v->name == name; }),
                m_volumes.end());
        }

        /** @brief Find a volume by name */
        Volume* FindVolume(const std::string& name) const
        {
            for (const auto& v : m_volumes)
            {
                if (v->name == name)
                    return v.get();
            }
            return nullptr;
        }

        /**
         * @brief Evaluate all volumes at the given camera position
         *
         * Blends parameters from all active volumes, sorted by priority.
         * Higher-priority volumes override lower-priority ones.
         */
        void Update(const XMFLOAT3& cameraPosition)
        {
            if (!m_initialized)
                return;

            // Reset stack to defaults
            m_stack = {};

            // Collect active volumes with their blend factors, sorted by priority
            struct ActiveVolume
            {
                Volume* volume;
                float blendFactor;
            };
            std::vector<ActiveVolume> active;
            active.reserve(m_volumes.size());

            for (const auto& vol : m_volumes)
            {
                float factor = vol->CalculateBlendFactor(cameraPosition);
                if (factor > 0.001f)
                {
                    active.push_back({vol.get(), factor});
                }
            }

            // Sort by priority (lowest first so highest overwrites)
            std::sort(active.begin(), active.end(), [](const ActiveVolume& a, const ActiveVolume& b)
                      { return a.volume->priority < b.volume->priority; });

            // Blend each active volume into the stack
            for (const auto& av : active)
            {
                float t = av.blendFactor;

                for (const auto& comp : av.volume->components)
                {
                    if (!comp->active)
                        continue;

                    switch (comp->type)
                    {
                    case VolumeParameterType::Exposure:
                        m_stack.exposure.Interpolate(*comp, t);
                        break;
                    case VolumeParameterType::Bloom:
                        m_stack.bloom.Interpolate(*comp, t);
                        break;
                    case VolumeParameterType::ColorGrading:
                        m_stack.colorGrading.Interpolate(*comp, t);
                        break;
                    case VolumeParameterType::Fog:
                        m_stack.fog.Interpolate(*comp, t);
                        break;
                    default:
                        break;
                    }
                }
            }
        }

        /** @brief Get the current blended parameter stack */
        const VolumeStack& GetStack() const { return m_stack; }
        VolumeStack& GetStack() { return m_stack; }

        /** @brief Get volume count */
        size_t GetVolumeCount() const { return m_volumes.size(); }

        bool IsInitialized() const { return m_initialized; }

        std::string Console_GetStatus() const
        {
            std::string result = "VolumeManager: " + std::to_string(m_volumes.size()) + " volumes\n";
            for (const auto& v : m_volumes)
            {
                result += "  " + v->name + (v->isGlobal ? " [GLOBAL]" : " [LOCAL]") +
                          " pri=" + std::to_string(v->priority) +
                          " components=" + std::to_string(v->components.size()) + "\n";
            }
            return result;
        }

      private:
        std::vector<std::unique_ptr<Volume>> m_volumes;
        VolumeStack m_stack;
        bool m_initialized = false;
    };

} // namespace Spark::Graphics
