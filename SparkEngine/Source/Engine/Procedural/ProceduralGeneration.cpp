/**
 * @file ProceduralGeneration.cpp
 * @brief Procedural generation implementation — noise, meshes, placement, WFC
 */

#include "ProceduralGeneration.h"
#include <sstream>
#include <cmath>
#include <algorithm>
#include <numeric>
#include <queue>

using namespace DirectX;
namespace Spark::Procedural
{

    // ============================================================================
    // NoiseGenerator
    // ============================================================================

    NoiseGenerator::NoiseGenerator(uint32_t seed)
    {
        SetSeed(seed);
    }

    void NoiseGenerator::SetSeed(uint32_t seed)
    {
        m_rng.seed(seed);
        m_permutation.resize(512);
        std::vector<int> p(256);
        std::iota(p.begin(), p.end(), 0);
        std::shuffle(p.begin(), p.end(), m_rng);
        for (int i = 0; i < 256; ++i)
        {
            m_permutation[i] = p[i];
            m_permutation[i + 256] = p[i];
        }
    }

    float NoiseGenerator::Fade(float t) const
    {
        return t * t * t * (t * (t * 6 - 15) + 10);
    }
    float NoiseGenerator::Lerp(float a, float b, float t) const
    {
        return a + t * (b - a);
    }

    float NoiseGenerator::Grad2D(int hash, float x, float y) const
    {
        int h = hash & 3;
        float u = h < 2 ? x : y;
        float v = h < 2 ? y : x;
        return ((h & 1) ? -u : u) + ((h & 2) ? -v : v);
    }

    float NoiseGenerator::Grad3D(int hash, float x, float y, float z) const
    {
        int h = hash & 15;
        float u = h < 8 ? x : y;
        float v = h < 4 ? y : (h == 12 || h == 14 ? x : z);
        return ((h & 1) ? -u : u) + ((h & 2) ? -v : v);
    }

    float NoiseGenerator::Perlin2D(float x, float y) const
    {
        int xi = static_cast<int>(std::floor(x)) & 255;
        int yi = static_cast<int>(std::floor(y)) & 255;
        float xf = x - std::floor(x);
        float yf = y - std::floor(y);
        float u = Fade(xf);
        float v = Fade(yf);

        int aa = m_permutation[m_permutation[xi] + yi];
        int ab = m_permutation[m_permutation[xi] + yi + 1];
        int ba = m_permutation[m_permutation[xi + 1] + yi];
        int bb = m_permutation[m_permutation[xi + 1] + yi + 1];

        float x1 = Lerp(Grad2D(aa, xf, yf), Grad2D(ba, xf - 1, yf), u);
        float x2 = Lerp(Grad2D(ab, xf, yf - 1), Grad2D(bb, xf - 1, yf - 1), u);
        return Lerp(x1, x2, v);
    }

    float NoiseGenerator::Perlin3D(float x, float y, float z) const
    {
        int xi = static_cast<int>(std::floor(x)) & 255;
        int yi = static_cast<int>(std::floor(y)) & 255;
        int zi = static_cast<int>(std::floor(z)) & 255;
        float xf = x - std::floor(x);
        float yf = y - std::floor(y);
        float zf = z - std::floor(z);
        float u = Fade(xf), v = Fade(yf), w = Fade(zf);

        int aaa = m_permutation[m_permutation[m_permutation[xi] + yi] + zi];
        int aba = m_permutation[m_permutation[m_permutation[xi] + yi + 1] + zi];
        int aab = m_permutation[m_permutation[m_permutation[xi] + yi] + zi + 1];
        int abb = m_permutation[m_permutation[m_permutation[xi] + yi + 1] + zi + 1];
        int baa = m_permutation[m_permutation[m_permutation[xi + 1] + yi] + zi];
        int bba = m_permutation[m_permutation[m_permutation[xi + 1] + yi + 1] + zi];
        int bab = m_permutation[m_permutation[m_permutation[xi + 1] + yi] + zi + 1];
        int bbb = m_permutation[m_permutation[m_permutation[xi + 1] + yi + 1] + zi + 1];

        float x1 = Lerp(Grad3D(aaa, xf, yf, zf), Grad3D(baa, xf - 1, yf, zf), u);
        float x2 = Lerp(Grad3D(aba, xf, yf - 1, zf), Grad3D(bba, xf - 1, yf - 1, zf), u);
        float y1 = Lerp(x1, x2, v);
        float x3 = Lerp(Grad3D(aab, xf, yf, zf - 1), Grad3D(bab, xf - 1, yf, zf - 1), u);
        float x4 = Lerp(Grad3D(abb, xf, yf - 1, zf - 1), Grad3D(bbb, xf - 1, yf - 1, zf - 1), u);
        float y2 = Lerp(x3, x4, v);
        return Lerp(y1, y2, w);
    }

    float NoiseGenerator::Simplex2D(float x, float y) const
    {
        // Simplified — delegate to Perlin2D for now (true Simplex would use a triangular grid)
        return Perlin2D(x * 1.2f, y * 1.2f);
    }

    float NoiseGenerator::Simplex3D(float x, float y, float z) const
    {
        return Perlin3D(x * 1.2f, y * 1.2f, z * 1.2f);
    }

    float NoiseGenerator::Worley2D(float x, float y, int cellCount) const
    {
        float cellX = x * cellCount;
        float cellY = y * cellCount;
        int ix = static_cast<int>(std::floor(cellX));
        int iy = static_cast<int>(std::floor(cellY));
        float minDist = 1e30f;

        for (int dx = -1; dx <= 1; ++dx)
        {
            for (int dy = -1; dy <= 1; ++dy)
            {
                int cx = ix + dx, cy = iy + dy;
                // Deterministic random point in cell
                int h = m_permutation[(m_permutation[cx & 255] + cy) & 255];
                float px = cx + (h & 0xF) / 16.0f;
                float py = cy + ((h >> 4) & 0xF) / 16.0f;
                float dist = (cellX - px) * (cellX - px) + (cellY - py) * (cellY - py);
                minDist = (std::min)(minDist, dist);
            }
        }
        return std::sqrt(minDist);
    }

