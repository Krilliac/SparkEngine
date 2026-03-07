/**
 * @file DecalSystem.h
 * @brief Projected decal system for bullet holes, scorch marks, and environmental detail
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides deferred decal projection for FPS visual feedback:
 * - Screen-space or OBB-projected decals
 * - Decal materials (albedo, normal, roughness)
 * - Decal pooling with fade-out
 * - Surface-type mapping (different decals for metal/concrete/wood)
 * - Integration with weapon system (spawn at raycast hit point)
 */

#pragma once
#include "../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <cstdint>


namespace Spark::Graphics {

// ============================================================================
// Decal Types
// ============================================================================

enum class SurfaceType : uint8_t {
    Default,
    Concrete,
    Metal,
    Wood,
    Glass,
    Dirt,
    Flesh,
    Water,
    Foliage,
    Count
};

enum class DecalType : uint8_t {
    BulletHole,
    ScorchMark,
    BloodSplatter,
    Footprint,
    TireTrack,
    Crack,
    Custom
};

// ============================================================================
// Decal Data
// ============================================================================

struct DecalMaterial {
    std::string name;
    std::string albedoTexture;
    std::string normalTexture;
    std::string roughnessTexture;
    float opacity = 1.0f;
    bool affectsNormals = true;
    bool affectsRoughness = true;
};

struct Decal {
    XMFLOAT3 position;
    XMFLOAT3 normal;          ///< Surface normal at hit point
    XMFLOAT3 tangent;         ///< Tangent direction (for alignment)
    XMFLOAT3 halfExtents;     ///< Size of the decal OBB
    float rotation;            ///< Random rotation around normal (radians)
    float opacity;
    float fadeTimer;           ///< Time remaining before fade starts
    float fadeDuration;        ///< How long to fade out
    float age;                 ///< Time since spawn
    DecalType type;
    SurfaceType surface;
    const DecalMaterial* material;
    bool active;

    XMMATRIX GetWorldMatrix() const;
    float GetCurrentOpacity() const;
};

// ============================================================================
// Surface-Decal Mapping
// ============================================================================

struct SurfaceDecalMapping {
    SurfaceType surface;
    DecalType decalType;
    std::string materialName;
    XMFLOAT2 sizeRange{0.1f, 0.3f};       ///< Random size between min and max
    float rotationVariance = 3.14159f;       ///< Max random rotation
};

// ============================================================================
// DecalSystem
// ============================================================================

class DecalSystem {
public:
    static DecalSystem& GetInstance();

    void Initialize(uint32_t maxDecals = 512);
    void Update(float deltaTime);
    void Shutdown();

    /// Spawn a decal at a hit point
    Decal* SpawnDecal(const XMFLOAT3& position, const XMFLOAT3& normal,
                       DecalType type, SurfaceType surface = SurfaceType::Default,
                       float size = -1.0f);

    /// Spawn a decal with explicit material
    Decal* SpawnDecalWithMaterial(const XMFLOAT3& position, const XMFLOAT3& normal,
                                  const std::string& materialName, float size = 0.2f);

    /// Get all active decals for rendering
    const std::vector<Decal>& GetActiveDecals() const { return m_decals; }
    uint32_t GetActiveDecalCount() const;

    // Material management
    void RegisterMaterial(const DecalMaterial& material);
    const DecalMaterial* GetMaterial(const std::string& name) const;

    // Surface-decal mapping
    void RegisterSurfaceMapping(const SurfaceDecalMapping& mapping);

    // Configuration
    void SetMaxDecals(uint32_t maxDecals) { m_maxDecals = maxDecals; }
    void SetDefaultFadeTime(float seconds) { m_defaultFadeTime = seconds; }
    void SetDefaultFadeDuration(float seconds) { m_defaultFadeDuration = seconds; }

    /// Clear all decals
    void ClearAllDecals();

    /// Console integration
    std::string Console_GetStatus() const;
    void Console_SetMaxDecals(uint32_t max);
    void Console_SpawnTestDecal(float x, float y, float z);

private:
    DecalSystem() = default;

    Decal* GetAvailableDecal();
    void FadeOldestDecal();
    XMFLOAT3 ComputeTangent(const XMFLOAT3& normal) const;

    std::vector<Decal> m_decals;
    std::unordered_map<std::string, DecalMaterial> m_materials;
    std::vector<SurfaceDecalMapping> m_surfaceMappings;

    uint32_t m_maxDecals = 512;
    float m_defaultFadeTime = 15.0f;     ///< Seconds before fade starts
    float m_defaultFadeDuration = 3.0f;  ///< Seconds to fade out
};

} // namespace Spark::Graphics
