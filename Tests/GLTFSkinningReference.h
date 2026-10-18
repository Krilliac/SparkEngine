/**
 * @file GLTFSkinningReference.h
 * @brief Shared glTF skinning fixtures and the independent posed-vertex reference for the ENG-220 tests.
 *
 * Every fixture is a GLB assembled in-test: Armature (non-joint) -> Root joint (+1 Y) -> Tip joint
 * (+2 Y), with a Body node binding a three-vertex triangle to the skin. ReferencePose() never touches
 * engine math, DirectXMath or quaternions: it writes each joint's glTF TRS at time t in closed form
 * from the authored keys, builds rotations with Rodrigues' formula, and composes column-vector
 * matrices exactly as the glTF 2.0 spec does (global = parent * T * R * S, skin = global * inverseBind).
 * Used by TestGLTFAnimationImport.cpp (CPU evaluator) and TestENG220GPUSkinningD3D11Real.cpp (GPU).
 */

#pragma once

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <system_error>
#include <vector>

namespace SparkTest::GLTFRig
{
    inline constexpr uint32_t kUnsignedByte = 5121;
    inline constexpr uint32_t kUnsignedShort = 5123;
    inline constexpr uint32_t kFloat = 5126;
    inline constexpr float kHalfSqrt2 = 0.70710678f;

    struct Sampler
    {
        std::vector<float> times;
        std::vector<float> values;
        const char* type = "VEC3";
        std::string interpolation = "LINEAR";
        /// Written as the output accessor's count; defaults to times.size().
        size_t outputCount = 0;
    };

    struct Channel
    {
        size_t sampler = 0;
        int node = 0;
        std::string path;
    };

    struct Animation
    {
        std::string name;
        std::vector<Sampler> samplers;
        std::vector<Channel> channels;
    };

    struct Fixture
    {
        bool skinned = true;
        std::string armatureTranslation = "[0,0,0]";
        std::string skinJoints = "[1,2]";
        /// WEIGHTS_0 for the three vertices (four per vertex); every row sums to 1 by default.
        std::vector<float> weights = {0.25f, 0.75f, 0, 0, 1, 0, 0, 0, 0.5f, 0.5f, 0, 0};
        /// Write the skin's inverseBindMatrices: the inverses of Root's (+1 Y) and Tip's (+3 Y) bind poses.
        bool inverseBinds = false;
        std::vector<Animation> animations;
    };

    /// "Wave": Root translates (0,1,0) -> (0,3,0) over 2 s; Tip rotates 0 -> 90 degrees about Z over 1 s.
    inline Animation WaveAnimation()
    {
        Animation wave;
        wave.name = "Wave";
        wave.samplers.push_back({{0.0f, 2.0f}, {0, 1, 0, 0, 3, 0}, "VEC3"});
        // The second key is deliberately not unit length; the importer must renormalize it.
        wave.samplers.push_back({{0.0f, 1.0f}, {0, 0, 0, 1, 0, 0, 2 * kHalfSqrt2, 2 * kHalfSqrt2}, "VEC4"});
        wave.channels.push_back({0, 1, "translation"});
        wave.channels.push_back({1, 2, "rotation"});
        return wave;
    }

    class GLBBuilder
    {
      public:
        size_t AddFloats(const std::vector<float>& values, size_t count, const char* type)
        {
            const size_t start = m_bin.size();
            for (float value : values)
            {
                AppendU32(m_bin, std::bit_cast<uint32_t>(value));
            }
            return AddAccessor(start, kFloat, count, type);
        }

        size_t AddBytes(const std::vector<uint8_t>& bytes, uint32_t componentType, size_t count, const char* type)
        {
            const size_t start = m_bin.size();
            m_bin.insert(m_bin.end(), bytes.begin(), bytes.end());
            return AddAccessor(start, componentType, count, type);
        }

