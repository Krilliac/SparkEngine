/**
 * @file DecalSystemLinux.cpp
 * @brief Linux implementation — split from DecalSystem.cpp
 */
#include "Core/Platform.h"
#ifndef SPARK_PLATFORM_WINDOWS


#include "DecalSystem.h"
#include "../Utils/Validate.h"
#include <sstream>
#include <cmath>
#include <cstdlib>
#include <algorithm>

namespace Spark::Graphics
{

    // ============================================================================
    // Decal (Linux stub)
    // ============================================================================

    XMMATRIX Decal::GetWorldMatrix() const
    {
        XMMATRIX m;
        memset(&m, 0, sizeof(m));
        return m;
    }

    float Decal::GetCurrentOpacity() const
    {
        if (!active || !std::isfinite(opacity) || !std::isfinite(age) || !std::isfinite(fadeTimer))
            return 0.0f;
        const float baseOpacity = std::clamp(opacity, 0.0f, 1.0f);
        if (age < fadeTimer)
            return baseOpacity;
        if (!std::isfinite(fadeDuration) || fadeDuration <= 0.0f)
            return 0.0f;
        const float fadeProgress = std::clamp((age - fadeTimer) / fadeDuration, 0.0f, 1.0f);
        return baseOpacity * (1.0f - fadeProgress);
    }

    // ============================================================================
    // DecalSystem (Linux stub)
    // ============================================================================

    DecalSystem& DecalSystem::GetInstance()
    {
        static DecalSystem instance;
        return instance;
    }

    void DecalSystem::Initialize(uint32_t maxDecals)
    {
        SPARK_LOG_INFO(Spark::LogCategory::Graphics, "DecalSystem initializing with maxDecals=%u", maxDecals);
        m_maxDecals = maxDecals;
        m_decals.reserve(maxDecals);

        DecalMaterial bulletHole;
        bulletHole.name = "decal_bullet_hole";
        bulletHole.albedoTexture = "Textures/Decals/bullet_hole.dds";
        bulletHole.normalTexture = "Textures/Decals/bullet_hole_normal.dds";
        RegisterMaterial(bulletHole);

        DecalMaterial scorchMark;
        scorchMark.name = "decal_scorch";
        scorchMark.albedoTexture = "Textures/Decals/scorch_mark.dds";
        scorchMark.affectsNormals = false;
        RegisterMaterial(scorchMark);

        DecalMaterial bloodSplatter;
        bloodSplatter.name = "decal_blood";
        bloodSplatter.albedoTexture = "Textures/Decals/blood_splatter.dds";
        bloodSplatter.affectsNormals = false;
        RegisterMaterial(bloodSplatter);

        RegisterSurfaceMapping({SurfaceType::Concrete, DecalType::BulletHole, "decal_bullet_hole", {0.05f, 0.15f}});
        RegisterSurfaceMapping({SurfaceType::Metal, DecalType::BulletHole, "decal_bullet_hole", {0.03f, 0.08f}});
        RegisterSurfaceMapping({SurfaceType::Wood, DecalType::BulletHole, "decal_bullet_hole", {0.04f, 0.12f}});
        RegisterSurfaceMapping({SurfaceType::Default, DecalType::ScorchMark, "decal_scorch", {0.3f, 0.8f}});
        RegisterSurfaceMapping({SurfaceType::Default, DecalType::BloodSplatter, "decal_blood", {0.2f, 0.5f}});
    }

    void DecalSystem::Update(float deltaTime)
    {
        for (auto& decal : m_decals)
        {
            if (!decal.active)
                continue;
            decal.age += deltaTime;
            if (decal.age >= decal.fadeTimer + decal.fadeDuration)
            {
                decal.active = false;
            }
        }
    }

