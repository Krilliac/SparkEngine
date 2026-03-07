#include "Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS
/**
 * @file DecalSystem.cpp
 * @brief Decal system implementation — spawning, pooling, fading, rendering data
 */

#include "DecalSystem.h"
#include <sstream>
#include <cmath>
#include <cstdlib>
#include <algorithm>

using namespace DirectX;
namespace Spark::Graphics {

// ============================================================================
// Decal
// ============================================================================

XMMATRIX Decal::GetWorldMatrix() const {
    // Build OBB matrix from position, normal, tangent, and half extents
    XMVECTOR N = XMLoadFloat3(&normal);
    XMVECTOR T = XMLoadFloat3(&tangent);
    XMVECTOR B = XMVector3Cross(N, T);
    XMVECTOR P = XMLoadFloat3(&position);

    // Apply rotation around normal
    if (std::abs(rotation) > 0.001f) {
        XMMATRIX rot = XMMatrixRotationAxis(N, rotation);
        T = XMVector3TransformNormal(T, rot);
        B = XMVector3TransformNormal(B, rot);
    }

    // Scale by half extents
    T = XMVectorScale(T, halfExtents.x);
    B = XMVectorScale(B, halfExtents.y);
    N = XMVectorScale(N, halfExtents.z);

    XMMATRIX world = XMMATRIX(
        T,
        B,
        N,
        XMVectorSetW(P, 1.0f)
    );

    return world;
}

float Decal::GetCurrentOpacity() const {
    if (!active) return 0.0f;
    if (age < fadeTimer) return opacity;
    float fadeProgress = (age - fadeTimer) / fadeDuration;
    fadeProgress = (std::min)(1.0f, fadeProgress);
    return opacity * (1.0f - fadeProgress);
}

// ============================================================================
// DecalSystem
// ============================================================================

DecalSystem& DecalSystem::GetInstance() {
    static DecalSystem instance;
    return instance;
}

void DecalSystem::Initialize(uint32_t maxDecals) {
    m_maxDecals = maxDecals;
    m_decals.reserve(maxDecals);

    // Register default materials
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

    // Register default surface mappings
    RegisterSurfaceMapping({SurfaceType::Concrete, DecalType::BulletHole, "decal_bullet_hole", {0.05f, 0.15f}});
    RegisterSurfaceMapping({SurfaceType::Metal, DecalType::BulletHole, "decal_bullet_hole", {0.03f, 0.08f}});
    RegisterSurfaceMapping({SurfaceType::Wood, DecalType::BulletHole, "decal_bullet_hole", {0.04f, 0.12f}});
    RegisterSurfaceMapping({SurfaceType::Default, DecalType::ScorchMark, "decal_scorch", {0.3f, 0.8f}});
    RegisterSurfaceMapping({SurfaceType::Default, DecalType::BloodSplatter, "decal_blood", {0.2f, 0.5f}});
}

void DecalSystem::Update(float deltaTime) {
    for (auto& decal : m_decals) {
        if (!decal.active) continue;

        decal.age += deltaTime;

        // Check if fully faded
        if (decal.age >= decal.fadeTimer + decal.fadeDuration) {
            decal.active = false;
        }
    }
}

void DecalSystem::Shutdown() {
    m_decals.clear();
    m_materials.clear();
    m_surfaceMappings.clear();
}

Decal* DecalSystem::SpawnDecal(const XMFLOAT3& position, const XMFLOAT3& normal,
                                DecalType type, SurfaceType surface, float size) {
    // Find appropriate material from surface mapping
    std::string materialName;
    XMFLOAT2 sizeRange = {0.05f, 0.2f};
    float rotVariance = 3.14159f;

    for (const auto& mapping : m_surfaceMappings) {
        if (mapping.decalType == type &&
            (mapping.surface == surface || mapping.surface == SurfaceType::Default)) {
            materialName = mapping.materialName;
            sizeRange = mapping.sizeRange;
            rotVariance = mapping.rotationVariance;
            break;
        }
    }

    const DecalMaterial* mat = GetMaterial(materialName);
    Decal* decal = GetAvailableDecal();
    if (!decal) return nullptr;

    // Compute random size if not specified
    if (size < 0.0f) {
        float t = static_cast<float>(rand()) / RAND_MAX;
        size = sizeRange.x + t * (sizeRange.y - sizeRange.x);
    }

    decal->position = position;
    decal->normal = normal;
    decal->tangent = ComputeTangent(normal);
    decal->halfExtents = {size, size, size * 0.5f};
    decal->rotation = (static_cast<float>(rand()) / RAND_MAX - 0.5f) * 2.0f * rotVariance;
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
                                            const std::string& materialName, float size) {
    const DecalMaterial* mat = GetMaterial(materialName);
    Decal* decal = GetAvailableDecal();
    if (!decal) return nullptr;

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

uint32_t DecalSystem::GetActiveDecalCount() const {
    uint32_t count = 0;
    for (const auto& d : m_decals)
        if (d.active) count++;
    return count;
}

void DecalSystem::RegisterMaterial(const DecalMaterial& material) {
    m_materials[material.name] = material;
}

const DecalMaterial* DecalSystem::GetMaterial(const std::string& name) const {
    auto it = m_materials.find(name);
    return (it != m_materials.end()) ? &it->second : nullptr;
}

void DecalSystem::RegisterSurfaceMapping(const SurfaceDecalMapping& mapping) {
    m_surfaceMappings.push_back(mapping);
}

void DecalSystem::ClearAllDecals() {
    for (auto& d : m_decals) d.active = false;
}

Decal* DecalSystem::GetAvailableDecal() {
    // Find an inactive slot
    for (auto& d : m_decals) {
        if (!d.active) return &d;
    }

    // Pool not full? Add one
    if (m_decals.size() < m_maxDecals) {
        m_decals.push_back({});
        return &m_decals.back();
    }

    // Pool full — recycle oldest
    FadeOldestDecal();
    for (auto& d : m_decals) {
        if (!d.active) return &d;
    }

    // Force recycle first
    m_decals[0].active = false;
    return &m_decals[0];
}

void DecalSystem::FadeOldestDecal() {
    float oldestAge = -1.0f;
    Decal* oldest = nullptr;
    for (auto& d : m_decals) {
        if (d.active && d.age > oldestAge) {
            oldestAge = d.age;
            oldest = &d;
        }
    }
    if (oldest) oldest->active = false;
}

XMFLOAT3 DecalSystem::ComputeTangent(const XMFLOAT3& normal) const {
    XMVECTOR n = XMLoadFloat3(&normal);
    XMVECTOR up = XMVectorSet(0, 1, 0, 0);

    // If normal is nearly up, use forward instead
    float dot = std::abs(XMVectorGetX(XMVector3Dot(n, up)));
    if (dot > 0.99f) up = XMVectorSet(0, 0, 1, 0);

    XMVECTOR tangent = XMVector3Normalize(XMVector3Cross(up, n));
    XMFLOAT3 result;
    XMStoreFloat3(&result, tangent);
    return result;
}

std::string DecalSystem::Console_GetStatus() const {
    std::ostringstream ss;
    ss << "=== Decal System ===\n";
    ss << "Active: " << GetActiveDecalCount() << "/" << m_decals.size()
       << " (Max: " << m_maxDecals << ")\n";
    ss << "Materials: " << m_materials.size() << "\n";
    ss << "Surface Mappings: " << m_surfaceMappings.size() << "\n";
    ss << "Fade Time: " << m_defaultFadeTime << "s + " << m_defaultFadeDuration << "s fade\n";
    return ss.str();
}

void DecalSystem::Console_SetMaxDecals(uint32_t max) {
    m_maxDecals = max;
}

void DecalSystem::Console_SpawnTestDecal(float x, float y, float z) {
    SpawnDecal({x, y, z}, {0, 1, 0}, DecalType::BulletHole, SurfaceType::Concrete, 0.15f);
}

} // namespace Spark::Graphics

#endif // SPARK_PLATFORM_WINDOWS