        std::vector<uint8_t> Finish(const std::string& documentTail) const
        {
            std::string json =
                "{\"asset\":{\"version\":\"2.0\"},\"buffers\":[{\"byteLength\":" + std::to_string(m_bin.size()) +
                "}],\"bufferViews\":[" + Join(m_views) + "],\"accessors\":[" + Join(m_accessors) + "]" + documentTail +
                "}";
            while (json.size() % 4 != 0)
            {
                json.push_back(' ');
            }

            std::vector<uint8_t> glb;
            AppendU32(glb, 0x46546C67);
            AppendU32(glb, 2);
            AppendU32(glb, static_cast<uint32_t>(12 + 8 + json.size() + 8 + m_bin.size()));
            AppendU32(glb, static_cast<uint32_t>(json.size()));
            AppendU32(glb, 0x4E4F534A);
            glb.insert(glb.end(), json.begin(), json.end());
            AppendU32(glb, static_cast<uint32_t>(m_bin.size()));
            AppendU32(glb, 0x004E4942);
            glb.insert(glb.end(), m_bin.begin(), m_bin.end());
            return glb;
        }

        static std::string Join(const std::vector<std::string>& items)
        {
            std::string joined;
            for (size_t i = 0; i < items.size(); ++i)
            {
                joined += (i ? "," : "") + items[i];
            }
            return joined;
        }

      private:
        static void AppendU32(std::vector<uint8_t>& bytes, uint32_t value)
        {
            for (int shift = 0; shift < 32; shift += 8)
            {
                bytes.push_back(static_cast<uint8_t>(value >> shift));
            }
        }

        size_t AddAccessor(size_t start, uint32_t componentType, size_t count, const char* type)
        {
            m_views.push_back("{\"buffer\":0,\"byteOffset\":" + std::to_string(start) +
                              ",\"byteLength\":" + std::to_string(m_bin.size() - start) + "}");
            while (m_bin.size() % 4 != 0)
            {
                m_bin.push_back(0);
            }
            m_accessors.push_back("{\"bufferView\":" + std::to_string(m_views.size() - 1) +
                                  ",\"componentType\":" + std::to_string(componentType) +
                                  ",\"count\":" + std::to_string(count) + ",\"type\":\"" + type + "\"}");
            return m_accessors.size() - 1;
        }

        std::vector<uint8_t> m_bin;
        std::vector<std::string> m_views;
        std::vector<std::string> m_accessors;
    };

    inline std::vector<uint8_t> BuildGLB(const Fixture& fixture)
    {
        GLBBuilder builder;
        const size_t positions = builder.AddFloats({0, 0, 0, 1, 0, 0, 0, 1, 0}, 3, "VEC3");
        const size_t normals = builder.AddFloats({0, 0, 1, 0, 0, 1, 0, 0, 1}, 3, "VEC3");
        // Skin joint 0 is Root (bone 0), joint 1 is Tip (bone 1).
        const size_t joints = builder.AddBytes({0, 1, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0}, kUnsignedByte, 3, "VEC4");
        const size_t weights = builder.AddFloats(fixture.weights, 3, "VEC4");
        const size_t indices = builder.AddBytes({0, 0, 1, 0, 2, 0}, kUnsignedShort, 3, "SCALAR");
        std::string inverseBinds;
        if (fixture.inverseBinds)
        {
            // Column-major translations by -1 and -3 along Y.
            const size_t accessor = builder.AddFloats(
                {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, -1, 0, 1, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, -3, 0, 1}, 2,
                "MAT4");
            inverseBinds = ",\"inverseBindMatrices\":" + std::to_string(accessor);
        }

        std::vector<std::string> animations;
        for (const Animation& animation : fixture.animations)
        {
            std::vector<std::string> samplers;
            for (const Sampler& sampler : animation.samplers)
            {
                const size_t components = std::string(sampler.type) == "VEC4" ? 4 : 3;
                const size_t outputCount = sampler.outputCount ? sampler.outputCount : sampler.times.size();
                const size_t input = builder.AddFloats(sampler.times, sampler.times.size(), "SCALAR");
                std::vector<float> values = sampler.values;
                values.resize(outputCount * components, 0.0f);
                const size_t output = builder.AddFloats(values, outputCount, sampler.type);
                samplers.push_back("{\"input\":" + std::to_string(input) + ",\"output\":" + std::to_string(output) +
                                   ",\"interpolation\":\"" + sampler.interpolation + "\"}");
            }
            std::vector<std::string> channels;
            for (const Channel& channel : animation.channels)
            {
                channels.push_back("{\"sampler\":" + std::to_string(channel.sampler) + ",\"target\":{\"node\":" +
                                   std::to_string(channel.node) + ",\"path\":\"" + channel.path + "\"}}");
            }
            animations.push_back("{\"name\":\"" + animation.name + "\",\"samplers\":[" + GLBBuilder::Join(samplers) +
                                 "],\"channels\":[" + GLBBuilder::Join(channels) + "]}");
        }

        const std::string skinAttributes =
            fixture.skinned ? ",\"JOINTS_0\":" + std::to_string(joints) + ",\"WEIGHTS_0\":" + std::to_string(weights)
                            : std::string();
        std::string tail = ",\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":" + std::to_string(positions) +
                           ",\"NORMAL\":" + std::to_string(normals) + skinAttributes +
                           "},\"indices\":" + std::to_string(indices) + "}]}]";
        tail += ",\"nodes\":[{\"name\":\"Armature\",\"translation\":" + fixture.armatureTranslation +
                ",\"children\":[1,3]},{\"name\":\"Root\",\"translation\":[0,1,0],\"children\":[2]},"
                "{\"name\":\"Tip\",\"translation\":[0,2,0]},{\"name\":\"Body\",\"mesh\":0" +
                std::string(fixture.skinned ? ",\"skin\":0" : "") + "}]";
        if (fixture.skinned)
        {
            tail += ",\"skins\":[{\"name\":\"Rig\",\"joints\":" + fixture.skinJoints + inverseBinds + "}]";
        }
        if (!animations.empty())
        {
            tail += ",\"animations\":[" + GLBBuilder::Join(animations) + "]";
        }
        return builder.Finish(tail);
    }

