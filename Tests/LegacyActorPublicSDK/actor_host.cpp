/**
 * @file actor_host.cpp
 * @brief Public-header-only native actor dispatch and deletion across a loaded DLL.
 */

#include "ActorContract.h"

#include <Spark/CubeObject.h>
#include <Spark/Model.h>
#include <Spark/PlaneObject.h>
#include <Spark/SphereObject.h>
#include <Spark/WallObject.h>

#include <array>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <memory>
#include <set>

#if !defined(_WIN32)
#include <dlfcn.h>
#endif

namespace
{
    class ModuleImage
    {
      public:
        explicit ModuleImage(const std::filesystem::path& path)
        {
#if defined(_WIN32)
            m_handle = LoadLibraryW(path.c_str());
#else
            m_handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
        }

        ~ModuleImage() { Close(); }
        ModuleImage(const ModuleImage&) = delete;
        ModuleImage& operator=(const ModuleImage&) = delete;

        template <typename Function> Function Get(const char* name) const
        {
            if (!m_handle)
                return nullptr;
#if defined(_WIN32)
            return reinterpret_cast<Function>(GetProcAddress(m_handle, name));
#else
            return reinterpret_cast<Function>(dlsym(m_handle, name));
#endif
        }

        bool Close()
        {
            if (!m_handle)
                return true;
#if defined(_WIN32)
            const bool closed = FreeLibrary(m_handle) != 0;
#else
            const bool closed = dlclose(m_handle) == 0;
#endif
            if (closed)
                m_handle = nullptr;
            return closed;
        }

      private:
#if defined(_WIN32)
        HMODULE m_handle = nullptr;
#else
        void* m_handle = nullptr;
#endif
    };
} // namespace

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::cerr << "Expected the actor module image path\n";
        return 1;
    }

    int failures = 0;
    const auto check = [&failures](bool condition, const char* message)
    {
        if (!condition)
        {
            std::cerr << message << '\n';
            ++failures;
        }
    };

#if defined(_WIN32)
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    const HRESULT deviceResult = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                                                   D3D11_SDK_VERSION, &device, nullptr, &context);
    if (FAILED(deviceResult))
    {
        std::cerr << "Native actor contract requires a working D3D11 WARP device\n";
        return 1;
    }
#endif

    constexpr std::array sizes = {sizeof(GameObject),   sizeof(Model),      sizeof(CubeObject), sizeof(PlaneObject),
                                  sizeof(SphereObject), sizeof(WallObject), sizeof(Mesh)};
    constexpr std::array alignments = {alignof(GameObject),  alignof(Model),        alignof(CubeObject),
                                       alignof(PlaneObject), alignof(SphereObject), alignof(WallObject),
                                       alignof(Mesh)};
    for (unsigned cycle = 0; cycle < 2; ++cycle)
    {
        ModuleImage image(std::filesystem::absolute(argv[1]));
        const auto create = image.Get<CreateActor>("CreatePublicActor");
        const auto matrixElement = image.Get<ReadActorMatrix>("PublicActorMatrixElement");
        const auto layout = image.Get<ReadActorLayout>("PublicActorLayout");
        const auto verifyMeshValues = image.Get<VerifyMeshValues>("VerifyPublicMeshValues");
        if (!create || !matrixElement || !layout || !verifyMeshValues)
        {
            std::cerr << "Failed to load the actor module or its required exports\n";
            return 1;
        }
        check(verifyMeshValues(), "Public mesh values must retain fields, defaults, and real validating construction");
        for (unsigned kind = 0; kind < sizes.size(); ++kind)
        {
            check(layout(kind, false) == sizes[kind], "Actor size differs across the DLL boundary");
            check(layout(kind, true) == alignments[kind], "Actor alignment differs across the DLL boundary");
        }

        ActorObservations observations;
        std::set<UINT> ids;
        for (unsigned kind = 0; kind < 5; ++kind)
        {
            // Host destruction invokes the module's deleting virtual destructor
            // while the DLL and its real engine implementation remain loaded.
            std::unique_ptr<GameObject> actor(create(kind, &observations));
            if (!actor)
            {
                check(false, "Module did not construct a real actor");
                continue;
            }
            check(ids.insert(actor->GetID()).second, "Real actor constructors must assign unique IDs");
            check(actor->GetPosition().y == 4.0f && actor->GetScale().z == 4.0f,
                  "Host must read the actual native transform fields");
            check(actor->GetMaterialPath() == "Assets/Materials/ActorConsumer.json",
                  "Host must observe the module's string state without a layout translation");
#if defined(_WIN32)
            check(SUCCEEDED(actor->Initialize(device.Get(), context.Get())), "Real native actor initialization failed");
            check(SUCCEEDED(observations.meshResult), "Derived actor could not call the real Mesh primitive API");
            Mesh* mesh = actor->GetMesh();
            check(mesh && mesh->GetVertexCount() > 0 && mesh->GetIndexCount() > 0,
                  "Real native actor initialization must create mesh geometry");
            check(mesh && mesh->GetVertexBuffer() && mesh->GetIndexBuffer(),
                  "Real native actor initialization must create D3D11 buffers");
#endif
            actor->Update(0.5f);
            actor->OnHit(actor.get());
            actor->OnHitWorld({7.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f});
            actor->Render(DirectX::XMMatrixIdentity(), DirectX::XMMatrixIdentity());
            const float expectedX = kind == 0 ? 3.5f : 3.0f;
            check(actor->GetPosition().x == expectedX, "Virtual update must dispatch to the module's derived actor");
            check(std::abs(matrixElement(actor.get(), 3, 0) - expectedX) < 0.0001f,
                  "The installed GameObject implementation must compute the world translation");
            check(std::abs(matrixElement(actor.get(), 0, 0) - 2.0f) < 0.0001f,
                  "The installed GameObject implementation must preserve transform scale");
            actor->Shutdown();
            check(actor->GetMesh() == nullptr, "Virtual shutdown must release the real actor mesh");
        }
        check(observations.updates == 1 && observations.hits == 1 && observations.worldHits == 1 &&
                  observations.renders == 1 && observations.destructions == 1,
              "Each derived override and destructor must execute exactly once across the DLL boundary");
        check(image.Close(), "Actor DLL must unload after all actors have been destroyed");
    }
    return failures == 0 ? 0 : 1;
}
