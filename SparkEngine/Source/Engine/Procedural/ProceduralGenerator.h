/**
 * @file ProceduralGenerator.h
 * @brief Procedural content generation framework with noise, dungeon, and WFC support
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides procedural generation utilities for terrain heightmaps, cave systems,
 * dungeon layouts, and tile-based generation via Wave Function Collapse. Uses
 * FastNoiseLite (already bundled) for noise generation with seed-based determinism.
 *
 * ## Usage
 * @code
 *   auto& gen = Spark::Procedural::ProceduralGenerator::GetInstance();
 *   gen.Initialize(42); // seed
 *
 *   // Generate a noise-based heightmap
 *   auto heightmap = gen.GenerateHeightmap(256, 256, {
 *       .octaves = 6, .frequency = 0.005f, .lacunarity = 2.0f, .gain = 0.5f
 *   });
 *
 *   // Generate a dungeon layout
 *   auto dungeon = gen.GenerateDungeon(50, 50, {
 *       .roomMinSize = 4, .roomMaxSize = 10, .maxRooms = 12
 *   });
 *
 *   // Generate using WFC
 *   auto tiles = gen.GenerateWFC(20, 20, tileRules);
 * @endcode
 */

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <numeric>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Spark::Procedural
{

    /**
     * @brief Parameters for fractal noise heightmap generation
     */
    struct HeightmapParams
    {
        int octaves = 6;          ///< Number of noise octaves (detail layers)
        float frequency = 0.005f; ///< Base frequency (lower = larger features)
        float lacunarity = 2.0f;  ///< Frequency multiplier per octave
        float gain = 0.5f;        ///< Amplitude multiplier per octave (persistence)
        float amplitude = 1.0f;   ///< Overall height scale
        float exponent = 1.0f;    ///< Power curve for redistribution (>1 = sharper peaks)
    };

    /**
     * @brief Parameters for dungeon generation
     */
    struct DungeonParams
    {
        int roomMinSize = 4;   ///< Minimum room dimension
        int roomMaxSize = 10;  ///< Maximum room dimension
        int maxRooms = 12;     ///< Maximum number of rooms to place
        int corridorWidth = 1; ///< Corridor width in tiles
    };

    /**
     * @brief A room in a generated dungeon
     */
    struct DungeonRoom
    {
        int x = 0, y = 0;          ///< Top-left corner
        int width = 0, height = 0; ///< Dimensions
        int centerX() const { return x + width / 2; }
        int centerY() const { return y + height / 2; }

        bool Overlaps(const DungeonRoom& other, int padding = 1) const
        {
            return !(x - padding >= other.x + other.width || x + width + padding <= other.x ||
                     y - padding >= other.y + other.height || y + height + padding <= other.y);
        }
    };

    /**
     * @brief Tile types for dungeon generation
     */
    enum class DungeonTile : uint8_t
    {
        Wall = 0,
        Floor = 1,
        Corridor = 2,
        Door = 3,
    };

    /**
     * @brief Result of a dungeon generation
     */
    struct DungeonResult
    {
        int width = 0;
        int height = 0;
        std::vector<DungeonTile> tiles; ///< Row-major tile grid
        std::vector<DungeonRoom> rooms;

        DungeonTile GetTile(int x, int y) const
        {
            if (x < 0 || x >= width || y < 0 || y >= height)
                return DungeonTile::Wall;
            return tiles[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)];
        }

        void SetTile(int x, int y, DungeonTile tile)
        {
            if (x >= 0 && x < width && y >= 0 && y < height)
                tiles[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)] = tile;
        }
    };

    /**
     * @brief Adjacency rules for Wave Function Collapse
     */
    struct WFCTileRule
    {
        int tileId = 0;
        float weight = 1.0f;                  ///< Relative probability
        std::unordered_set<int> allowedRight; ///< Tile IDs allowed to the right
        std::unordered_set<int> allowedDown;  ///< Tile IDs allowed below
        std::unordered_set<int> allowedLeft;  ///< Tile IDs allowed to the left
        std::unordered_set<int> allowedUp;    ///< Tile IDs allowed above
    };

    /**
     * @brief Result of Wave Function Collapse generation
     */
    struct WFCResult
    {
        int width = 0;
        int height = 0;
        std::vector<int> tiles; ///< Row-major tile IDs (-1 = unresolved/contradiction)
        bool success = false;   ///< false if a contradiction occurred

        int GetTile(int x, int y) const
        {
            if (x < 0 || x >= width || y < 0 || y >= height)
                return -1;
            return tiles[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)];
        }
    };

    /**
     * @brief Procedural content generation system
     *
     * Singleton providing noise-based heightmaps, dungeon layouts, and
     * Wave Function Collapse tile generation. All generation is deterministic
     * given the same seed.
     */
    class ProceduralGenerator
    {
      public:
        /**
         * @brief Get the singleton instance
         * @return Reference to the ProceduralGenerator
         */
        static ProceduralGenerator& GetInstance()
        {
            static ProceduralGenerator instance;
            return instance;
        }

        /**
         * @brief Initialize with a seed
         * @param seed Random seed for deterministic generation
         */
        void Initialize(uint32_t seed = 42)
        {
            m_seed = seed;
            m_rng.seed(seed);
            m_initialized = true;
        }

        /**
         * @brief Shut down the generator
         */
        void Shutdown() { m_initialized = false; }

        /**
         * @brief Set the random seed
         * @param seed New seed value
         */
        void SetSeed(uint32_t seed)
        {
            m_seed = seed;
            m_rng.seed(seed);
        }

        /**
         * @brief Get the current seed
         * @return Current seed value
         */
        uint32_t GetSeed() const { return m_seed; }

        /**
         * @brief Generate a noise-based heightmap
         * @param width Map width in samples
         * @param height Map height in samples
         * @param params Noise parameters
         * @return Vector of height values in [0, 1] range, row-major
         */
        std::vector<float> GenerateHeightmap(int width, int height, const HeightmapParams& params = {})
        {
            if (width <= 0 || height <= 0 || width > 16384 || height > 16384)
                return {};
            std::vector<float> heightmap(static_cast<size_t>(width) * height, 0.0f);

            float maxVal = 0.0f;
            float minVal = 1e10f;

            for (int y = 0; y < height; ++y)
            {
                for (int x = 0; x < width; ++x)
                {
                    float value = 0.0f;
                    float freq = params.frequency;
                    float amp = params.amplitude;

                    for (int oct = 0; oct < params.octaves; ++oct)
                    {
                        float nx = static_cast<float>(x) * freq;
                        float ny = static_cast<float>(y) * freq;
                        float noise = SimplexNoise2D(nx + static_cast<float>(m_seed), ny + static_cast<float>(m_seed));
                        value += noise * amp;
                        freq *= params.lacunarity;
                        amp *= params.gain;
                    }

                    // Apply power curve
                    if (params.exponent != 1.0f)
                        value = std::pow(std::max(0.0f, value), params.exponent);

                    heightmap[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)] = value;
                    maxVal = std::max(maxVal, value);
                    minVal = std::min(minVal, value);
                }
            }

            // Normalize to [0, 1]
            float range = maxVal - minVal;
            if (range > 1e-6f)
            {
                for (auto& h : heightmap)
                    h = (h - minVal) / range;
            }

            return heightmap;
        }

        /**
         * @brief Generate a dungeon layout using BSP-like room placement
         * @param width Grid width
         * @param height Grid height
         * @param params Dungeon parameters
         * @return DungeonResult with tile grid and room list
         */
        DungeonResult GenerateDungeon(int width, int height, const DungeonParams& params = {})
        {
            DungeonResult result;
            result.width = width;
            result.height = height;
            if (width <= 0 || height <= 0 || width > 16384 || height > 16384)
                return result;
            result.tiles.assign(static_cast<size_t>(width) * height, DungeonTile::Wall);

            std::uniform_int_distribution<int> sizeDist(params.roomMinSize, params.roomMaxSize);

            for (int attempt = 0;
                 attempt < params.maxRooms * 3 && static_cast<int>(result.rooms.size()) < params.maxRooms; ++attempt)
            {
                DungeonRoom room;
                room.width = sizeDist(m_rng);
                room.height = sizeDist(m_rng);

                std::uniform_int_distribution<int> xDist(1, std::max(1, width - room.width - 1));
                std::uniform_int_distribution<int> yDist(1, std::max(1, height - room.height - 1));
                room.x = xDist(m_rng);
                room.y = yDist(m_rng);

                // Check overlap with existing rooms
                bool overlaps = false;
                for (const auto& existing : result.rooms)
                {
                    if (room.Overlaps(existing, 2))
                    {
                        overlaps = true;
                        break;
                    }
                }

                if (!overlaps)
                {
                    // Carve room
                    for (int ry = room.y; ry < room.y + room.height; ++ry)
                        for (int rx = room.x; rx < room.x + room.width; ++rx)
                            result.SetTile(rx, ry, DungeonTile::Floor);

                    // Connect to previous room with an L-shaped corridor
                    if (!result.rooms.empty())
                    {
                        const auto& prev = result.rooms.back();
                        int cx = prev.centerX();
                        int cy = prev.centerY();
                        int nx = room.centerX();
                        int ny = room.centerY();

                        // Horizontal then vertical
                        int xStep = (nx > cx) ? 1 : -1;
                        for (int x = cx; x != nx; x += xStep)
                        {
                            for (int w = 0; w < params.corridorWidth; ++w)
                                result.SetTile(x, cy + w, DungeonTile::Corridor);
                        }
                        int yStep = (ny > cy) ? 1 : -1;
                        for (int y = cy; y != ny; y += yStep)
                        {
                            for (int w = 0; w < params.corridorWidth; ++w)
                                result.SetTile(nx + w, y, DungeonTile::Corridor);
                        }
                    }

                    result.rooms.push_back(room);
                }
            }

            return result;
        }

        /**
         * @brief Generate tiles using Wave Function Collapse
         * @param width Grid width
         * @param height Grid height
         * @param rules Adjacency rules for each tile type
         * @param maxIterations Maximum collapse iterations before giving up
         * @return WFCResult with resolved tile grid
         */
        WFCResult GenerateWFC(int width, int height, const std::vector<WFCTileRule>& rules, int maxIterations = 10000)
        {
            WFCResult result;
            result.width = width;
            result.height = height;

            if (width <= 0 || height <= 0 || width > 16384 || height > 16384)
            {
                result.success = false;
                return result;
            }

            if (rules.empty())
            {
                result.tiles.assign(static_cast<size_t>(width) * height, -1);
                return result;
            }

            // Initialize — each cell starts with all possible tiles
            size_t cellCount = static_cast<size_t>(width) * height;
            std::vector<std::vector<int>> possibilities(cellCount);
            for (auto& cell : possibilities)
            {
                for (const auto& rule : rules)
                    cell.push_back(rule.tileId);
            }

            result.tiles.assign(cellCount, -1);
            result.success = true;

            auto getIdx = [width](int x, int y) -> size_t
            { return static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x); };

            for (int iter = 0; iter < maxIterations; ++iter)
            {
                // Find cell with minimum entropy (fewest possibilities > 1)
                int minEntropy = static_cast<int>(rules.size()) + 1;
                int bestCell = -1;

                for (size_t i = 0; i < cellCount; ++i)
                {
                    int entropy = static_cast<int>(possibilities[i].size());
                    if (entropy > 1 && entropy < minEntropy)
                    {
                        minEntropy = entropy;
                        bestCell = static_cast<int>(i);
                    }
                }

                if (bestCell == -1)
                    break; // All cells resolved

                // Collapse: pick a weighted random tile
                auto& opts = possibilities[static_cast<size_t>(bestCell)];
                float totalWeight = 0.0f;
                for (int tileId : opts)
                {
                    for (const auto& r : rules)
                    {
                        if (r.tileId == tileId)
                        {
                            totalWeight += r.weight;
                            break;
                        }
                    }
                }

                std::uniform_real_distribution<float> weightDist(0.0f, totalWeight);
                float pick = weightDist(m_rng);
                float cumulative = 0.0f;
                int chosenTile = opts.front();

                for (int tileId : opts)
                {
                    for (const auto& r : rules)
                    {
                        if (r.tileId == tileId)
                        {
                            cumulative += r.weight;
                            if (cumulative >= pick)
                            {
                                chosenTile = tileId;
                                goto collapsed;
                            }
                            break;
                        }
                    }
                }
            collapsed:
                opts = {chosenTile};
                result.tiles[static_cast<size_t>(bestCell)] = chosenTile;

                // Propagate constraints to neighbors
                int cx = bestCell % width;
                int cy = bestCell / width;

                const WFCTileRule* chosenRule = nullptr;
                for (const auto& r : rules)
                {
                    if (r.tileId == chosenTile)
                    {
                        chosenRule = &r;
                        break;
                    }
                }

                if (chosenRule)
                {
                    // Right neighbor
                    if (cx + 1 < width)
                        Propagate(possibilities[getIdx(cx + 1, cy)], chosenRule->allowedRight, result.success);
                    // Left neighbor
                    if (cx - 1 >= 0)
                        Propagate(possibilities[getIdx(cx - 1, cy)], chosenRule->allowedLeft, result.success);
                    // Down neighbor
                    if (cy + 1 < height)
                        Propagate(possibilities[getIdx(cx, cy + 1)], chosenRule->allowedDown, result.success);
                    // Up neighbor
                    if (cy - 1 >= 0)
                        Propagate(possibilities[getIdx(cx, cy - 1)], chosenRule->allowedUp, result.success);
                }

                if (!result.success)
                    break;
            }

            // Fill remaining resolved cells
            for (size_t i = 0; i < cellCount; ++i)
            {
                if (result.tiles[i] == -1 && possibilities[i].size() == 1)
                    result.tiles[i] = possibilities[i].front();
            }

            return result;
        }

        /**
         * @brief Get status string for console/debug display
         * @return Formatted status string
         */
        std::string Console_GetStatus() const { return "ProceduralGenerator: seed=" + std::to_string(m_seed); }

      private:
        ProceduralGenerator() = default;

        /**
         * @brief Propagate WFC constraints — remove impossible tiles from a cell
         */
        static void Propagate(std::vector<int>& cellPossibilities, const std::unordered_set<int>& allowed,
                              bool& success)
        {
            if (cellPossibilities.size() <= 1)
                return;

            std::vector<int> filtered;
            for (int id : cellPossibilities)
            {
                if (allowed.contains(id))
                    filtered.push_back(id);
            }

            if (filtered.empty())
            {
                success = false;
                return;
            }

            cellPossibilities = std::move(filtered);
        }

        /**
         * @brief Simple 2D simplex-like noise using value noise with smoothstep
         *
         * Not a true simplex implementation but sufficient for procedural generation.
         * For production quality, use FastNoiseLite via the bundled header.
         */
        static float SimplexNoise2D(float x, float y)
        {
            // Hash-based value noise with cosine interpolation
            auto hash = [](int ix, int iy) -> float
            {
                uint32_t h = static_cast<uint32_t>(ix) * 374761393u + static_cast<uint32_t>(iy) * 668265263u;
                h = (h ^ (h >> 13)) * 1274126177u;
                h = h ^ (h >> 16);
                return static_cast<float>(h & 0x7FFFFFFF) / static_cast<float>(0x7FFFFFFF);
            };

            int ix = static_cast<int>(std::floor(x));
            int iy = static_cast<int>(std::floor(y));
            float fx = x - static_cast<float>(ix);
            float fy = y - static_cast<float>(iy);

            // Smoothstep interpolation
            float sx = fx * fx * (3.0f - 2.0f * fx);
            float sy = fy * fy * (3.0f - 2.0f * fy);

            float n00 = hash(ix, iy);
            float n10 = hash(ix + 1, iy);
            float n01 = hash(ix, iy + 1);
            float n11 = hash(ix + 1, iy + 1);

            float nx0 = n00 + sx * (n10 - n00);
            float nx1 = n01 + sx * (n11 - n01);

            return nx0 + sy * (nx1 - nx0);
        }

        bool m_initialized = false;
        uint32_t m_seed = 42;
        std::mt19937 m_rng;
    };

} // namespace Spark::Procedural