    void DecalSystem::Shutdown()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Graphics, "DecalSystem shutting down");
        m_decals.clear();
        m_materials.clear();
        m_surfaceMappings.clear();
    }

    Decal* DecalSystem::SpawnDecal(const XMFLOAT3& position, const XMFLOAT3& normal, DecalType type,
                                   SurfaceType surface, float size)
    {
        std::string materialName;
        XMFLOAT2 sizeRange = {0.05f, 0.2f};
        float rotVariance = MathUtils::PI;

        for (const auto& mapping : m_surfaceMappings)
        {
            if (mapping.decalType == type && (mapping.surface == surface || mapping.surface == SurfaceType::Default))
            {
                materialName = mapping.materialName;
                sizeRange = mapping.sizeRange;
                rotVariance = mapping.rotationVariance;
                break;
            }
        }

        const DecalMaterial* mat = GetMaterial(materialName);
        Decal* decal = GetAvailableDecal();
        if (!decal)
            return nullptr;

        if (size < 0.0f)
        {
            float t = static_cast<float>(rand()) / static_cast<float>(RAND_MAX);
            size = sizeRange.x + t * (sizeRange.y - sizeRange.x);
        }

        decal->position = position;
        decal->normal = normal;
        decal->tangent = ComputeTangent(normal);
        decal->halfExtents = {size, size, size * 0.5f};
        decal->rotation = (static_cast<float>(rand()) / static_cast<float>(RAND_MAX) - 0.5f) * 2.0f * rotVariance;
        decal->opacity = 1.0f;
        decal->fadeTimer = m_defaultFadeTime;
        decal->fadeDuration = m_defaultFadeDuration;
        decal->age = 0.0f;
        decal->type = type;
        decal->surface = surface;
        decal->material = mat;
        decal->active = true;

        return decal;
    }

    Decal* DecalSystem::SpawnDecalWithMaterial(const XMFLOAT3& position, const XMFLOAT3& normal,
                                               const std::string& materialName, float size)
    {
        const DecalMaterial* mat = GetMaterial(materialName);
        Decal* decal = GetAvailableDecal();
        if (!decal)
            return nullptr;

        decal->position = position;
        decal->normal = normal;
        decal->tangent = ComputeTangent(normal);
        decal->halfExtents = {size, size, size * 0.5f};
        decal->rotation = 0.0f;
        decal->opacity = 1.0f;
        decal->fadeTimer = m_defaultFadeTime;
        decal->fadeDuration = m_defaultFadeDuration;
        decal->age = 0.0f;
        decal->type = DecalType::Custom;
        decal->surface = SurfaceType::Default;
        decal->material = mat;
        decal->active = true;

        return decal;
    }

    uint32_t DecalSystem::GetActiveDecalCount() const
    {
        uint32_t count = 0;
        for (const auto& d : m_decals)
            if (d.active)
                count++;
        return count;
    }

    void DecalSystem::RegisterMaterial(const DecalMaterial& material)
    {
        m_materials[material.name] = material;
    }

    const DecalMaterial* DecalSystem::GetMaterial(const std::string& name) const
    {
        auto it = m_materials.find(name);
        return (it != m_materials.end()) ? &it->second : nullptr;
    }

    void DecalSystem::RegisterSurfaceMapping(const SurfaceDecalMapping& mapping)
    {
        m_surfaceMappings.push_back(mapping);
    }

    void DecalSystem::ClearAllDecals()
    {
        for (auto& d : m_decals)
            d.active = false;
    }

    Decal* DecalSystem::GetAvailableDecal()
    {
        for (auto& d : m_decals)
        {
            if (!d.active)
                return &d;
        }
        if (m_decals.size() < m_maxDecals)
        {
            m_decals.push_back({});
            return &m_decals.back();
        }
        FadeOldestDecal();
        for (auto& d : m_decals)
        {
            if (!d.active)
                return &d;
        }
        m_decals[0].active = false;
        return &m_decals[0];
    }

    void DecalSystem::FadeOldestDecal()
    {
        float oldestAge = -1.0f;
        Decal* oldest = nullptr;
        for (auto& d : m_decals)
        {
            if (d.active && d.age > oldestAge)
            {
                oldestAge = d.age;
                oldest = &d;
            }
        }
        if (oldest)
            oldest->active = false;
    }

    XMFLOAT3 DecalSystem::ComputeTangent(const XMFLOAT3& normal) const
    {
        // Simple tangent computation without DirectXMath intrinsics
        XMFLOAT3 up = {0.0f, 1.0f, 0.0f};
        float dot = std::abs(normal.x * up.x + normal.y * up.y + normal.z * up.z);
        if (dot > 0.99f)
            up = {0.0f, 0.0f, 1.0f};

        // Cross product: up x normal
        XMFLOAT3 tangent;
        tangent.x = up.y * normal.z - up.z * normal.y;
        tangent.y = up.z * normal.x - up.x * normal.z;
        tangent.z = up.x * normal.y - up.y * normal.x;

        // Normalize
        float len = std::sqrt(tangent.x * tangent.x + tangent.y * tangent.y + tangent.z * tangent.z);
        if (len > 0.0001f)
        {
            tangent.x /= len;
            tangent.y /= len;
            tangent.z /= len;
        }
        return tangent;
    }

    std::string DecalSystem::Console_GetStatus() const
    {
        std::ostringstream ss;
        ss << "=== Decal System ===\n";
        ss << "Active: " << GetActiveDecalCount() << "/" << m_decals.size() << " (Max: " << m_maxDecals << ")\n";
        ss << "Materials: " << m_materials.size() << "\n";
        ss << "Surface Mappings: " << m_surfaceMappings.size() << "\n";
        ss << "Fade Time: " << m_defaultFadeTime << "s + " << m_defaultFadeDuration << "s fade\n";
        return ss.str();
    }

    void DecalSystem::Console_SetMaxDecals(uint32_t max)
    {
        m_maxDecals = max;
    }

    void DecalSystem::Console_SpawnTestDecal(float x, float y, float z)
    {
        SpawnDecal({x, y, z}, {0, 1, 0}, DecalType::BulletHole, SurfaceType::Concrete, 0.15f);
    }

} // namespace Spark::Graphics


#endif // !SPARK_PLATFORM_WINDOWS
