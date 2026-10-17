/**
 * @file actor_module.cpp
 * @brief Real engine actors exposed by a dynamically loaded public-SDK consumer.
 */

#include "ActorContract.h"

#include <Spark/CubeObject.h>
#include <Spark/Model.h>
#include <Spark/PlaneObject.h>
#include <Spark/SphereObject.h>
#include <Spark/WallObject.h>

#include <array>
#include <memory>
#include <type_traits>

#if defined(_WIN32)
#define ACTOR_TEST_EXPORT __declspec(dllexport)
#else
#define ACTOR_TEST_EXPORT __attribute__((visibility("default")))
#endif

static_assert(
    std::is_same_v<decltype(&GameObject::Initialize), HRESULT (GameObject::*)(ID3D11Device*, ID3D11DeviceContext*)>);
static_assert(std::is_same_v<decltype(&GameObject::Render),
                             void (GameObject::*)(const DirectX::XMMATRIX&, const DirectX::XMMATRIX&)>);
static_assert(std::is_same_v<decltype(&Mesh::CreateCube), HRESULT (Mesh::*)(float)>);
static_assert(std::is_same_v<decltype(&Mesh::CreateSphere), HRESULT (Mesh::*)(float, int, int)>);
static_assert(std::is_standard_layout_v<Vertex> && sizeof(Vertex) == 32);
static_assert(offsetof(Vertex, Position) == 0 && offsetof(Vertex, Normal) == 12 && offsetof(Vertex, TexCoord) == 24);

namespace
{
    class ConsumerActor final : public GameObject
    {
      public:
        explicit ConsumerActor(ActorObservations& observations) : m_observations(observations) {}
        ~ConsumerActor() override { ++m_observations.destructions; }

        void Update(float dt) override
        {
            ++m_observations.updates;
            Translate({dt, 0.0f, 0.0f});
            GameObject::Update(dt);
        }

        void Render(const XMMATRIX& view, const XMMATRIX& projection) override
        {
            ++m_observations.renders;
            GameObject::Render(view, projection);
        }

        void OnHit(GameObject* target) override
        {
            if (target)
                ++m_observations.hits;
        }

        void OnHitWorld(const XMFLOAT3& point, const XMFLOAT3& normal) override
        {
            if (point.x == 7.0f && normal.y == 1.0f)
                ++m_observations.worldHits;
        }

      protected:
        void CreateMesh() override
        {
            // Match the FPS extension seam: a derived actor directly calls the
            // real protected Mesh without any engine-private header.
            m_observations.meshResult = m_mesh->CreateCube(1.0f);
            if (SUCCEEDED(m_observations.meshResult))
                m_observations.meshResult = m_mesh->CreateSphere(1.0f, 8, 4);
        }

      private:
        ActorObservations& m_observations;
    };
} // namespace

extern "C" ACTOR_TEST_EXPORT GameObject* CreatePublicActor(unsigned kind, ActorObservations* observations)
{
    if (!observations)
        return nullptr;

    // Its out-of-line destructor must resolve to the installed engine library.
    Model model;
    std::unique_ptr<GameObject> actor;
    switch (kind)
    {
    case 0:
        actor = std::make_unique<ConsumerActor>(*observations);
        break;
    case 1:
        actor = std::make_unique<CubeObject>(2.0f);
        break;
    case 2:
        actor = std::make_unique<PlaneObject>(3.0f, 4.0f);
        break;
    case 3:
        actor = std::make_unique<SphereObject>(1.5f, 8, 4);
        break;
    case 4:
        actor = std::make_unique<WallObject>(5.0f, 3.0f);
        break;
    default:
        return nullptr;
    }
    actor->SetPosition({3.0f, 4.0f, 5.0f});
    actor->SetScale({2.0f, 3.0f, 4.0f});
    actor->SetMaterialPath("Assets/Materials/ActorConsumer.json");
    actor->SetVisible(false);
    return actor.release();
}

extern "C" ACTOR_TEST_EXPORT float PublicActorMatrixElement(const GameObject* actor, unsigned row, unsigned column)
{
    if (!actor || row >= 4 || column >= 4)
        return 0.0f;
    DirectX::XMFLOAT4X4 matrix;
    DirectX::XMStoreFloat4x4(&matrix, actor->GetWorldMatrix());
    return matrix.m[row][column];
}

extern "C" ACTOR_TEST_EXPORT std::size_t PublicActorLayout(unsigned kind, bool alignment)
{
    constexpr std::array sizes = {sizeof(GameObject),   sizeof(Model),      sizeof(CubeObject), sizeof(PlaneObject),
                                  sizeof(SphereObject), sizeof(WallObject), sizeof(Mesh)};
    constexpr std::array alignments = {alignof(GameObject),  alignof(Model),        alignof(CubeObject),
                                       alignof(PlaneObject), alignof(SphereObject), alignof(WallObject),
                                       alignof(Mesh)};
    if (kind >= sizes.size())
        return 0;
    return alignment ? alignments[kind] : sizes[kind];
}

extern "C" ACTOR_TEST_EXPORT bool VerifyPublicMeshValues()
{
    const Vertex defaultVertex;
    const Vertex vertex({1.0f, 2.0f, 3.0f}, {0.0f, 1.0f, 0.0f}, {0.25f, 0.75f});
    const std::vector<Vertex> vertices{vertex};
    MeshSubmesh submesh;
    submesh.indexStart = 3;
    submesh.indexCount = 6;
    submesh.diffuseTexture = "public-texture.png";
    const std::vector<MeshSubmesh> submeshes{submesh};
    Mesh mesh;
    return defaultVertex.Position.x == 0.0f && defaultVertex.Normal.y == 1.0f && vertices[0].Position.z == 3.0f &&
           vertices[0].Normal.y == 1.0f && vertices[0].TexCoord.y == 0.75f && submeshes[0].indexStart == 3 &&
           submeshes[0].indexCount == 6 && submeshes[0].diffuseColor.w == 1.0f &&
           submeshes[0].diffuseTexture == "public-texture.png" && mesh.GetSubmeshes().empty();
}
