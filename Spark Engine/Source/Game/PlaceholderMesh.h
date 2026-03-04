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
 * The function emits extensive debug logging to @c std::wcerr and the Windows
 * debug output channel (@c OutputDebugStringW) so that mesh loading issues can
 * be diagnosed without attaching a debugger.
 *
 * @note Only @c .obj files are supported by the underlying tinyobjloader backend.
 *       Other formats will be logged as unsupported and trigger the fallback path.
 *
 * @see Mesh, Primitives, CubeObject, PlaneObject, SphereObject
 */

#pragma once
#include "../Core/Platform.h"

#include "Utils/Assert.h"
#include "../Graphics/Mesh.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include <Windows.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <iostream>
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
 *
 * @note Verbose debug output is written to @c std::wcerr and @c OutputDebugStringW
 *       at every decision point to aid in diagnosing asset-loading issues.
 */
inline void LoadOrPlaceholderMesh(
    Mesh& mesh,
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    const std::wstring& path)
{
    // 1) Initialize mesh with device/context
    std::wcerr << L"[DEBUG] Initializing mesh with D3D device/context..." << std::endl;
    HRESULT hrInit = mesh.Initialize(device, context);
    std::wcerr << L"[DEBUG] Mesh::Initialize returned HR = 0x"
        << std::hex << hrInit << std::dec << std::endl;
    ASSERT_MSG(SUCCEEDED(hrInit), "Mesh::Initialize failed");

    // 2) Attempt to load from file
    bool loaded = false;
    if (!path.empty())
    {
        // Check if file exists first
        bool fileExists = std::filesystem::exists(path);
        std::wcerr << L"[DEBUG] File \"" << path << L"\" exists: "
            << (fileExists ? L"YES" : L"NO") << std::endl;

        if (fileExists) {
            // Check file extension - tinyobjloader only supports OBJ files
            std::wstring ext = std::filesystem::path(path).extension().wstring();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);

            std::wcerr << L"[DEBUG] File extension: \"" << ext << L"\"" << std::endl;

            if (ext == L".obj") {
                std::wcerr << L"[DEBUG] Attempting mesh.LoadFromFile(\"" << path << L"\")..." << std::endl;
                loaded = mesh.LoadFromFile(path);
                std::wcerr << L"[DEBUG] LoadFromFile returned: "
                    << (loaded ? L"SUCCESS" : L"FAILURE") << std::endl;
            }
            else {
                std::wcerr << L"[DEBUG] Unsupported file format: " << ext
                    << L" (tinyobjloader only supports .obj files)" << std::endl;
                OutputDebugStringW((L"[Mesh] Unsupported file format: " + ext +
                    L" - tinyobjloader only supports .obj files. Falling back to cube.\n").c_str());
            }
        }

        if (!loaded) {
            OutputDebugStringW((L"[Mesh] Failed to load \"" + path +
                L"\" – falling back to procedural cube.\n").c_str());
        }
    }
    else
    {
        std::wcerr << L"[DEBUG] No path provided, skipping LoadFromFile." << std::endl;
    }

    // 3) Fallback to procedural shapes if load failed
    if (!loaded)
    {
        std::wcerr << L"[DEBUG] Creating procedural cube placeholder..." << std::endl;

        // Try multiple creation methods
        HRESULT hrShape = E_FAIL;

        // Method 1: Try standard CreateCube
        hrShape = mesh.CreateCube(1.0f);
        std::wcerr << L"[DEBUG] CreateCube(1.0f) HR = 0x"
            << std::hex << hrShape << std::dec << std::endl;

        // Method 2: If that fails, try CreateTriangle
        if (FAILED(hrShape)) {
            std::wcerr << L"[DEBUG] CreateCube failed, trying CreateTriangle..." << std::endl;
            hrShape = mesh.CreateTriangle(1.0f);
            std::wcerr << L"[DEBUG] CreateTriangle HR = 0x"
                << std::hex << hrShape << std::dec << std::endl;
        }

        // Method 3: Try CreatePlane as last resort
        if (FAILED(hrShape)) {
            std::wcerr << L"[DEBUG] CreateTriangle failed, trying CreatePlane..." << std::endl;
            hrShape = mesh.CreatePlane(2.0f, 2.0f);
            std::wcerr << L"[DEBUG] CreatePlane HR = 0x"
                << std::hex << hrShape << std::dec << std::endl;
        }

        if (FAILED(hrShape)) {
            std::wcerr << L"[ERROR] All fallback mesh creation methods failed!" << std::endl;
            OutputDebugStringW(L"[Mesh] CRITICAL: All fallback mesh creation methods failed!\n");
        }
        else {
            mesh.SetPlaceholder(true);
        }
    }

    // 4) Log final mesh counts
    UINT vc = mesh.GetVertexCount();
    UINT ic = mesh.GetIndexCount();
    std::wcerr << L"[DEBUG] Final mesh counts: Vertices=" << vc
        << L", Indices=" << ic << std::endl;

    // 5) More detailed assertion with helpful message
    if (vc == 0 || ic == 0) {
        std::wcerr << L"[ERROR] Mesh creation completely failed!" << std::endl;
        std::wcerr << L"[ERROR] This usually means:" << std::endl;
        std::wcerr << L"[ERROR]   1. D3D11 device/context is invalid" << std::endl;
        std::wcerr << L"[ERROR]   2. Buffer creation failed" << std::endl;
        std::wcerr << L"[ERROR]   3. Mesh creation implementation has bugs" << std::endl;

        OutputDebugStringW(L"[Mesh] CRITICAL ERROR: Mesh ended up with zero vertices or indices!\n");
    }

    ASSERT_ALWAYS_MSG(vc > 0 && ic > 0,
        "Mesh ended up with zero vertices or indices after placeholder creation!");
}
