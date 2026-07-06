#include "Graphics/WorldBasicRenderer.h"
#include "Engine/ECS/Components.h"
#include "Engine/ECS/Components/CoreComponents.h"
#include "Game/PlaceholderMesh.h"
#include "Graphics/GraphicsEngine.h"
#include "Graphics/Mesh.h"

using namespace DirectX;

namespace Spark
{

WorldMeshCache::WorldMeshCache() = default;
WorldMeshCache::~WorldMeshCache() = default;

Mesh* WorldMeshCache::GetOrLoad(GraphicsEngine& g, const std::string& path)
{
    if (path.empty() || !g.GetDevice())
        return nullptr;
    if (auto it = m_cache.find(path); it != m_cache.end())
        return it->second.get();
    auto mesh = std::make_unique<Mesh>();
    LoadOrPlaceholderMesh(*mesh, g.GetDevice(), g.GetContext(), std::wstring(path.begin(), path.end()));
    Mesh* raw = mesh.get();
    m_cache.emplace(path, std::move(mesh));
    return raw;
}

void RenderWorldBasic(World& world, GraphicsEngine& g, WorldMeshCache& cache, const XMMATRIX& view,
                       const XMMATRIX& proj)
{
    g.SetBasicShaders();
    g.ApplyBasicRenderStates();
    for (auto e : world.GetEntitiesWith<Transform, MeshRenderer>())
    {
        const MeshRenderer* mr = world.GetComponent<MeshRenderer>(e);
        if (!mr->visible)
            continue;
        Mesh* mesh = cache.GetOrLoad(g, mr->meshPath);
        if (!mesh || mesh->GetIndexCount() == 0)
            continue;
        const Transform* t = world.GetComponent<Transform>(e);
        const XMMATRIX wmat = t->GetWorldMatrix(world.GetRegistry());
        ID3D11ShaderResourceView* srv = nullptr;
        if (!mr->materialPath.empty())
        {
            if (mr->materialPath.size() > 5 && mr->materialPath.compare(mr->materialPath.size() - 5, 5, ".json") == 0)
            {
                if (const auto* mat = g.GetOrLoadBasicMaterial(mr->materialPath))
                    srv = mat->srv.Get();
            }
            else
            {
                srv = g.GetOrLoadTextureSRV(mr->materialPath);
            }
        }
        g.UpdateBasicConstants(wmat, view, proj, XMFLOAT4(1, 1, 1, 1), XMFLOAT2(1, 1));
        g.SetBasicTexture(srv);
        mesh->Render(g.GetContext());
    }
    g.SetBasicTexture(nullptr);
}

} // namespace Spark