    /// Engine asset paths are UTF-8 std::strings; these convert without the Windows ANSI code page.
    inline std::filesystem::path PathFromUtf8(const std::string& utf8)
    {
        return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
    }

    inline std::string PathToUtf8(const std::filesystem::path& path)
    {
        const std::u8string utf8 = path.u8string();
        return std::string(utf8.begin(), utf8.end());
    }

    struct TemporaryGLB
    {
        /// `name` is UTF-8.
        TemporaryGLB(const std::string& name, const Fixture& fixture)
            : directory(std::filesystem::temp_directory_path() / PathFromUtf8("spark_gltf_anim_" + name)),
              path(directory / "rig.glb")
        {
            std::error_code ec;
            std::filesystem::remove_all(directory, ec);
            std::filesystem::create_directories(directory, ec);
            const std::vector<uint8_t> glb = BuildGLB(fixture);
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            file.write(reinterpret_cast<const char*>(glb.data()), static_cast<std::streamsize>(glb.size()));
        }

        ~TemporaryGLB()
        {
            std::error_code ec;
            std::filesystem::remove_all(directory, ec);
        }

        std::filesystem::path directory;
        std::filesystem::path path;
    };

    inline Fixture WaveFixture()
    {
        Fixture fixture;
        fixture.animations.push_back(WaveAnimation());
        return fixture;
    }

    inline constexpr float kPi = 3.14159265358979f;
    inline constexpr float kPoseTolerance = 1.0e-4f;

    /// "Pose" over 2 s. Root: translation (0,1,0) -> (2,1,0) and 0 -> 90 degrees about Z, both LINEAR.
    /// Tip: 0 -> 120 degrees about X (LINEAR) and STEP scale 1 -> 2 (at 1 s) -> 0.5 (at 2 s).
    inline Animation PoseAnimation()
    {
        Animation pose;
        pose.name = "Pose";
        pose.samplers.push_back({{0.0f, 2.0f}, {0, 1, 0, 2, 1, 0}, "VEC3"});
        pose.samplers.push_back({{0.0f, 2.0f}, {0, 0, 0, 1, 0, 0, kHalfSqrt2, kHalfSqrt2}, "VEC4"});
        pose.samplers.push_back({{0.0f, 2.0f}, {0, 0, 0, 1, 0.8660254f, 0, 0, 0.5f}, "VEC4"});
        pose.samplers.push_back({{0.0f, 1.0f, 2.0f}, {1, 1, 1, 2, 2, 2, 0.5f, 0.5f, 0.5f}, "VEC3", "STEP"});
        pose.channels.push_back({0, 1, "translation"});
        pose.channels.push_back({1, 1, "rotation"});
        pose.channels.push_back({2, 2, "rotation"});
        pose.channels.push_back({3, 2, "scale"});
        return pose;
    }

    inline Fixture PoseFixture()
    {
        Fixture fixture;
        fixture.inverseBinds = true;
        fixture.animations.push_back(PoseAnimation());
        return fixture;
    }