    float NoiseGenerator::FBM(float x, float y, int octaves, float lacunarity, float persistence) const
    {
        float total = 0.0f, amplitude = 1.0f, frequency = 1.0f, maxVal = 0.0f;
        for (int i = 0; i < octaves; ++i)
        {
            total += Perlin2D(x * frequency, y * frequency) * amplitude;
            maxVal += amplitude;
            amplitude *= persistence;
            frequency *= lacunarity;
        }
        return total / maxVal;
    }

    float NoiseGenerator::RidgedMultifractal(float x, float y, int octaves, float lacunarity, float gain) const
    {
        float total = 0.0f, amplitude = 1.0f, frequency = 1.0f, weight = 1.0f;
        for (int i = 0; i < octaves; ++i)
        {
            float signal = std::abs(Perlin2D(x * frequency, y * frequency));
            signal = 1.0f - signal;
            signal *= signal;
            signal *= weight;
            weight = (std::min)(1.0f, signal * gain);
            total += signal * amplitude;
            amplitude *= 0.5f;
            frequency *= lacunarity;
        }
        return total;
    }

    float NoiseGenerator::WarpedNoise(float x, float y, float warpStrength) const
    {
        float wx = FBM(x + 0.0f, y + 0.0f, 4);
        float wy = FBM(x + 5.2f, y + 1.3f, 4);
        return FBM(x + wx * warpStrength, y + wy * warpStrength, 6);
    }

    // ============================================================================
    // HeightmapGenerator
    // ============================================================================

    std::vector<float> HeightmapGenerator::Generate(const HeightmapSettings& settings)
    {
        NoiseGenerator noise(settings.seed);
        size_t size = static_cast<size_t>(settings.width) * settings.height;
        std::vector<float> heightmap(size);

        for (int y = 0; y < settings.height; ++y)
        {
            for (int x = 0; x < settings.width; ++x)
            {
                float nx = static_cast<float>(x) / settings.scale;
                float ny = static_cast<float>(y) / settings.scale;
                float h = noise.FBM(nx, ny, settings.octaves, settings.lacunarity, settings.persistence);
                h = (h + 1.0f) * 0.5f; // Normalize to 0-1
                heightmap[y * settings.width + x] = h * settings.heightMultiplier;
            }
        }

        if (settings.applyErosion)
        {
            ApplyHydraulicErosion(heightmap, settings.width, settings.height, settings.erosionIterations,
                                  settings.seed);
        }

        return heightmap;
    }

    void HeightmapGenerator::ApplyThermalErosion(std::vector<float>& heightmap, int width, int height, int iterations,
                                                 float talus)
    {
        for (int iter = 0; iter < iterations; ++iter)
        {
            for (int y = 1; y < height - 1; ++y)
            {
                for (int x = 1; x < width - 1; ++x)
                {
                    float h = heightmap[y * width + x];
                    float maxDiff = 0.0f;
                    int bestDx = 0, bestDy = 0;

                    for (int dy = -1; dy <= 1; ++dy)
                    {
                        for (int dx = -1; dx <= 1; ++dx)
                        {
                            if (dx == 0 && dy == 0)
                                continue;
                            float nh = heightmap[(y + dy) * width + (x + dx)];
                            float diff = h - nh;
                            if (diff > maxDiff)
                            {
                                maxDiff = diff;
                                bestDx = dx;
                                bestDy = dy;
                            }
                        }
                    }

                    if (maxDiff > talus)
                    {
                        float transfer = maxDiff * 0.5f;
                        heightmap[y * width + x] -= transfer;
                        heightmap[(y + bestDy) * width + (x + bestDx)] += transfer;
                    }
                }
            }
        }
    }

    void HeightmapGenerator::ApplyHydraulicErosion(std::vector<float>& heightmap, int width, int height, int iterations,
                                                   uint32_t seed)
    {
        std::mt19937 rng(seed);
        std::uniform_real_distribution<float> distX(0.0f, static_cast<float>(width - 1));
        std::uniform_real_distribution<float> distY(0.0f, static_cast<float>(height - 1));

        for (int iter = 0; iter < iterations; ++iter)
        {
            float posX = distX(rng);
            float posY = distY(rng);
            float sediment = 0.0f;
            float water = 1.0f;
            float speed = 1.0f;

            for (int step = 0; step < 30 && water > 0.01f; ++step)
            {
                int ix = static_cast<int>(posX), iy = static_cast<int>(posY);
                if (ix < 1 || ix >= width - 1 || iy < 1 || iy >= height - 1)
                    break;

                // Compute gradient
                float h = heightmap[iy * width + ix];
                float gx = heightmap[iy * width + ix + 1] - heightmap[iy * width + ix - 1];
                float gy = heightmap[(iy + 1) * width + ix] - heightmap[(iy - 1) * width + ix];

                float gradLen = std::sqrt(gx * gx + gy * gy);
                if (gradLen < 0.0001f)
                    break;

                posX -= gx / gradLen;
                posY -= gy / gradLen;

                int nix = static_cast<int>(posX), niy = static_cast<int>(posY);
                if (nix < 0 || nix >= width || niy < 0 || niy >= height)
                    break;

                float newH = heightmap[niy * width + nix];
                float heightDiff = h - newH;

                float capacity = (std::max)(heightDiff * speed * water * 4.0f, 0.01f);

                if (sediment > capacity)
                {
                    float deposit = (sediment - capacity) * 0.3f;
                    heightmap[iy * width + ix] += deposit;
                    sediment -= deposit;
                }
                else
                {
                    float erode = (std::min)((capacity - sediment) * 0.3f, heightDiff);
                    heightmap[iy * width + ix] -= erode;
                    sediment += erode;
                }

                speed = std::sqrt(speed * speed + heightDiff);
                water *= 0.99f;
            }
        }
    }

    void HeightmapGenerator::Normalize(std::vector<float>& heightmap)
    {
        if (heightmap.empty())
            return;
        float minH = *std::min_element(heightmap.begin(), heightmap.end());
        float maxH = *std::max_element(heightmap.begin(), heightmap.end());
        float range = maxH - minH;
        if (range < 0.0001f)
            return;
        for (auto& h : heightmap)
            h = (h - minH) / range;
    }

