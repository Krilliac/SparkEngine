#pragma once
#include "Core/Platform.h" // DirectXMath on Windows / vector-math stubs on Linux
#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif
#include <memory>
#include <string>
#include <unordered_map>
class World;
class GraphicsEngine;
class Mesh;
namespace Spark
{
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

        Mesh* GetOrLoad(GraphicsEngine& g, const std::string& path);

      private:
        std::unordered_map<std::string, std::unique_ptr<Mesh>> m_cache;
    };

    /**
 * @brief Draws every entity that has Transform+MeshRenderer(visible) into the
 * currently bound render target, using the basic shader.
 *
 * Caller is responsible for BeginFrame()/EndFrame() around this call.
 */
    // NOTE: '::World' (not bare 'World') — TUs that include a header declaring
    // 'namespace Spark::World' (e.g. TFWorldSetup.h) before this one would
    // otherwise resolve the unqualified name to that namespace (C2882).
    void RenderWorldBasic(::World& world, GraphicsEngine& g, WorldMeshCache& cache, const DirectX::XMMATRIX& view,
                          const DirectX::XMMATRIX& proj);

} // namespace Spark
