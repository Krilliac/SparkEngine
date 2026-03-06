/**
 * @file PlaceholderMesh.h
 * @brief Robust mesh loading helper with automatic procedural fallback
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides the LoadOrPlaceholderMesh() inline utility that attempts to load a
 * mesh from an OBJ file on disk and, if that fails for any reason, falls back
 * through a cascade of procedural shape generators (cube -> triangle -> plane)
 * to guarantee that the caller always ends up with a renderable mesh.
 *
 * This helper is used by every primitive game object (CubeObject, PlaneObject,
 * SphereObject, etc.) during their CreateMesh() override to make initialization
 * resilient against missing or corrupt asset files.
 *
 * @note Only @c .obj files are supported by the underlying tinyobjloader backend.
 *       Other formats will be logged as unsupported and trigger the fallback path.
 *
 * @see Mesh, Primitives, CubeObject, PlaneObject, SphereObject
 */

#pragma once
#include "../Core/Platform.h"

#include "Utils/Assert.h"
#include "Utils/SparkError.h"
#include "../Graphics/Mesh.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include <Windows.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <filesystem>

/**
 * @brief Load a mesh from file, or generate a procedural placeholder on failure
 *
 * This function follows a multi-step strategy to ensure the given Mesh object
 * always contains valid, renderable geometry:
 *
 * 1. **Initialize** the Mesh with the provided D3D11 device/context.
 * 2. **Attempt file load** — if @p path is non-empty and the file exists with
 *    a @c .obj extension, call Mesh::LoadFromFile().
 * 3. **Fallback cascade** — if file loading fails (or is skipped), try creating
 *    geometry procedurally in this order:
 *    - Mesh::CreateCube(1.0f)
 *    - Mesh::CreateTriangle(1.0f)
 *    - Mesh::CreatePlane(2.0f, 2.0f)
 * 4. **Validation** — asserts that the mesh has non-zero vertex and index counts.
 *
 * @param mesh    [in,out] Mesh object to populate. Must not already be initialized.
 * @param device  DirectX 11 device for GPU resource creation
 * @param context DirectX 11 device context for rendering commands
 * @param path    File path to attempt loading from. May be empty to skip the
 *                file-load step and go straight to procedural generation.
 *
 * @warning Triggers ASSERT_ALWAYS_MSG if all fallback methods fail, as a mesh
 *          with zero vertices or indices would cause rendering crashes.
 */
inline void LoadOrPlaceholderMesh(
    Mesh& mesh,
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    const std::wstring& path)
{
    // 1) Initialize mesh with device/context
    HRESULT hrInit = mesh.Initialize(device, context);
    SPARK_LOG_DEBUG("Mesh", "Mesh::Initialize returned HR=0x%08lX", static_cast<long>(hrInit));
    ASSERT_MSG(SUCCEEDED(hrInit), "Mesh::Initialize failed");

    // 2) Attempt to load from file
    bool loaded = false;
    if (!path.empty())
    {
        bool fileExists = std::filesystem::exists(path);

        if (fileExists) {
            std::wstring ext = std::filesystem::path(path).extension().wstring();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);

            if (ext == L".obj") {
                loaded = mesh.LoadFromFile(path);
                if (!loaded) {
                    SPARK_LOG_WARN("Mesh", "LoadFromFile failed, falling back to procedural cube");
                }
            }
            else {
                SPARK_LOG_WARN("Mesh", "Unsupported file format (tinyobjloader only supports .obj). Falling back to cube");
            }
        }
        else {
            SPARK_LOG_WARN("Mesh", "Mesh file not found, falling back to procedural cube");
        }
    }

    // 3) Fallback to procedural shapes if load failed
    if (!loaded)
    {
        HRESULT hrShape = mesh.CreateCube(1.0f);

        if (FAILED(hrShape)) {
            SPARK_LOG_WARN("Mesh", "CreateCube failed (HR=0x%08lX), trying CreateTriangle", static_cast<long>(hrShape));
            hrShape = mesh.CreateTriangle(1.0f);
        }

        if (FAILED(hrShape)) {
            SPARK_LOG_WARN("Mesh", "CreateTriangle failed (HR=0x%08lX), trying CreatePlane", static_cast<long>(hrShape));
            hrShape = mesh.CreatePlane(2.0f, 2.0f);
        }

        if (FAILED(hrShape)) {
            SPARK_LOG_ERROR("Mesh", "All fallback mesh creation methods failed (HR=0x%08lX)", static_cast<long>(hrShape));
        }
        else {
            mesh.SetPlaceholder(true);
        }
    }

    // 4) Validate final mesh
    UINT vc = mesh.GetVertexCount();
    UINT ic = mesh.GetIndexCount();

    if (vc == 0 || ic == 0) {
        SPARK_LOG_ERROR("Mesh", "Mesh has zero vertices (%u) or indices (%u). "
            "Possible causes: invalid D3D device/context, buffer creation failure, or mesh creation bug",
            vc, ic);
    }

    ASSERT_ALWAYS_MSG(vc > 0 && ic > 0,
        "Mesh ended up with zero vertices or indices after placeholder creation!");
}
