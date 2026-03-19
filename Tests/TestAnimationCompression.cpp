// TestAnimationCompression.cpp - Tests for quaternion/vector quantization roundtrip accuracy
// Standalone implementations for CI testing

#include "TestFramework.h"
#include <cmath>
#include <cstdint>

namespace TestAnimCompress
{

    struct Quat
    {
        float x = 0.0f, y = 0.0f, z = 0.0f, w = 1.0f;

        float Length() const { return std::sqrt(x * x + y * y + z * z + w * w); }

        Quat Normalized() const
        {
            float len = Length();
            if (len < 0.0001f)
                return {0, 0, 0, 1};
            return {x / len, y / len, z / len, w / len};
        }
    };

    struct Vec3
    {
        float x = 0.0f, y = 0.0f, z = 0.0f;
    };

    // 48-bit quaternion: drop smallest component, store 3 x 16-bit
    struct CompressedQuat
    {
        uint16_t a = 0, b = 0, c = 0;
        uint8_t droppedIndex = 0;
    };

    struct CompressedVec3
    {
        uint16_t x = 0, y = 0, z = 0;
    };

    // Quantize float [-1,1] to uint16
    inline uint16_t FloatToU16(float v)
    {
        float clamped = std::max(-1.0f, std::min(1.0f, v));
        return static_cast<uint16_t>((clamped + 1.0f) * 0.5f * 65535.0f);
    }

    // Dequantize uint16 to float [-1,1]
    inline float U16ToFloat(uint16_t v)
    {
        return (static_cast<float>(v) / 65535.0f) * 2.0f - 1.0f;
    }

    // Quantize float in range [min, max] to uint16
    inline uint16_t FloatToU16Range(float v, float minVal, float maxVal)
    {
        float t = (v - minVal) / (maxVal - minVal);
        t = std::max(0.0f, std::min(1.0f, t));
        return static_cast<uint16_t>(t * 65535.0f);
    }

    inline float U16ToFloatRange(uint16_t v, float minVal, float maxVal)
    {
        float t = static_cast<float>(v) / 65535.0f;
        return minVal + t * (maxVal - minVal);
    }

    CompressedQuat QuantizeQuaternion(Quat q)
    {
        q = q.Normalized();
        // Ensure w >= 0 for consistency
        if (q.w < 0)
        {
            q.x = -q.x;
            q.y = -q.y;
            q.z = -q.z;
            q.w = -q.w;
        }

        float components[4] = {q.x, q.y, q.z, q.w};
        // Find largest absolute component to drop
        uint8_t dropIdx = 0;
        float maxAbs = 0.0f;
        for (uint8_t i = 0; i < 4; i++)
        {
            float a = std::abs(components[i]);
            if (a > maxAbs)
            {
                maxAbs = a;
                dropIdx = i;
            }
        }

        // Encode the other 3 components
        int stored = 0;
        uint16_t vals[3] = {};
        for (uint8_t i = 0; i < 4; i++)
        {
            if (i == dropIdx)
                continue;
            vals[stored++] = FloatToU16(components[i]);
        }

        return {vals[0], vals[1], vals[2], dropIdx};
    }

    Quat DequantizeQuaternion(const CompressedQuat& cq)
    {
        float decoded[3] = {U16ToFloat(cq.a), U16ToFloat(cq.b), U16ToFloat(cq.c)};

        // Reconstruct dropped component
        float sumSq = decoded[0] * decoded[0] + decoded[1] * decoded[1] + decoded[2] * decoded[2];
        float dropped = std::sqrt(std::max(0.0f, 1.0f - sumSq));

        float components[4] = {};
        int stored = 0;
        for (int i = 0; i < 4; i++)
        {
            if (i == cq.droppedIndex)
                components[i] = dropped;
            else
                components[i] = decoded[stored++];
        }

        return {components[0], components[1], components[2], components[3]};
    }

    CompressedVec3 QuantizeVector(Vec3 v, float minVal, float maxVal)
    {
        return {FloatToU16Range(v.x, minVal, maxVal), FloatToU16Range(v.y, minVal, maxVal),
                FloatToU16Range(v.z, minVal, maxVal)};
    }

    Vec3 DequantizeVector(const CompressedVec3& cv, float minVal, float maxVal)
    {
        return {U16ToFloatRange(cv.x, minVal, maxVal), U16ToFloatRange(cv.y, minVal, maxVal),
                U16ToFloatRange(cv.z, minVal, maxVal)};
    }

} // namespace TestAnimCompress

