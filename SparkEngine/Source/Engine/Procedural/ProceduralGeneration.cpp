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
