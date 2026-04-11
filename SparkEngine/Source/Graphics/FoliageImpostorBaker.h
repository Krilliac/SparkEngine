/**
 * @file FoliageImpostorBaker.h
 * @brief Atlas packing and angle selection for foliage impostors
 * @author Spark Engine Team
 * @date 2026
 *
 * Impostors are low-cost 2D proxies that replace a foliage mesh once it is
 * beyond its per-species impostor distance. Each species is baked at a
 * fixed number of yaw angles into a shared atlas texture; the renderer
 * selects the cell closest to the view-to-instance angle each frame and
 * draws it as a camera-facing quad.
 *
 * This header contains only the **CPU-side layout and selection logic**:
 *
 *   - `ComputeAtlasLayout()` packs N species x M angle cells into a single
 *     2D atlas and reports the pixel rectangles and the final texture size.
 *   - `SelectAngleSlot()` picks which angle cell to use for a given
 *     view-relative yaw.
 *   - `GetAngleSlotUV()` returns the [minU, minV, maxU, maxV] region of a
 *     specific angle cell inside an atlas slot.
 *
 * The actual GPU bake (rendering the mesh into the atlas) is performed by
 * the render consumer on Windows builds; it is intentionally kept out of
 * this header so the logic here is testable in CI without a GPU.
 *
 * @note **GPU bake — partial.** The CPU layout helpers above are joined by
 *       `FoliageImpostorAtlas` (Windows-only) which:
 *
 *         - Allocates the atlas `ID3D11Texture2D` plus matching depth
 *           buffer, RTV, SRV.
 *         - Compiles the bake VS/PS pair from
 *           `Shaders/HLSL/FoliageImpostorBake.hlsl`.
 *         - Renders one mesh into one atlas slot via `BakeSlot()`,
 *           iterating over `slot.angleSteps` yaw angles and using a
 *           viewport the size of a single cell so each angle lands in
 *           the correct sub-rectangle.
 *
 *       What is **not** wired in yet:
 *         - The lifecycle does not call `BakeSlot()` for any species.
 *           Production code must walk `FoliageManager`'s species
 *           registry, fetch each `MeshAsset`, and bake on demand. The
 *           render side already accepts `FoliageRenderLOD::Impostor`
 *           records (see `FoliageRenderer::UploadToSceneBuffer`) but
 *           the SRV is currently never bound for sampling because no
 *           atlas has been built.
 *
 *       Tests: `Tests/TestFoliageImpostorBaker.cpp` covers the CPU-side
 *       layout math. The Windows-only GPU paths in
 *       `FoliageImpostorAtlas` are exercised by
 *       `Tests/TestFoliageImpostorAtlas.cpp` once a real D3D11 device is
 *       reachable; on Linux CI they compile out of the build.
 *
 * @threadsafety All static methods are pure functions — safe to call from
 *               any thread.
 */

#pragma once

#include "../Core/Platform.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#include <d3d11.h>
#include <wrl/client.h>
#endif

class MeshAsset;

namespace Spark::Graphics
{
    class FoliageManager;

    /**
     * @brief A packed slot in a foliage impostor atlas.
     *
     * Each slot reserves `angleSteps` horizontal cells of size
     * `cellSize x cellSize` pixels, laid out in a single row starting at
     * `(atlasX, atlasY)`. The slot width is therefore
     * `angleSteps * cellSize`.
     */
    struct ImpostorAtlasSlot
    {
        uint32_t speciesIndex = 0; ///< Species identifier the slot belongs to.
        uint32_t atlasX = 0;       ///< Pixel X of the slot's top-left corner.
        uint32_t atlasY = 0;       ///< Pixel Y of the slot's top-left corner.
        uint32_t cellSize = 0;     ///< Width/height of a single angle cell in pixels.
        uint32_t angleSteps = 0;   ///< Number of angle cells laid out horizontally.
    };

    /**
     * @brief CPU-side impostor layout and selection helpers.
     */
    class FoliageImpostorBaker
    {
      public:
        /**
         * @brief Compute an atlas layout for a set of species.
         *
         * Each species gets one horizontal slot of `cellSize * angleSteps`
         * pixels wide and `cellSize` pixels tall. Slots are packed in a
         * grid, wrapping to a new row whenever the current row would
         * exceed `maxAtlasSize`. The returned atlas width/height are
         * rounded up to the next power of two for GPU friendliness.
         *
         * @param speciesIndices  Species identifiers to pack. Order is preserved.
         * @param cellSize        Side length in pixels of a single angle cell.
         * @param angleSteps      Number of yaw cells per species (>= 1).
         * @param maxAtlasSize    Maximum atlas width/height in pixels.
         * @param outAtlasWidth   [out] Final atlas width in pixels.
         * @param outAtlasHeight  [out] Final atlas height in pixels.
         * @return Per-species slot rectangles in the same order as the input.
         *         Empty if inputs are invalid (e.g. cellSize == 0).
         */
        static std::vector<ImpostorAtlasSlot> ComputeAtlasLayout(const std::vector<uint32_t>& speciesIndices,
                                                                 uint32_t cellSize, uint32_t angleSteps,
                                                                 uint32_t maxAtlasSize, uint32_t& outAtlasWidth,
                                                                 uint32_t& outAtlasHeight);