// ============================================================================
// Quaternion Roundtrip
// ============================================================================

TEST(AnimCompression_QuantizeQuaternion_Identity)
{
    TestAnimCompress::Quat identity{0, 0, 0, 1};
    auto compressed = TestAnimCompress::QuantizeQuaternion(identity);
    auto decompressed = TestAnimCompress::DequantizeQuaternion(compressed);

    EXPECT_NEAR(decompressed.x, 0.0f, 0.001f);
    EXPECT_NEAR(decompressed.y, 0.0f, 0.001f);
    EXPECT_NEAR(decompressed.z, 0.0f, 0.001f);
    EXPECT_NEAR(decompressed.w, 1.0f, 0.001f);
}

TEST(AnimCompression_QuantizeQuaternion_Rotation)
{
    // 90-degree rotation around Y axis
    float angle = 3.14159265f / 2.0f;
    TestAnimCompress::Quat rot{0, std::sin(angle / 2), 0, std::cos(angle / 2)};
    auto compressed = TestAnimCompress::QuantizeQuaternion(rot);
    auto decompressed = TestAnimCompress::DequantizeQuaternion(compressed);

    EXPECT_NEAR(decompressed.x, rot.x, 0.01f);
    EXPECT_NEAR(decompressed.y, rot.y, 0.01f);
    EXPECT_NEAR(decompressed.z, rot.z, 0.01f);
    EXPECT_NEAR(decompressed.w, rot.w, 0.01f);
}

TEST(AnimCompression_QuantizeQuaternion_Arbitrary)
{
    // Arbitrary normalized quaternion
    TestAnimCompress::Quat q{0.5f, 0.5f, 0.5f, 0.5f};
    auto compressed = TestAnimCompress::QuantizeQuaternion(q);
    auto decompressed = TestAnimCompress::DequantizeQuaternion(compressed);

    // Should be unit length
    float len = decompressed.Length();
    EXPECT_NEAR(len, 1.0f, 0.02f);

    EXPECT_NEAR(decompressed.x, 0.5f, 0.01f);
    EXPECT_NEAR(decompressed.y, 0.5f, 0.01f);
    EXPECT_NEAR(decompressed.z, 0.5f, 0.01f);
    EXPECT_NEAR(decompressed.w, 0.5f, 0.01f);
}

// ============================================================================
// Vector Roundtrip
// ============================================================================

TEST(AnimCompression_QuantizeVector_Roundtrip)
{
    TestAnimCompress::Vec3 v{1.5f, -3.2f, 7.8f};
    auto compressed = TestAnimCompress::QuantizeVector(v, -10.0f, 10.0f);
    auto decompressed = TestAnimCompress::DequantizeVector(compressed, -10.0f, 10.0f);

    EXPECT_NEAR(decompressed.x, 1.5f, 0.01f);
    EXPECT_NEAR(decompressed.y, -3.2f, 0.01f);
    EXPECT_NEAR(decompressed.z, 7.8f, 0.01f);
}

TEST(AnimCompression_QuantizeVector_ZeroVector)
{
    TestAnimCompress::Vec3 v{0, 0, 0};
    auto compressed = TestAnimCompress::QuantizeVector(v, -100.0f, 100.0f);
    auto decompressed = TestAnimCompress::DequantizeVector(compressed, -100.0f, 100.0f);

    EXPECT_NEAR(decompressed.x, 0.0f, 0.01f);
    EXPECT_NEAR(decompressed.y, 0.0f, 0.01f);
    EXPECT_NEAR(decompressed.z, 0.0f, 0.01f);
}

TEST(AnimCompression_QuantizeVector_Extremes)
{
    TestAnimCompress::Vec3 v{-5.0f, 5.0f, 0.0f};
    auto compressed = TestAnimCompress::QuantizeVector(v, -5.0f, 5.0f);
    auto decompressed = TestAnimCompress::DequantizeVector(compressed, -5.0f, 5.0f);

    EXPECT_NEAR(decompressed.x, -5.0f, 0.01f);
    EXPECT_NEAR(decompressed.y, 5.0f, 0.01f);
    EXPECT_NEAR(decompressed.z, 0.0f, 0.01f);
}
