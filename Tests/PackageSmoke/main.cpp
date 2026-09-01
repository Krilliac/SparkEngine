#include <SparkEngine/Core/PlatformAudioStubs.h>
#include <SparkEngine/Core/PlatformTypes.h>
#include <SparkEngine/Engine/AI/RecastDetourBackend.h>
#include <SparkEngine/Engine/ECS/Components/CoreComponents.h>
#include <SparkEngine/Graphics/EXRLoader.h>
#include <SparkEngine/Graphics/FBXImporter.h>
#include <SparkEngine/Graphics/ScreenCapture.h>
#include <SparkEngine/Physics/PhysicsSystem.h>
#include <SparkEngine/Utils/JsonUtils.h>
#include <Spark/Version.h>

#if defined(SPARK_ANGELSCRIPT_SUPPORT)
#include <angelscript.h>
static_assert(SPARK_ANGELSCRIPT_PACKED_POINTER_OPERAND == 1);
static_assert(alignof(AS_NAMESPACE_QUALIFIER asPWORD_UNALIGNED) == 1);
#endif

#if defined(SPARK_JOLT_PHYSICS_AVAILABLE)
#include <SparkEngine/Physics/PhysicsSpatialQueriesInternal.h>
#endif

#if defined(SPARK_OPENGL_SUPPORT)
#include <SparkEngine/Graphics/RHI/OpenGL/OpenGLDevice.h>
#endif

#include <array>
#include <cstdint>
#include <iostream>

int main()
{
    std::array<uint8_t, 27> bytes{};
    Spark::Graphics::EXRImage image;
    const bool exrRejected = !Spark::Graphics::EXRLoader::Load(bytes.data(), bytes.size(), image);
    const bool fbxRejected =
        !Spark::Graphics::FBXImporter::GetInstance().CanImportFromMemory(bytes.data(), bytes.size());
    const bool screenCaptureRejected =
        !Spark::Graphics::ScreenCapture::GetInstance().TakeScreenshot(nullptr, 0, 0).success;
    const auto json = Spark::Json::Parse(R"({"installed":true})");
    const bool jsonWorks = json["installed"].AsBool();
#if defined(SPARK_ANGELSCRIPT_SUPPORT)
    const char* angelScriptVersion = asGetLibraryVersion();
    const bool angelScriptLinked = angelScriptVersion != nullptr && angelScriptVersion[0] != '\0';
#else
    const bool angelScriptLinked = true;
#endif
    NameComponent name{"package-smoke"};
    PhysicsSystem physics;
    const bool recastLinked = Spark::AI::IsRecastAvailable();
    std::cout << "Spark package smoke test: EXR=" << exrRejected << " FBX=" << fbxRejected
              << " capture=" << screenCaptureRejected << " JSON=" << jsonWorks << " Recast=" << recastLinked
              << " AngelScript=" << angelScriptLinked << " ECS=" << name.name
              << " EngineVersion=" << Spark::GetEngineVersion() << '\n';
    return exrRejected && fbxRejected && screenCaptureRejected && jsonWorks && angelScriptLinked && !name.name.empty()
               ? 0
               : 1;
}
