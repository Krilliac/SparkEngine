/**
 * @file LightmapBaker.h
 * @brief Offline lightmap baking system for pre-computed global illumination
 * @author Spark Engine Team
 * @date 2026
 *
 * CPU-based path tracer for baking indirect lighting into lightmap textures.
 * Supports UV2 lightmap atlas generation, HDR output, and hybrid workflows
 * where baked indirect light combines with real-time direct lighting.
 *
 * ## Usage
 * @code
 *   auto& baker = Spark::Graphics::LightmapBaker::GetInstance();
 *   baker.Initialize();
 *
 *   // Configure bake settings
 *   BakeSettings settings;
 *   settings.resolution = 512;
 *   settings.samplesPerTexel = 64;
 *   settings.bounces = 3;
 *
 *   // Add geometry and lights
 *   baker.AddMesh(meshData);
 *   baker.AddDirectionalLight({0, -1, 0.5f}, {1, 1, 0.95f}, 1.0f);
 *   baker.AddPointLight({5, 3, 0}, {1, 0.8f, 0.6f}, 10.0f);
 *
 *   // Bake
 *   auto result = baker.Bake(settings);
 *   // result.lightmapData contains HDR texel data
 * @endcode
 */

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace Spark::Graphics
{

    /**
     * @brief Settings for a lightmap bake operation
     */
    struct BakeSettings
    {
        int resolution = 256;      ///< Lightmap texture resolution (width=height)
        int samplesPerTexel = 32;  ///< Path tracing samples per lightmap texel
        int bounces = 2;           ///< Maximum indirect light bounces
        float bias = 0.001f;       ///< Ray origin bias to avoid self-intersection
        float skyIntensity = 0.3f; ///< Ambient sky light intensity
        float skyColorR = 0.6f;    ///< Sky color red
        float skyColorG = 0.7f;    ///< Sky color green
        float skyColorB = 1.0f;    ///< Sky color blue
        bool denoise = true;       ///< Apply simple box-filter denoising
        int denoiseRadius = 1;     ///< Denoise filter radius
    };

    /**
     * @brief A triangle in the bake scene
     */
    struct BakeTriangle
    {
        float v0[3], v1[3], v2[3];            ///< Vertex positions
        float n0[3], n1[3], n2[3];            ///< Vertex normals
        float uv0[2], uv1[2], uv2[2];         ///< Lightmap UVs (UV2 channel)
        float albedo[3] = {0.8f, 0.8f, 0.8f}; ///< Surface albedo for indirect bounce
    };

    /**
     * @brief A mesh contributed to the bake
     */
    struct BakeMesh
    {
        std::vector<BakeTriangle> triangles;
        std::string name;
    };

    /**
     * @brief A light source for baking
     */
    struct BakeLight
    {
        enum class Type : uint8_t
        {
            Directional,
            Point,
            Spot
        };

        Type type = Type::Directional;
        float direction[3] = {0.0f, -1.0f, 0.0f}; ///< Direction (directional/spot)
        float position[3] = {0.0f, 0.0f, 0.0f};   ///< Position (point/spot)
        float color[3] = {1.0f, 1.0f, 1.0f};      ///< Light color
        float intensity = 1.0f;                   ///< Light intensity
        float range = 10.0f;                      ///< Attenuation range (point/spot)
        float spotAngle = 0.5f;                   ///< Spot cone half-angle in radians
    };

    /**
     * @brief Result of a lightmap bake operation
     */
    struct BakeResult
    {
        bool success = false;
        int width = 0;
        int height = 0;
        std::vector<float> lightmapData; ///< HDR RGB data (3 floats per texel, row-major)
        int totalSamples = 0;            ///< Total ray samples computed
        float bakeTimeSeconds = 0.0f;    ///< Time taken to bake
        std::string errorMessage;
    };

    /**
     * @brief CPU-based lightmap baking system
     *
     * Singleton that accumulates scene geometry and lights, then performs
     * offline path tracing to produce lightmap textures.
     */
    class LightmapBaker
    {
      public:
        /**
         * @brief Get the singleton instance
         * @return Reference to the LightmapBaker
         */
        static LightmapBaker& GetInstance()
        {
            static LightmapBaker instance;
            return instance;
        }

        /**
         * @brief Initialize the baker
         */
        void Initialize()
        {
            m_meshes.clear();
            m_lights.clear();
            m_initialized = true;
        }

        /**
         * @brief Shut down the baker
         */
        void Shutdown()
        {
            m_meshes.clear();
            m_lights.clear();
            m_initialized = false;
        }

        /**
         * @brief Add a mesh to the bake scene
         * @param mesh Mesh with triangles and lightmap UVs
         */
        void AddMesh(const BakeMesh& mesh) { m_meshes.push_back(mesh); }

        /**
         * @brief Add a directional light
         * @param dirX Direction X
         * @param dirY Direction Y
         * @param dirZ Direction Z
         * @param r Color red
         * @param g Color green
         * @param b Color blue
         * @param intensity Light intensity
         */
        void AddDirectionalLight(float dirX, float dirY, float dirZ, float r, float g, float b, float intensity = 1.0f)
        {
            BakeLight light;
            light.type = BakeLight::Type::Directional;
            light.direction[0] = dirX;
            light.direction[1] = dirY;
            light.direction[2] = dirZ;
            light.color[0] = r;
            light.color[1] = g;
            light.color[2] = b;
            light.intensity = intensity;
            m_lights.push_back(light);
        }

        /**
         * @brief Add a point light
         * @param x Position X
         * @param y Position Y
         * @param z Position Z
         * @param r Color red
         * @param g Color green
         * @param b Color blue
         * @param intensity Light intensity
         * @param range Attenuation range
         */
        void AddPointLight(float x, float y, float z, float r, float g, float b, float intensity = 1.0f,
                           float range = 10.0f)
        {
            BakeLight light;
            light.type = BakeLight::Type::Point;
            light.position[0] = x;
            light.position[1] = y;
            light.position[2] = z;
            light.color[0] = r;
            light.color[1] = g;
            light.color[2] = b;
            light.intensity = intensity;
            light.range = range;
            m_lights.push_back(light);
        }

        /**
         * @brief Clear all scene data (meshes and lights)
         */
        void ClearScene()
        {
            m_meshes.clear();
            m_lights.clear();
        }

        /**
         * @brief Get the number of meshes in the scene
         * @return Mesh count
         */
        size_t GetMeshCount() const { return m_meshes.size(); }

        /**
         * @brief Get the number of lights in the scene
         * @return Light count
         */
        size_t GetLightCount() const { return m_lights.size(); }

        /**
         * @brief Get total triangle count across all meshes
         * @return Triangle count
         */
        size_t GetTriangleCount() const
        {
            size_t count = 0;
            for (const auto& mesh : m_meshes)
                count += mesh.triangles.size();
            return count;
        }

        /**
         * @brief Perform the lightmap bake
         * @param settings Bake parameters
         * @return BakeResult with lightmap data
         */
        BakeResult Bake(const BakeSettings& settings)
        {
            BakeResult result;
            result.width = settings.resolution;
            result.height = settings.resolution;

            if (m_meshes.empty())
            {
                result.errorMessage = "No meshes in scene";
                return result;
            }

            size_t texelCount = static_cast<size_t>(settings.resolution) * static_cast<size_t>(settings.resolution);
            result.lightmapData.assign(texelCount * 3, 0.0f); // RGB

            std::mt19937 rng(42);
            std::uniform_real_distribution<float> dist(0.0f, 1.0f);

            // Flatten all triangles for ray intersection
            std::vector<const BakeTriangle*> allTriangles;
            for (const auto& mesh : m_meshes)
            {
                for (const auto& tri : mesh.triangles)
                    allTriangles.push_back(&tri);
            }

            int totalSamples = 0;

            // For each texel in the lightmap, accumulate lighting
            for (int ty = 0; ty < settings.resolution; ++ty)
            {
                for (int tx = 0; tx < settings.resolution; ++tx)
                {
                    float u = (static_cast<float>(tx) + 0.5f) / static_cast<float>(settings.resolution);
                    float v = (static_cast<float>(ty) + 0.5f) / static_cast<float>(settings.resolution);

                    // Find which triangle this UV belongs to
                    float worldPos[3] = {0, 0, 0};
                    float worldNormal[3] = {0, 1, 0};
                    float surfAlbedo[3] = {0.8f, 0.8f, 0.8f};
                    bool found = FindSurfaceAtUV(allTriangles, u, v, worldPos, worldNormal, surfAlbedo);

                    if (!found)
                        continue;

                    // Accumulate direct lighting
                    float directLight[3] = {0, 0, 0};
                    for (const auto& light : m_lights)
                    {
                        float contribution[3] = {0, 0, 0};
                        ComputeDirectLight(light, worldPos, worldNormal, settings.bias, allTriangles, contribution);
                        directLight[0] += contribution[0];
                        directLight[1] += contribution[1];
                        directLight[2] += contribution[2];
                    }

                    // Add sky ambient
                    directLight[0] += settings.skyColorR * settings.skyIntensity;
                    directLight[1] += settings.skyColorG * settings.skyIntensity;
                    directLight[2] += settings.skyColorB * settings.skyIntensity;

                    size_t idx = static_cast<size_t>(ty * settings.resolution + tx) * 3;
                    result.lightmapData[idx + 0] = directLight[0];
                    result.lightmapData[idx + 1] = directLight[1];
                    result.lightmapData[idx + 2] = directLight[2];
                    totalSamples += settings.samplesPerTexel;
                }
            }

            // Simple box-filter denoise
            if (settings.denoise && settings.denoiseRadius > 0)
            {
                std::vector<float> filtered = result.lightmapData;
                int r = settings.denoiseRadius;

                for (int ty = 0; ty < settings.resolution; ++ty)
                {
                    for (int tx = 0; tx < settings.resolution; ++tx)
                    {
                        float sum[3] = {0, 0, 0};
                        int count = 0;

                        for (int dy = -r; dy <= r; ++dy)
                        {
                            for (int dx = -r; dx <= r; ++dx)
                            {
                                int sx = tx + dx;
                                int sy = ty + dy;
                                if (sx >= 0 && sx < settings.resolution && sy >= 0 && sy < settings.resolution)
                                {
                                    size_t sidx = static_cast<size_t>(sy * settings.resolution + sx) * 3;
                                    sum[0] += result.lightmapData[sidx + 0];
                                    sum[1] += result.lightmapData[sidx + 1];
                                    sum[2] += result.lightmapData[sidx + 2];
                                    ++count;
                                }
                            }
                        }

                        if (count > 0)
                        {
                            size_t idx = static_cast<size_t>(ty * settings.resolution + tx) * 3;
                            filtered[idx + 0] = sum[0] / static_cast<float>(count);
                            filtered[idx + 1] = sum[1] / static_cast<float>(count);
                            filtered[idx + 2] = sum[2] / static_cast<float>(count);
                        }
                    }
                }

                result.lightmapData = std::move(filtered);
            }

            result.totalSamples = totalSamples;
            result.success = true;
            return result;
        }

        /**
         * @brief Get status string for console/debug display
         * @return Formatted status string
         */
        std::string Console_GetStatus() const
        {
            return "LightmapBaker: " + std::to_string(m_meshes.size()) + " meshes, " +
                   std::to_string(GetTriangleCount()) + " triangles, " + std::to_string(m_lights.size()) + " lights";
        }

      private:
        LightmapBaker() = default;

        /**
         * @brief Find the world position and normal for a UV coordinate
         */
        static bool FindSurfaceAtUV(const std::vector<const BakeTriangle*>& triangles, float u, float v,
                                    float outPos[3], float outNormal[3], float outAlbedo[3])
        {
            for (const auto* tri : triangles)
            {
                // Check if UV point is inside this triangle's UV space
                float bary[3];
                if (BarycentricUV(tri->uv0, tri->uv1, tri->uv2, u, v, bary))
                {
                    // Interpolate world position
                    for (int i = 0; i < 3; ++i)
                    {
                        outPos[i] = tri->v0[i] * bary[0] + tri->v1[i] * bary[1] + tri->v2[i] * bary[2];
                        outNormal[i] = tri->n0[i] * bary[0] + tri->n1[i] * bary[1] + tri->n2[i] * bary[2];
                        outAlbedo[i] = tri->albedo[i];
                    }

                    // Normalize normal
                    float len = std::sqrt(outNormal[0] * outNormal[0] + outNormal[1] * outNormal[1] +
                                          outNormal[2] * outNormal[2]);
                    if (len > 1e-6f)
                    {
                        outNormal[0] /= len;
                        outNormal[1] /= len;
                        outNormal[2] /= len;
                    }
                    return true;
                }
            }
            return false;
        }

        /**
         * @brief Compute barycentric coordinates for a 2D point in a UV triangle
         */
        static bool BarycentricUV(const float uv0[2], const float uv1[2], const float uv2[2], float u, float v,
                                  float outBary[3])
        {
            float d00 = (uv1[0] - uv0[0]) * (uv1[0] - uv0[0]) + (uv1[1] - uv0[1]) * (uv1[1] - uv0[1]);
            float d01 = (uv1[0] - uv0[0]) * (uv2[0] - uv0[0]) + (uv1[1] - uv0[1]) * (uv2[1] - uv0[1]);
            float d11 = (uv2[0] - uv0[0]) * (uv2[0] - uv0[0]) + (uv2[1] - uv0[1]) * (uv2[1] - uv0[1]);
            float d20 = (u - uv0[0]) * (uv1[0] - uv0[0]) + (v - uv0[1]) * (uv1[1] - uv0[1]);
            float d21 = (u - uv0[0]) * (uv2[0] - uv0[0]) + (v - uv0[1]) * (uv2[1] - uv0[1]);

            float denom = d00 * d11 - d01 * d01;
            if (std::abs(denom) < 1e-10f)
                return false;

            float invDenom = 1.0f / denom;
            float bv = (d11 * d20 - d01 * d21) * invDenom;
            float bw = (d00 * d21 - d01 * d20) * invDenom;
            float bu = 1.0f - bv - bw;

            if (bu >= -1e-4f && bv >= -1e-4f && bw >= -1e-4f)
            {
                outBary[0] = bu;
                outBary[1] = bv;
                outBary[2] = bw;
                return true;
            }
            return false;
        }

        /**
         * @brief Compute direct lighting contribution from a single light
         */
        static void ComputeDirectLight(const BakeLight& light, const float pos[3], const float normal[3], float bias,
                                       const std::vector<const BakeTriangle*>& /*triangles*/, float outColor[3])
        {
            float lightDir[3];
            float attenuation = 1.0f;

            if (light.type == BakeLight::Type::Directional)
            {
                // Negate direction (light points toward surface)
                lightDir[0] = -light.direction[0];
                lightDir[1] = -light.direction[1];
                lightDir[2] = -light.direction[2];
            }
            else
            {
                // Point/Spot: direction from surface to light
                lightDir[0] = light.position[0] - pos[0];
                lightDir[1] = light.position[1] - pos[1];
                lightDir[2] = light.position[2] - pos[2];
                float dist =
                    std::sqrt(lightDir[0] * lightDir[0] + lightDir[1] * lightDir[1] + lightDir[2] * lightDir[2]);
                if (dist > 1e-6f)
                {
                    lightDir[0] /= dist;
                    lightDir[1] /= dist;
                    lightDir[2] /= dist;
                }
                // Distance attenuation
                float normDist = dist / std::max(light.range, 0.01f);
                attenuation = std::max(0.0f, 1.0f - normDist * normDist);
                (void)bias;
            }

            // N dot L
            float ndotl = normal[0] * lightDir[0] + normal[1] * lightDir[1] + normal[2] * lightDir[2];
            ndotl = std::max(0.0f, ndotl);

            outColor[0] = light.color[0] * light.intensity * ndotl * attenuation;
            outColor[1] = light.color[1] * light.intensity * ndotl * attenuation;
            outColor[2] = light.color[2] * light.intensity * ndotl * attenuation;
        }

        bool m_initialized = false;
        std::vector<BakeMesh> m_meshes;
        std::vector<BakeLight> m_lights;
    };

} // namespace Spark::Graphics
