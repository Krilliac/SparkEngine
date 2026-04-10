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
 * @warning **GPU bake pipeline not yet implemented.** As of 2026-04-10 this
 *          file contains only CPU-side atlas layout (`ComputeAtlasLayout`),
 *          angle slot selection (`SelectAngleSlot`), and UV math
 *          (`GetAngleSlotUV`). There is no compute shader, no render-target
 *          allocation, and no HLSL under `Shaders/HLSL/FoliageImpostor*`
 *          today. Until the GPU bake is written, impostor atlases must be
 *          pre-baked offline (e.g. by a tool pass) and loaded as regular
 *          textures. The CPU layout logic here stays valid either way — a
 *          future `FoliageImpostorGPUBaker` will consume the same
 *          `AtlasLayout` struct and just write pixels into the rectangles
 *          this file allocates.
 *
 *          Exercised by `Tests/TestFoliageImpostorBaker.cpp` (CPU-side
 *          layout math). See `FoliageRenderer::UploadToSceneBuffer()` for
 *          the matching runtime instance-upload path, which is also only
 *          active on Windows builds and currently unused from the main
 *          render loop.
 *
 * @threadsafety All static methods are pure functions — safe to call from
 *               any thread.
 */

#pragma once

#include <cstdint>
#include <vector>

namespace Spark::Graphics
{

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

} // namespace Spark::Graphics