    /// Row-major 4x4 matrix applied to column vectors (glTF convention).
    struct Mat4
    {
        float r[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
    };

    inline Mat4 Multiply(const Mat4& a, const Mat4& b)
    {
        Mat4 product;
        for (int row = 0; row < 4; ++row)
        {
            for (int column = 0; column < 4; ++column)
            {
                float sum = 0.0f;
                for (int k = 0; k < 4; ++k)
                {
                    sum += a.r[row][k] * b.r[k][column];
                }
                product.r[row][column] = sum;
            }
        }
        return product;
    }

    inline Mat4 Translation(float x, float y, float z)
    {
        Mat4 m;
        m.r[0][3] = x;
        m.r[1][3] = y;
        m.r[2][3] = z;
        return m;
    }

    inline Mat4 UniformScale(float s)
    {
        Mat4 m;
        m.r[0][0] = s;
        m.r[1][1] = s;
        m.r[2][2] = s;
        return m;
    }

    /// Right-handed rotation by `degrees` about the unit axis (x, y, z), via Rodrigues' formula.
    inline Mat4 Rotation(float x, float y, float z, float degrees)
    {
        const float angle = degrees * kPi / 180.0f;
        const float c = std::cos(angle);
        const float s = std::sin(angle);
        const float t = 1.0f - c;
        Mat4 m;
        m.r[0][0] = c + x * x * t;
        m.r[0][1] = x * y * t - z * s;
        m.r[0][2] = x * z * t + y * s;
        m.r[1][0] = y * x * t + z * s;
        m.r[1][1] = c + y * y * t;
        m.r[1][2] = y * z * t - x * s;
        m.r[2][0] = z * x * t - y * s;
        m.r[2][1] = z * y * t + x * s;
        m.r[2][2] = c + z * z * t;
        return m;
    }

    struct Vec3
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    /// Reference posed vertices of the fixture triangle at clip time t (0 <= t <= 2), from the keys in closed form.
    inline std::vector<Vec3> ReferencePose(float t)
    {
        // Armature (non-joint) has the identity transform, so Root's local transform is its global one.
        const Mat4 root = Multiply(Translation(t, 1.0f, 0.0f), Rotation(0, 0, 1, 45.0f * t));
        const float tipScale = t < 1.0f ? 1.0f : (t < 2.0f ? 2.0f : 0.5f);
        const Mat4 tip = Multiply(
            root, Multiply(Translation(0, 2, 0), Multiply(Rotation(1, 0, 0, 60.0f * t), UniformScale(tipScale))));
        const Mat4 skin[2] = {Multiply(root, Translation(0, -1, 0)), Multiply(tip, Translation(0, -3, 0))};

        // Authored positions and (Root, Tip) weights of the three vertices.
        const float positions[3][3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
        const float weights[3][2] = {{0.25f, 0.75f}, {0.0f, 1.0f}, {0.5f, 0.5f}};
        std::vector<Vec3> posed(3);
        for (int v = 0; v < 3; ++v)
        {
            float out[3] = {};
            for (int joint = 0; joint < 2; ++joint)
            {
                for (int row = 0; row < 3; ++row)
                {
                    const Mat4& m = skin[joint];
                    out[row] += weights[v][joint] * (m.r[row][0] * positions[v][0] + m.r[row][1] * positions[v][1] +
                                                     m.r[row][2] * positions[v][2] + m.r[row][3]);
                }
            }
            posed[v] = {out[0], out[1], out[2]};
        }
        return posed;
    }

    /// Largest per-component distance between two posed triangles. Infinity when the sizes differ or
    /// any coordinate is not finite: NaN compares false with everything, so a running maximum would
    /// otherwise skip it and a NaN pose would pass every `< tolerance` check.
    inline float MaxDeviation(const std::vector<Vec3>& a, const std::vector<Vec3>& b)
    {
        if (a.size() != b.size())
        {
            return std::numeric_limits<float>::infinity();
        }
        float deviation = 0.0f;
        for (size_t i = 0; i < a.size(); ++i)
        {
            for (const float difference :
                 {std::abs(a[i].x - b[i].x), std::abs(a[i].y - b[i].y), std::abs(a[i].z - b[i].z)})
            {
                if (!std::isfinite(difference))
                {
                    return std::numeric_limits<float>::infinity();
                }
                deviation = std::max(deviation, difference);
            }
        }
        return deviation;
    }
} // namespace SparkTest::GLTFRig