        /**
         * @brief Select which angle cell to sample for a given view-relative yaw.
         *
         * The yaw is wrapped into [0, 2*pi) and mapped to one of
         * `angleSteps` evenly-spaced bins. Bin 0 corresponds to yaw 0.
         *
         * @param yawRadians  Yaw in radians (any real value; wrapped internally).
         * @param angleSteps  Number of angle cells (>= 1).
         * @return Bin index in [0, angleSteps).
         */
        static uint32_t SelectAngleSlot(float yawRadians, uint32_t angleSteps);

        /**
         * @brief Compute the UV region for a specific angle cell in a slot.
         *
         * Returns normalized [0, 1] texture coordinates suitable for direct
         * sampling. If the slot or angle index is invalid, all outputs are
         * set to 0.
         *
         * @param slot             Atlas slot.
         * @param angleSlotIndex   Angle bin in [0, slot.angleSteps).
         * @param atlasWidth       Atlas texture width in pixels.
         * @param atlasHeight      Atlas texture height in pixels.
         * @param outMinU [out]    Minimum U of the cell.
         * @param outMinV [out]    Minimum V of the cell.
         * @param outMaxU [out]    Maximum U of the cell.
         * @param outMaxV [out]    Maximum V of the cell.
         */
        static void GetAngleSlotUV(const ImpostorAtlasSlot& slot, uint32_t angleSlotIndex, uint32_t atlasWidth,
                                   uint32_t atlasHeight, float& outMinU, float& outMinV, float& outMaxU,
                                   float& outMaxV);

        /**
         * @brief Round a value up to the next power of two.
         *
         * Used to size the atlas texture. Values already a power of two are
         * returned unchanged. Zero maps to one.
         */
        static uint32_t NextPowerOfTwo(uint32_t v);
    };

#ifdef SPARK_PLATFORM_WINDOWS
    /**
     * @brief D3D11-backed runtime impostor atlas.
     *
     * Owns the GPU texture, depth buffer, and bake shader pair used to
     * render foliage species into per-species slots. Lives next to
     * `FoliageImpostorBaker` because it consumes the same
     * `ImpostorAtlasSlot` layout produced by `ComputeAtlasLayout()`.
     *
     * Lifecycle:
     *   - `Initialize(device, width, height)` allocates the atlas and
     *     compiles the bake shaders. Idempotent — re-initialising
     *     releases the previous atlas first.
     *   - `BakeSlot(context, slot, mesh, bbox, tint)` rasterises the
     *     mesh into the slot's cells; one orthographic view per yaw step.
     *   - `Shutdown()` releases all GPU resources.
     *
     * @threadsafety Not thread-safe. Call only from the render thread
     *               while no other system is mutating the immediate
     *               D3D11 device context.
     */
    class FoliageImpostorAtlas
    {
      public:
        FoliageImpostorAtlas() = default;
        ~FoliageImpostorAtlas();

        FoliageImpostorAtlas(const FoliageImpostorAtlas&) = delete;
        FoliageImpostorAtlas& operator=(const FoliageImpostorAtlas&) = delete;

        /**
         * @brief Allocate the atlas texture and compile bake shaders.
         *
         * @param device     D3D11 device used to create resources.
         * @param width      Atlas width in pixels (must match
         *                   `ComputeAtlasLayout` output to avoid clipping).
         * @param height     Atlas height in pixels.
         * @return true on success, false on shader compile or resource
         *         creation failure.
         */
        bool Initialize(ID3D11Device* device, uint32_t width, uint32_t height);

        /**
         * @brief Release every GPU resource owned by the atlas.
         */
        void Shutdown();

        /**
         * @brief Render `mesh` into one atlas slot from `slot.angleSteps`
         *        yaw angles.
         *
         * The mesh is fitted with an orthographic projection sized to
         * `bboxMax - bboxMin`. Cells are written one at a time using a
         * cell-sized viewport, so the result is identical to the layout
         * `ComputeAtlasLayout` reported.
         *
         * @param context        D3D11 immediate context.
         * @param slot           Slot rectangle from `ComputeAtlasLayout`.
         * @param mesh           Source mesh — must already be uploaded
         *                       (vertex+index buffers non-null).
         * @param bboxMin        World-space mesh bounding box minimum.
         * @param bboxMax        World-space mesh bounding box maximum.
         * @param tint           Per-species RGBA tint passed to the PS.
         * @return true if every angle was rendered without error.
         */
        bool BakeSlot(ID3D11DeviceContext* context, const ImpostorAtlasSlot& slot, const MeshAsset& mesh,
                      const DirectX::XMFLOAT3& bboxMin, const DirectX::XMFLOAT3& bboxMax,
                      const DirectX::XMFLOAT4& tint);