    // ============================================================================
    // ProceduralMesh
    // ============================================================================

    ProceduralMeshData ProceduralMesh::CreatePlane(float width, float depth, int subdivX, int subdivZ)
    {
        ProceduralMeshData mesh;
        float halfW = width * 0.5f, halfD = depth * 0.5f;

        for (int z = 0; z <= subdivZ; ++z)
        {
            for (int x = 0; x <= subdivX; ++x)
            {
                float u = static_cast<float>(x) / subdivX;
                float v = static_cast<float>(z) / subdivZ;
                mesh.vertices.push_back({{-halfW + u * width, 0.0f, -halfD + v * depth}, {0, 1, 0}, {u, v}});
            }
        }

        for (int z = 0; z < subdivZ; ++z)
        {
            for (int x = 0; x < subdivX; ++x)
            {
                uint32_t tl = static_cast<uint32_t>(z * (subdivX + 1) + x);
                uint32_t tr = tl + 1;
                uint32_t bl = static_cast<uint32_t>((z + 1) * (subdivX + 1) + x);
                uint32_t br = bl + 1;
                mesh.indices.insert(mesh.indices.end(), {tl, bl, tr, tr, bl, br});
            }
        }
        return mesh;
    }

    ProceduralMeshData ProceduralMesh::CreateSphere(float radius, int slices, int stacks)
    {
        ProceduralMeshData mesh;
        const float PI = 3.14159265f;

        for (int st = 0; st <= stacks; ++st)
        {
            float phi = PI * st / stacks;
            for (int sl = 0; sl <= slices; ++sl)
            {
                float theta = 2.0f * PI * sl / slices;
                float x = std::sin(phi) * std::cos(theta);
                float y = std::cos(phi);
                float z = std::sin(phi) * std::sin(theta);
                mesh.vertices.push_back({{x * radius, y * radius, z * radius},
                                         {x, y, z},
                                         {static_cast<float>(sl) / slices, static_cast<float>(st) / stacks}});
            }
        }

        for (int st = 0; st < stacks; ++st)
        {
            for (int sl = 0; sl < slices; ++sl)
            {
                uint32_t a = static_cast<uint32_t>(st * (slices + 1) + sl);
                uint32_t b = a + static_cast<uint32_t>(slices) + 1;
                mesh.indices.insert(mesh.indices.end(), {a, b, a + 1, a + 1, b, b + 1});
            }
        }
        return mesh;
    }

