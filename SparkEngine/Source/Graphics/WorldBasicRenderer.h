#pragma once
#include "Core/Platform.h" // DirectXMath on Windows / vector-math stubs on Linux
#include "Graphics/ProjectAssetPath.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include "Core/Platform.h"
#endif
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
class World;
class GraphicsEngine;
class Mesh;
namespace Spark
{
    struct WorldBasicRenderStats
    {
        uint32_t candidates = 0;
        uint32_t visible = 0;
        uint32_t drawn = 0;
        uint32_t rejected = 0;
    };
    /**
 * @brief Per-path cache of device-loaded meshes for generic ECS rendering.
 *
 * Shared by the runtime (`-scene`) and, later, the editor viewport — both
 * draw an arbitrary reflected World via RenderWorldBasic() and want mesh
 * loads deduplicated across frames.
 */
    class WorldMeshCache
    {
      public:
        // Declared (not defaulted) here and defined in the .cpp: the cached
        // std::unique_ptr<Mesh> deleter needs Mesh's complete definition, which
        // callers of this header (e.g. SparkEngineWindows.cpp) don't include.
        WorldMeshCache();
        ~WorldMeshCache();

        Mesh* GetOrLoad(GraphicsEngine& g, const std::string& path, std::string_view projectRootUtf8);

        std::optional<ResolvedProjectAssetPath> ResolveAsset(std::string_view projectRootUtf8, const std::string& path);
        void WarnRejectedAssetOnce(std::string_view kind, const std::string& path);

      private:
        std::unordered_map<std::string, std::unique_ptr<Mesh>> m_cache;
        std::unordered_map<std::string, ResolvedProjectAssetPath> m_resolvedAssets;
        std::unordered_map<std::string, std::string> m_placeholderFileSignatures;
        std::unordered_set<std::string> m_warnedRejectedAssets;
    };

    /**
 * @brief Draws visible Transform+MeshRenderer and Transform+SpriteRenderer
 * entities into the currently bound render target, using the basic shader.
 * Sprites are alpha blended with read-only depth and are ordered by
 * (sortingLayer, orderInLayer, entity identity).
 *
 * Caller is responsible for BeginFrame()/EndFrame() around this call and must
 * supply the active project root. Relative asset paths are confined to that
 * root; an empty root permits reserved procedural primitives only.
 */
    // NOTE: '::World' (not bare 'World') — TUs that include a header declaring
    // 'namespace Spark::World' (e.g. TFWorldSetup.h) before this one would
    // otherwise resolve the unqualified name to that namespace (C2882).
    WorldBasicRenderStats RenderWorldBasic(::World& world, GraphicsEngine& g, WorldMeshCache& cache,
                                           const DirectX::XMMATRIX& view, const DirectX::XMMATRIX& proj,
                                           std::string_view projectRootUtf8);

} // namespace Spark