        /**
         * @brief Walk a `FoliageManager` registry and bake every species
         *        whose mesh is currently resolvable into the atlas.
         *
         * Allocates a fresh layout sized for `manager.GetSpeciesCount()`
         * via `FoliageImpostorBaker::ComputeAtlasLayout`, re-creates the
         * atlas texture/depth/SRV at the resulting size, then iterates the
         * species, fetches each mesh through the supplied loader, computes
         * the bounding box from `MeshAssetData`, and calls `BakeSlot`.
         *
         * @param device         D3D11 device used to (re)allocate the atlas.
         * @param context        D3D11 immediate context for draw commands.
         * @param manager        Foliage species registry.
         * @param meshLoader     Callback that resolves a species `meshPath`
         *                       to a `std::shared_ptr<MeshAsset>`. Returning
         *                       null marks the species as unbaked (it will
         *                       be re-attempted on the next call).
         * @param cellSize       Pixel side length of one angle cell.
         * @param angleSteps     Number of yaw cells per species.
         * @param maxAtlasSize   Upper bound on atlas dimensions.
         * @return Number of species successfully baked. Zero is a valid
         *         outcome (empty registry, or every mesh load returned null).
         */
        uint32_t BakeAllRegisteredSpecies(
            ID3D11Device* device, ID3D11DeviceContext* context, const class FoliageManager& manager,
            const std::function<std::shared_ptr<MeshAsset>(const std::string&)>& meshLoader, uint32_t cellSize = 256,
            uint32_t angleSteps = 8, uint32_t maxAtlasSize = 4096);

        bool IsInitialized() const { return m_initialized; }
        uint32_t GetWidth() const { return m_width; }
        uint32_t GetHeight() const { return m_height; }
        ID3D11ShaderResourceView* GetSRV() const { return m_srv.Get(); }
        ID3D11Texture2D* GetTexture() const { return m_atlas.Get(); }

        /**
         * @brief Read-only access to the per-species layout slots from the
         *        most recent `BakeAllRegisteredSpecies` call.
         *
         * Indexed by the *position* in the layout request — which is also
         * the global species index since the helper passes indices 0..N-1.
         */
        const std::vector<ImpostorAtlasSlot>& GetSlots() const { return m_slots; }

        /** @brief Cell size (pixels) used for the most recent bake. */
        uint32_t GetCellSize() const { return m_cellSize; }

        /** @brief Angle steps per species used for the most recent bake. */
        uint32_t GetAngleSteps() const { return m_angleSteps; }

        /**
         * @brief GPU structured buffer view containing per-species atlas
         *        cell records, indexed by global species index.
         *
         * Phase F layout: two float4s per species, stored contiguously.
         *   record[0] = uvRect: (minU, minV, cellDU, cellDV)
         *   record[1] = meta:   (billboardHeight, 0, 0, 0)
         *
         * The shader reads both via `cells[materialId * 2 + 0]` (uvRect)
         * and `cells[materialId * 2 + 1]` (meta). Built once per bake by
         * `UploadCellBuffer`. Returns nullptr if no bake has happened yet.
         */
        ID3D11ShaderResourceView* GetCellSRV() const { return m_cellSrv.Get(); }

        /**
         * @brief CPU-visible copy of the per-species cell records.
         *
         * Same 2-float4-per-species layout as the GPU structured buffer —
         * `m_cellRecords[speciesIndex * 2 + 0]` is the UV rect and
         * `m_cellRecords[speciesIndex * 2 + 1]` is the meta vector. Used
         * by unit tests to verify the layout math without touching D3D.
         */
        const std::vector<DirectX::XMFLOAT4>& GetCellRecords() const { return m_cellRecords; }

      private:
        bool CompileBakeShaders(ID3D11Device* device);
        bool UploadCellBuffer(ID3D11Device* device, const class FoliageManager& manager);

        Microsoft::WRL::ComPtr<ID3D11Texture2D> m_atlas;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_rtv;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_srv;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> m_depthBuffer;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilView> m_dsv;

        Microsoft::WRL::ComPtr<ID3D11VertexShader> m_vs;
        Microsoft::WRL::ComPtr<ID3D11PixelShader> m_ps;
        Microsoft::WRL::ComPtr<ID3D11InputLayout> m_inputLayout;
        Microsoft::WRL::ComPtr<ID3D11Buffer> m_constantBuffer;
        Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_rasterState;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilState> m_depthState;
        Microsoft::WRL::ComPtr<ID3D11BlendState> m_blendState;

        // Per-species cell buffer (rebuilt by BakeAllRegisteredSpecies).
        // m_cellRecords stores 2 float4s per species: [uvRect, meta].
        Microsoft::WRL::ComPtr<ID3D11Buffer> m_cellBuffer;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_cellSrv;
        std::vector<ImpostorAtlasSlot> m_slots;
        std::vector<DirectX::XMFLOAT4> m_cellRecords;

        bool m_initialized = false;
        uint32_t m_width = 0;
        uint32_t m_height = 0;
        uint32_t m_cellSize = 0;
        uint32_t m_angleSteps = 0;
    };
#endif // SPARK_PLATFORM_WINDOWS

} // namespace Spark::Graphics