    ProceduralMeshData ProceduralMesh::CreateBox(float width, float height, float depth, int)
    {
        ProceduralMeshData mesh;
        float hw = width * 0.5f, hh = height * 0.5f, hd = depth * 0.5f;

        // 6 faces, 4 vertices each
        auto addFace = [&](XMFLOAT3 p0, XMFLOAT3 p1, XMFLOAT3 p2, XMFLOAT3 p3, XMFLOAT3 n)
        {
            uint32_t base = static_cast<uint32_t>(mesh.vertices.size());
            mesh.vertices.push_back({p0, n, {0, 0}});
            mesh.vertices.push_back({p1, n, {1, 0}});
            mesh.vertices.push_back({p2, n, {1, 1}});
            mesh.vertices.push_back({p3, n, {0, 1}});
            mesh.indices.insert(mesh.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
        };

        addFace({-hw, hh, -hd}, {hw, hh, -hd}, {hw, hh, hd}, {-hw, hh, hd}, {0, 1, 0});      // Top
        addFace({-hw, -hh, hd}, {hw, -hh, hd}, {hw, -hh, -hd}, {-hw, -hh, -hd}, {0, -1, 0}); // Bottom
        addFace({-hw, -hh, -hd}, {hw, -hh, -hd}, {hw, hh, -hd}, {-hw, hh, -hd}, {0, 0, -1}); // Front
        addFace({hw, -hh, hd}, {-hw, -hh, hd}, {-hw, hh, hd}, {hw, hh, hd}, {0, 0, 1});      // Back
        addFace({-hw, -hh, hd}, {-hw, -hh, -hd}, {-hw, hh, -hd}, {-hw, hh, hd}, {-1, 0, 0}); // Left
        addFace({hw, -hh, -hd}, {hw, -hh, hd}, {hw, hh, hd}, {hw, hh, -hd}, {1, 0, 0});      // Right

        return mesh;
    }

    ProceduralMeshData ProceduralMesh::CreateCylinder(float radius, float height, int slices)
    {
        ProceduralMeshData mesh;
        const float PI = 3.14159265f;
        float halfH = height * 0.5f;

        // Side vertices
        for (int i = 0; i <= slices; ++i)
        {
            float theta = 2.0f * PI * i / slices;
            float x = std::cos(theta), z = std::sin(theta);
            mesh.vertices.push_back({{x * radius, halfH, z * radius}, {x, 0, z}, {static_cast<float>(i) / slices, 0}});
            mesh.vertices.push_back({{x * radius, -halfH, z * radius}, {x, 0, z}, {static_cast<float>(i) / slices, 1}});
        }

        for (int i = 0; i < slices; ++i)
        {
            uint32_t a = static_cast<uint32_t>(i * 2);
            mesh.indices.insert(mesh.indices.end(), {a, a + 1, a + 2, a + 2, a + 1, a + 3});
        }

        return mesh;
    }

    ProceduralMeshData ProceduralMesh::CreateCone(float radius, float height, int slices)
    {
        ProceduralMeshData mesh;
        const float PI = 3.14159265f;

        // Apex
        mesh.vertices.push_back({{0, height, 0}, {0, 1, 0}, {0.5f, 0}});

        for (int i = 0; i <= slices; ++i)
        {
            float theta = 2.0f * PI * i / slices;
            float x = std::cos(theta), z = std::sin(theta);
            mesh.vertices.push_back({{x * radius, 0, z * radius}, {x, 0, z}, {static_cast<float>(i) / slices, 1}});
        }

        for (int i = 1; i <= slices; ++i)
        {
            mesh.indices.insert(mesh.indices.end(), {0, static_cast<uint32_t>(i + 1), static_cast<uint32_t>(i)});
        }

        return mesh;
    }

    ProceduralMeshData ProceduralMesh::CreateTorus(float majorR, float minorR, int slices, int stacks)
    {
        ProceduralMeshData mesh;
        const float PI = 3.14159265f;

        for (int i = 0; i <= slices; ++i)
        {
            float theta = 2.0f * PI * i / slices;
            for (int j = 0; j <= stacks; ++j)
            {
                float phi = 2.0f * PI * j / stacks;
                float x = (majorR + minorR * std::cos(phi)) * std::cos(theta);
                float y = minorR * std::sin(phi);
                float z = (majorR + minorR * std::cos(phi)) * std::sin(theta);
                float nx = std::cos(phi) * std::cos(theta);
                float ny = std::sin(phi);
                float nz = std::cos(phi) * std::sin(theta);
                mesh.vertices.push_back(
                    {{x, y, z}, {nx, ny, nz}, {static_cast<float>(i) / slices, static_cast<float>(j) / stacks}});
            }
        }

        for (int i = 0; i < slices; ++i)
        {
            for (int j = 0; j < stacks; ++j)
            {
                uint32_t a = static_cast<uint32_t>(i * (stacks + 1) + j);
                uint32_t b = a + static_cast<uint32_t>(stacks) + 1;
                mesh.indices.insert(mesh.indices.end(), {a, b, a + 1, a + 1, b, b + 1});
            }
        }
        return mesh;
    }

    ProceduralMeshData ProceduralMesh::CreateTerrainFromHeightmap(const float* heightmap, int width, int height,
                                                                  float cellSize, float heightScale)
    {
        ProceduralMeshData mesh;

        for (int z = 0; z < height; ++z)
        {
            for (int x = 0; x < width; ++x)
            {
                float h = heightmap[z * width + x] * heightScale;
                float u = static_cast<float>(x) / (width - 1);
                float v = static_cast<float>(z) / (height - 1);

                XMFLOAT3 normal = {0, 1, 0};
                if (x > 0 && x < width - 1 && z > 0 && z < height - 1)
                {
                    float hL = heightmap[z * width + (x - 1)] * heightScale;
                    float hR = heightmap[z * width + (x + 1)] * heightScale;
                    float hD = heightmap[(z - 1) * width + x] * heightScale;
                    float hU = heightmap[(z + 1) * width + x] * heightScale;
                    normal = {(hL - hR) / (2 * cellSize), 1.0f, (hD - hU) / (2 * cellSize)};
                    float len = std::sqrt(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
                    if (len > 0)
                    {
                        normal.x /= len;
                        normal.y /= len;
                        normal.z /= len;
                    }
                }

                mesh.vertices.push_back({{x * cellSize, h, z * cellSize}, normal, {u, v}});
            }
        }

        for (int z = 0; z < height - 1; ++z)
        {
            for (int x = 0; x < width - 1; ++x)
            {
                uint32_t tl = static_cast<uint32_t>(z * width + x);
                uint32_t tr = tl + 1;
                uint32_t bl = static_cast<uint32_t>((z + 1) * width + x);
                uint32_t br = bl + 1;
                mesh.indices.insert(mesh.indices.end(), {tl, bl, tr, tr, bl, br});
            }
        }
        return mesh;
    }

    ProceduralMeshData ProceduralMesh::CreateRock(float radius, int detail, uint32_t seed)
    {
        auto sphere = CreateSphere(radius, detail, detail);
        NoiseGenerator noise(seed);
        for (auto& v : sphere.vertices)
        {
            float n = noise.FBM(v.position.x * 2, v.position.y * 2, 4) * 0.3f;
            v.position.x *= (1.0f + n);
            v.position.y *= (1.0f + n);
            v.position.z *= (1.0f + n);
        }
        return sphere;
    }

    ProceduralMeshData ProceduralMesh::CreateTree(float height, int branches, uint32_t seed)
    {
        // Simplified: trunk cylinder + cone top
        auto trunk = CreateCylinder(height * 0.05f, height * 0.6f, 8);
        // Offset trunk up
        for (auto& v : trunk.vertices)
            v.position.y += height * 0.3f;
        return trunk;
    }

    // ============================================================================
    // ObjectPlacer
    // ============================================================================

    std::vector<PlacementResult> ObjectPlacer::PlaceObjects(const float* heightmap, int width, int height,
                                                            float cellSize, const XMFLOAT3& origin,
                                                            const std::vector<PlacementRule>& rules, uint32_t seed)
    {

        std::vector<PlacementResult> results;
        std::mt19937 rng(seed);
        NoiseGenerator noise(seed);

        for (const auto& rule : rules)
        {
            float cellArea = cellSize * cellSize;
            float expectedCount = rule.density * cellArea * width * height;
            int count = static_cast<int>(expectedCount);

            std::uniform_real_distribution<float> distX(0.0f, static_cast<float>(width - 1));
            std::uniform_real_distribution<float> distY(0.0f, static_cast<float>(height - 1));
            std::uniform_real_distribution<float> distRot(0.0f, rule.rotationVariance);
            std::uniform_real_distribution<float> distScale(rule.scaleRange.x, rule.scaleRange.y);

            for (int i = 0; i < count; ++i)
            {
                float px = distX(rng);
                float py = distY(rng);
                int ix = static_cast<int>(px);
                int iy = static_cast<int>(py);
                if (ix < 0 || ix >= width || iy < 0 || iy >= height)
                    continue;

                float h = heightmap[iy * width + ix];
                float normalizedH = h / 100.0f; // Approximate normalization

                if (normalizedH < rule.minHeight || normalizedH > rule.maxHeight)
                    continue;

                // Noise mask
                if (rule.noiseMaskThreshold > 0.0f)
                {
                    float nm = noise.Perlin2D(px / rule.noiseMaskScale, py / rule.noiseMaskScale);
                    if (nm < rule.noiseMaskThreshold)
                        continue;
                }

                float s = distScale(rng);
                float rot = distRot(rng);

                PlacementResult result;
                result.objectType = rule.objectType;
                result.position = {origin.x + px * cellSize, origin.y + h, origin.z + py * cellSize};
                result.rotation = {0, rot, 0};
                result.scale = {s, s, s};
                results.push_back(result);
            }
        }
        return results;
    }

    // ============================================================================
    // Poisson Disk Sampling (Bridson's algorithm)
    // ============================================================================

    std::vector<XMFLOAT2> PoissonDiskSampling(float width, float height, float minDist, int maxAttempts, uint32_t seed)
    {
        std::mt19937 rng(seed);
        std::uniform_real_distribution<float> dist01(0.0f, 1.0f);

        float cellSize = minDist / std::sqrt(2.0f);
        int gridW = static_cast<int>(std::ceil(width / cellSize));
        int gridH = static_cast<int>(std::ceil(height / cellSize));

        // Background grid: -1 means empty, otherwise index into points
        std::vector<int> grid(gridW * gridH, -1);
        std::vector<XMFLOAT2> points;
        std::vector<int> activeList;

        // Helper to get grid cell for a point
        auto toGrid = [cellSize](float px, float py) -> std::pair<int, int>
        { return {static_cast<int>(px / cellSize), static_cast<int>(py / cellSize)}; };

        // Seed with first point
        float startX = width * dist01(rng);
        float startY = height * dist01(rng);
        points.push_back({startX, startY});
        activeList.push_back(0);
        auto [gx0, gy0] = toGrid(startX, startY);
        grid[gy0 * gridW + gx0] = 0;

        float minDistSq = minDist * minDist;

        while (!activeList.empty())
        {
            // Pick a random active point
            std::uniform_int_distribution<int> activeDist(0, static_cast<int>(activeList.size()) - 1);
            int activeIdx = activeDist(rng);
            int pointIdx = activeList[activeIdx];
            const auto& activePoint = points[pointIdx];

            bool found = false;
            for (int attempt = 0; attempt < maxAttempts; ++attempt)
            {
                // Generate candidate in annulus [minDist, 2*minDist]
                float angle = dist01(rng) * 2.0f * 3.14159265f;
                float radius = minDist + dist01(rng) * minDist;
                float cx = activePoint.x + radius * std::cos(angle);
                float cy = activePoint.y + radius * std::sin(angle);

                // Bounds check
                if (cx < 0 || cx >= width || cy < 0 || cy >= height)
                {
                    continue;
                }

                auto [gcx, gcy] = toGrid(cx, cy);

                // Check neighbors in a 5x5 grid window
                bool tooClose = false;
                for (int dy = -2; dy <= 2 && !tooClose; ++dy)
                {
                    for (int dx = -2; dx <= 2 && !tooClose; ++dx)
                    {
                        int nx = gcx + dx;
                        int ny = gcy + dy;
                        if (nx < 0 || nx >= gridW || ny < 0 || ny >= gridH)
                        {
                            continue;
                        }
                        int neighborIdx = grid[ny * gridW + nx];
                        if (neighborIdx >= 0)
                        {
                            float ddx = points[neighborIdx].x - cx;
                            float ddy = points[neighborIdx].y - cy;
                            if (ddx * ddx + ddy * ddy < minDistSq)
                            {
                                tooClose = true;
                            }
                        }
                    }
                }

                if (!tooClose)
                {
                    int newIdx = static_cast<int>(points.size());
                    points.push_back({cx, cy});
                    activeList.push_back(newIdx);
                    grid[gcy * gridW + gcx] = newIdx;
                    found = true;
                    break;
                }
            }

            if (!found)
            {
                // Remove from active list (swap with last)
                activeList[activeIdx] = activeList.back();
                activeList.pop_back();
            }
        }

        return points;
    }

    // ============================================================================
    // DungeonGenerator — BSP
    // ============================================================================

    void DungeonGenerator::SplitBSP(BSPNode* node, int minLeafSize, std::mt19937& rng)
    {
        if (node->width <= minLeafSize * 2 && node->height <= minLeafSize * 2)
        {
            return;
        }

        // Decide split direction based on aspect ratio
        bool splitHorizontal;
        if (node->width > node->height && node->width > minLeafSize * 2)
        {
            splitHorizontal = false; // Split vertically
        }
        else if (node->height > node->width && node->height > minLeafSize * 2)
        {
            splitHorizontal = true;
        }
        else
        {
            std::uniform_int_distribution<int> coinFlip(0, 1);
            splitHorizontal = coinFlip(rng) == 0;
        }

        int maxSplit = (splitHorizontal ? node->height : node->width) - minLeafSize;
        if (maxSplit <= minLeafSize)
        {
            return;
        }

        std::uniform_int_distribution<int> splitDist(minLeafSize, maxSplit);
        int splitPos = splitDist(rng);

        if (splitHorizontal)
        {
            node->left = std::make_unique<BSPNode>();
            node->left->x = node->x;
            node->left->y = node->y;
            node->left->width = node->width;
            node->left->height = splitPos;

            node->right = std::make_unique<BSPNode>();
            node->right->x = node->x;
            node->right->y = node->y + splitPos;
            node->right->width = node->width;
            node->right->height = node->height - splitPos;
        }
        else
        {
            node->left = std::make_unique<BSPNode>();
            node->left->x = node->x;
            node->left->y = node->y;
            node->left->width = splitPos;
            node->left->height = node->height;

            node->right = std::make_unique<BSPNode>();
            node->right->x = node->x + splitPos;
            node->right->y = node->y;
            node->right->width = node->width - splitPos;
            node->right->height = node->height;
        }

        SplitBSP(node->left.get(), minLeafSize, rng);
        SplitBSP(node->right.get(), minLeafSize, rng);
    }

    void DungeonGenerator::CreateRooms(BSPNode* node, const DungeonSettings& settings, std::mt19937& rng)
    {
        if (node->left || node->right)
        {
            if (node->left)
            {
                CreateRooms(node->left.get(), settings, rng);
            }
            if (node->right)
            {
                CreateRooms(node->right.get(), settings, rng);
            }
            return;
        }

        // Leaf node: create a room within it
        int pad = settings.roomPadding;
        int maxW = std::min(settings.maxRoomSize, node->width - 2 * pad);
        int maxH = std::min(settings.maxRoomSize, node->height - 2 * pad);
        int minW = std::min(settings.minRoomSize, maxW);
        int minH = std::min(settings.minRoomSize, maxH);

        if (minW < 3 || minH < 3 || maxW < minW || maxH < minH)
        {
            return;
        }

        std::uniform_int_distribution<int> wDist(minW, maxW);
        std::uniform_int_distribution<int> hDist(minH, maxH);
        int roomW = wDist(rng);
        int roomH = hDist(rng);

        int maxX = node->x + node->width - roomW - pad;
        int maxY = node->y + node->height - roomH - pad;
        int minX = node->x + pad;
        int minY = node->y + pad;

        if (maxX < minX)
        {
            maxX = minX;
        }
        if (maxY < minY)
        {
            maxY = minY;
        }

        std::uniform_int_distribution<int> xDist(minX, maxX);
        std::uniform_int_distribution<int> yDist(minY, maxY);

        node->room.x = xDist(rng);
        node->room.y = yDist(rng);
        node->room.width = roomW;
        node->room.height = roomH;
        node->hasRoom = true;
    }

    void DungeonGenerator::CollectRooms(const BSPNode* node, std::vector<DungeonRoom>& rooms)
    {
        if (!node)
        {
            return;
        }
        if (node->hasRoom)
        {
            DungeonRoom room = node->room;
            room.id = static_cast<int>(rooms.size());
            rooms.push_back(room);
        }
        CollectRooms(node->left.get(), rooms);
        CollectRooms(node->right.get(), rooms);
    }

    DungeonRoom DungeonGenerator::GetAnyRoom(const BSPNode* node)
    {
        if (node->hasRoom)
        {
            return node->room;
        }
        if (node->left)
        {
            return GetAnyRoom(node->left.get());
        }
        if (node->right)
        {
            return GetAnyRoom(node->right.get());
        }
        return {}; // Should not happen with valid tree
    }

    void DungeonGenerator::CarveRoom(DungeonLayout& layout, const DungeonRoom& room)
    {
        for (int ry = room.y; ry < room.y + room.height; ++ry)
        {
            for (int rx = room.x; rx < room.x + room.width; ++rx)
            {
                if (rx >= 0 && rx < layout.width && ry >= 0 && ry < layout.height)
                {
                    layout.grid[ry * layout.width + rx] = 1; // Floor
                }
            }
        }
    }

    void DungeonGenerator::CarveCorridor(DungeonLayout& layout, int x1, int y1, int x2, int y2, int corridorWidth)
    {
        int halfW = corridorWidth / 2;

        // Carve horizontal segment
        int startX = std::min(x1, x2);
        int endX = std::max(x1, x2);
        for (int x = startX; x <= endX; ++x)
        {
            for (int w = -halfW; w <= halfW; ++w)
            {
                int cy = y1 + w;
                if (x >= 0 && x < layout.width && cy >= 0 && cy < layout.height)
                {
                    if (layout.grid[cy * layout.width + x] == 0)
                    {
                        layout.grid[cy * layout.width + x] = 2; // Corridor
                    }
                }
            }
        }

        // Carve vertical segment
        int startY = std::min(y1, y2);
        int endY = std::max(y1, y2);
        for (int y = startY; y <= endY; ++y)
        {
            for (int w = -halfW; w <= halfW; ++w)
            {
                int cx = x2 + w;
                if (cx >= 0 && cx < layout.width && y >= 0 && y < layout.height)
                {
                    if (layout.grid[y * layout.width + cx] == 0)
                    {
                        layout.grid[y * layout.width + cx] = 2; // Corridor
                    }
                }
            }
        }
    }

    void DungeonGenerator::ConnectRooms(DungeonLayout& layout, const DungeonRoom& a, const DungeonRoom& b,
                                        int corridorWidth, std::mt19937& rng)
    {
        int ax = a.CenterX(), ay = a.CenterY();
        int bx = b.CenterX(), by = b.CenterY();

        // L-shaped corridor: random whether horizontal-first or vertical-first
        std::uniform_int_distribution<int> coinFlip(0, 1);

        DungeonCorridor corridor;
        corridor.x1 = ax;
        corridor.y1 = ay;
        corridor.x2 = bx;
        corridor.y2 = by;
        layout.corridors.push_back(corridor);

        if (coinFlip(rng) == 0)
        {
            // Horizontal then vertical
            CarveCorridor(layout, ax, ay, bx, ay, corridorWidth);
            CarveCorridor(layout, bx, ay, bx, by, corridorWidth);
        }
        else
        {
            // Vertical then horizontal
            CarveCorridor(layout, ax, ay, ax, by, corridorWidth);
            CarveCorridor(layout, ax, by, bx, by, corridorWidth);
        }
    }

    void DungeonGenerator::ConnectBSP(BSPNode* node, DungeonLayout& layout, int corridorWidth, std::mt19937& rng)
    {
        if (!node || !node->left || !node->right)
        {
            return;
        }

        ConnectBSP(node->left.get(), layout, corridorWidth, rng);
        ConnectBSP(node->right.get(), layout, corridorWidth, rng);

        // Connect a room from left subtree to a room from right subtree
        DungeonRoom leftRoom = GetAnyRoom(node->left.get());
        DungeonRoom rightRoom = GetAnyRoom(node->right.get());
        ConnectRooms(layout, leftRoom, rightRoom, corridorWidth, rng);
    }

    DungeonLayout DungeonGenerator::GenerateBSP(const DungeonSettings& settings)
    {
        DungeonLayout layout;
        layout.width = settings.width;
        layout.height = settings.height;
        layout.grid.resize(settings.width * settings.height, 0); // All walls

        std::mt19937 rng(settings.seed);

        // Build BSP tree
        auto root = std::make_unique<BSPNode>();
        root->x = 0;
        root->y = 0;
        root->width = settings.width;
        root->height = settings.height;

        SplitBSP(root.get(), settings.minLeafSize, rng);
        CreateRooms(root.get(), settings, rng);
        CollectRooms(root.get(), layout.rooms);

        // Carve rooms into grid
        for (const auto& room : layout.rooms)
        {
            CarveRoom(layout, room);
        }

        // Connect rooms via BSP tree
        ConnectBSP(root.get(), layout, settings.corridorWidth, rng);

        return layout;
    }

    // ============================================================================
    // DungeonGenerator — Cellular Automata
    // ============================================================================

    DungeonLayout DungeonGenerator::GenerateCellularAutomata(int width, int height, float fillProb, int iterations,
                                                             uint32_t seed)
    {
        DungeonLayout layout;
        layout.width = width;
        layout.height = height;
        layout.grid.resize(width * height, 0);

        std::mt19937 rng(seed);
        std::uniform_real_distribution<float> dist(0.0f, 1.0f);

        // Initial random fill
        for (int y = 0; y < height; ++y)
        {
            for (int x = 0; x < width; ++x)
            {
                // Keep borders as walls
                if (x == 0 || x == width - 1 || y == 0 || y == height - 1)
                {
                    layout.grid[y * width + x] = 0;
                }
                else
                {
                    layout.grid[y * width + x] = (dist(rng) < fillProb) ? 0 : 1;
                }
            }
        }

        // Cellular automata smoothing
        std::vector<int> temp(width * height, 0);
        for (int iter = 0; iter < iterations; ++iter)
        {
            for (int y = 1; y < height - 1; ++y)
            {
                for (int x = 1; x < width - 1; ++x)
                {
                    int walls = 0;
                    for (int dy = -1; dy <= 1; ++dy)
                    {
                        for (int dx = -1; dx <= 1; ++dx)
                        {
                            if (layout.grid[(y + dy) * width + (x + dx)] == 0)
                            {
                                walls++;
                            }
                        }
                    }
                    // 4-5 rule: become wall if >= 5 neighbors are walls
                    temp[y * width + x] = (walls >= 5) ? 0 : 1;
                }
            }
            // Copy back, keeping borders
            for (int y = 1; y < height - 1; ++y)
            {
                for (int x = 1; x < width - 1; ++x)
                {
                    layout.grid[y * width + x] = temp[y * width + x];
                }
            }
        }

        // Identify "rooms" as connected floor regions using flood fill
        std::vector<bool> visited(width * height, false);
        int roomId = 0;
        for (int y = 1; y < height - 1; ++y)
        {
            for (int x = 1; x < width - 1; ++x)
            {
                if (layout.grid[y * width + x] == 1 && !visited[y * width + x])
                {
                    // Flood fill to find room bounds
                    DungeonRoom room;
                    room.id = roomId++;
                    int minX = x, maxX = x, minY = y, maxY = y;

                    std::queue<std::pair<int, int>> q;
                    q.push({x, y});
                    visited[y * width + x] = true;

                    while (!q.empty())
                    {
                        auto [cx, cy] = q.front();
                        q.pop();

                        minX = std::min(minX, cx);
                        maxX = std::max(maxX, cx);
                        minY = std::min(minY, cy);
                        maxY = std::max(maxY, cy);

                        int neighbors[] = {-1, 0, 1, 0, 0, -1, 0, 1};
                        for (int i = 0; i < 8; i += 2)
                        {
                            int nx = cx + neighbors[i];
                            int ny = cy + neighbors[i + 1];
                            if (nx >= 0 && nx < width && ny >= 0 && ny < height && !visited[ny * width + nx] &&
                                layout.grid[ny * width + nx] == 1)
                            {
                                visited[ny * width + nx] = true;
                                q.push({nx, ny});
                            }
                        }
                    }

                    room.x = minX;
                    room.y = minY;
                    room.width = maxX - minX + 1;
                    room.height = maxY - minY + 1;
                    layout.rooms.push_back(room);
                }
            }
        }

        return layout;
    }

    // ============================================================================
    // DungeonGenerator — Room Placement with Corridors
    // ============================================================================

    DungeonLayout DungeonGenerator::GenerateRooms(int width, int height, int roomCount, int minRoomSize,
                                                  int maxRoomSize, uint32_t seed)
    {
        DungeonLayout layout;
        layout.width = width;
        layout.height = height;
        layout.grid.resize(width * height, 0);

        std::mt19937 rng(seed);

        // Place rooms with overlap rejection
        int maxAttempts = roomCount * 10;
        for (int attempt = 0; attempt < maxAttempts && static_cast<int>(layout.rooms.size()) < roomCount; ++attempt)
        {
            std::uniform_int_distribution<int> wDist(minRoomSize, maxRoomSize);
            std::uniform_int_distribution<int> hDist(minRoomSize, maxRoomSize);
            int rw = wDist(rng);
            int rh = hDist(rng);

            std::uniform_int_distribution<int> xDist(1, width - rw - 1);
            std::uniform_int_distribution<int> yDist(1, height - rh - 1);
            int rx = xDist(rng);
            int ry = yDist(rng);

            // Check for overlap with existing rooms (with padding)
            bool overlaps = false;
            for (const auto& existing : layout.rooms)
            {
                if (rx - 1 < existing.x + existing.width && rx + rw + 1 > existing.x &&
                    ry - 1 < existing.y + existing.height && ry + rh + 1 > existing.y)
                {
                    overlaps = true;
                    break;
                }
            }

            if (!overlaps)
            {
                DungeonRoom room;
                room.id = static_cast<int>(layout.rooms.size());
                room.x = rx;
                room.y = ry;
                room.width = rw;
                room.height = rh;
                layout.rooms.push_back(room);
                CarveRoom(layout, room);
            }
        }

        // Connect rooms using a minimum spanning tree (Prim's algorithm)
        if (layout.rooms.size() < 2)
        {
            return layout;
        }

        int numRooms = static_cast<int>(layout.rooms.size());
        std::vector<bool> inTree(numRooms, false);
        std::vector<float> minCost(numRooms, 1e30f);
        std::vector<int> closestInTree(numRooms, -1);

        inTree[0] = true;
        for (int i = 1; i < numRooms; ++i)
        {
            float dx = static_cast<float>(layout.rooms[0].CenterX() - layout.rooms[i].CenterX());
            float dy = static_cast<float>(layout.rooms[0].CenterY() - layout.rooms[i].CenterY());
            minCost[i] = dx * dx + dy * dy;
            closestInTree[i] = 0;
        }

        for (int added = 1; added < numRooms; ++added)
        {
            // Find cheapest room not yet in tree
            int best = -1;
            float bestCost = 1e30f;
            for (int i = 0; i < numRooms; ++i)
            {
                if (!inTree[i] && minCost[i] < bestCost)
                {
                    bestCost = minCost[i];
                    best = i;
                }
            }

            if (best < 0)
            {
                break;
            }

            inTree[best] = true;
            ConnectRooms(layout, layout.rooms[closestInTree[best]], layout.rooms[best], 2, rng);

            // Update costs for remaining rooms
            for (int i = 0; i < numRooms; ++i)
            {
                if (!inTree[i])
                {
                    float dx = static_cast<float>(layout.rooms[best].CenterX() - layout.rooms[i].CenterX());
                    float dy = static_cast<float>(layout.rooms[best].CenterY() - layout.rooms[i].CenterY());
                    float cost = dx * dx + dy * dy;
                    if (cost < minCost[i])
                    {
                        minCost[i] = cost;
                        closestInTree[i] = best;
                    }
                }
            }
        }

        return layout;
    }

    // ============================================================================
    // WaveFunctionCollapse
    // ============================================================================

    bool WFCGrid::IsCollapsed() const
    {
        for (const auto& row : cells)
            for (const auto& cell : row)
                if (cell.size() != 1)
                    return false;
        return true;
    }

    bool WFCGrid::HasContradiction() const
    {
        for (const auto& row : cells)
            for (const auto& cell : row)
                if (cell.empty())
                    return true;
        return false;
    }

    void WaveFunctionCollapse::AddTile(const WFCTile& tile)
    {
        m_tiles.push_back(tile);
    }

    void WaveFunctionCollapse::SetGridSize(int width, int height)
    {
        m_grid.width = width;
        m_grid.height = height;
        m_grid.cells.resize(height);
        for (auto& row : m_grid.cells)
        {
            row.resize(width);
            for (auto& cell : row)
            {
                cell.clear();
                for (size_t i = 0; i < m_tiles.size(); ++i)
                    cell.push_back(static_cast<int>(i));
            }
        }
    }

    bool WaveFunctionCollapse::Collapse(uint32_t seed)
    {
        std::mt19937 rng(seed);

        while (!m_grid.IsCollapsed())
        {
            if (m_grid.HasContradiction())
                return false;

            auto [x, y] = FindMinEntropyCell();
            if (x < 0)
                return false;

            auto& cell = m_grid.cells[y][x];
            // Weighted random selection
            std::vector<float> weights;
            for (int id : cell)
                weights.push_back(m_tiles[id].weight);
            std::discrete_distribution<int> dist(weights.begin(), weights.end());
            int chosen = dist(rng);
            int chosenTile = cell[chosen];
            cell = {chosenTile};

            Propagate(x, y);
        }
        return true;
    }

    const WFCTile* WaveFunctionCollapse::GetTile(int id) const
    {
        if (id >= 0 && id < static_cast<int>(m_tiles.size()))
            return &m_tiles[id];
        return nullptr;
    }

    std::pair<int, int> WaveFunctionCollapse::FindMinEntropyCell() const
    {
        int minEntropy = INT32_MAX;
        int bestX = -1, bestY = -1;
        for (int y = 0; y < m_grid.height; ++y)
        {
            for (int x = 0; x < m_grid.width; ++x)
            {
                int entropy = static_cast<int>(m_grid.cells[y][x].size());
                if (entropy > 1 && entropy < minEntropy)
                {
                    minEntropy = entropy;
                    bestX = x;
                    bestY = y;
                }
            }
        }
        return {bestX, bestY};
    }

    void WaveFunctionCollapse::Propagate(int x, int y)
    {
        // Direction offsets: North, East, South, West
        int dx[] = {0, 1, 0, -1};
        int dy[] = {-1, 0, 1, 0};
        int oppositeDir[] = {2, 3, 0, 1};

        std::queue<std::pair<int, int>> toProcess;
        toProcess.push({x, y});

        while (!toProcess.empty())
        {
            auto [cx, cy] = toProcess.front();
            toProcess.pop();

            for (int dir = 0; dir < 4; ++dir)
            {
                int nx = cx + dx[dir];
                int ny = cy + dy[dir];
                if (nx < 0 || nx >= m_grid.width || ny < 0 || ny >= m_grid.height)
                    continue;

                auto& neighborCell = m_grid.cells[ny][nx];
                if (neighborCell.size() <= 1)
                    continue;

                // Collect valid sockets from current cell
                std::vector<std::string> validSockets;
                for (int id : m_grid.cells[cy][cx])
                {
                    validSockets.push_back(m_tiles[id].sockets[dir]);
                }

                // Remove neighbor tiles that don't match
                size_t prevSize = neighborCell.size();
                neighborCell.erase(std::remove_if(neighborCell.begin(), neighborCell.end(),
                                                  [&](int tileId)
                                                  {
                                                      const auto& neighborSocket =
                                                          m_tiles[tileId].sockets[oppositeDir[dir]];
                                                      for (const auto& vs : validSockets)
                                                      {
                                                          if (AreSocketsCompatible(vs, neighborSocket))
                                                              return false;
                                                      }
                                                      return true;
                                                  }),
                                   neighborCell.end());

                if (neighborCell.size() < prevSize)
                {
                    toProcess.push({nx, ny});
                }
            }
        }
    }

    bool WaveFunctionCollapse::AreSocketsCompatible(const std::string& a, const std::string& b) const
    {
        return a == b; // Simple string matching; can be extended for asymmetric rules
    }

    std::string WaveFunctionCollapse::Console_GetStatus() const
    {
        std::ostringstream ss;
        ss << "=== WFC Status ===\n";
        ss << "Tiles: " << m_tiles.size() << "\n";
        ss << "Grid: " << m_grid.width << "x" << m_grid.height << "\n";
        ss << "Collapsed: " << (m_grid.IsCollapsed() ? "Yes" : "No") << "\n";
        ss << "Contradiction: " << (m_grid.HasContradiction() ? "Yes" : "No") << "\n";
        return ss.str();
    }

} // namespace Spark::Procedural
