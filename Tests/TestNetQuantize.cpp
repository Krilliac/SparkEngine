// TestNetQuantize.cpp — Round-trip and NetBuffer integration tests for
// Spark::Net::Quantize (Advanced-techniques catalog item #26: network
// transform quantization). Exercises the real Utils::Quantization math and
// the real Spark::Net::NetBuffer wire layer.

#include "TestFramework.h"

#include "../SparkEngine/Source/Engine/Networking/NetQuantize.h"
#include "../SparkEngine/Source/Engine/Networking/NetworkManager.h"
#include "../SparkEngine/Source/Utils/Quantization.h"

#include <cmath>

using Spark::Utils::Quantization::PackedQuat48;
using Spark::Utils::Quantization::PackedVec16;
using Spark::Utils::Quantization::PackQuat48;
using Spark::Utils::Quantization::PackVec16;
using Spark::Utils::Quantization::UnpackQuat48;
using Spark::Utils::Quantization::UnpackVec16;

namespace
{

    constexpr float kQuatTolerance = 1.0e-3f;   // ~32767 quantization steps in [-1/sqrt(2), 1/sqrt(2)]
    constexpr float kVec10kTolerance = 0.2f;    // ±10 km AABB → ~0.3 m step; 0.2 m is tight
    constexpr float kVec100mTolerance = 0.005f; // ±100 m AABB → ~3 mm step

    bool ApproxQuat(const XMFLOAT4& a, const XMFLOAT4& b, float tol)
    {
        // q and -q are the same rotation — compare with sign ambiguity
        const float d1 = std::fabs(a.x - b.x) + std::fabs(a.y - b.y) + std::fabs(a.z - b.z) + std::fabs(a.w - b.w);
        const float d2 = std::fabs(a.x + b.x) + std::fabs(a.y + b.y) + std::fabs(a.z + b.z) + std::fabs(a.w + b.w);
        return std::min(d1, d2) < tol * 4.0f;
    }

    bool ApproxFloat(float a, float b, float tol)
    {
        return std::fabs(a - b) <= tol;
    }

    bool ApproxVec3(const XMFLOAT3& a, const XMFLOAT3& b, float tol)
    {
        return ApproxFloat(a.x, b.x, tol) && ApproxFloat(a.y, b.y, tol) && ApproxFloat(a.z, b.z, tol);
    }

} // namespace

// ============================================================================
// Utils::Quantization — pure-math round trip
// ============================================================================

TEST(NetQuantize_PackUnpack_IdentityQuat)
{
    const XMFLOAT4 identity{0.0f, 0.0f, 0.0f, 1.0f};
    const PackedQuat48 packed = PackQuat48(identity);
    const XMFLOAT4 decoded = UnpackQuat48(packed);
    EXPECT_TRUE(ApproxQuat(identity, decoded, kQuatTolerance));
}

TEST(NetQuantize_PackUnpack_AxisRotationsSurviveRoundTrip)
{
    // 90-degree rotation around each axis
    const XMFLOAT4 inputs[] = {
        {0.70710678f, 0.0f, 0.0f, 0.70710678f}, // X
        {0.0f, 0.70710678f, 0.0f, 0.70710678f}, // Y
        {0.0f, 0.0f, 0.70710678f, 0.70710678f}, // Z
        {-0.70710678f, 0.0f, 0.0f, 0.70710678f},
    };
    for (const auto& q : inputs)
    {
        const XMFLOAT4 decoded = UnpackQuat48(PackQuat48(q));
        EXPECT_TRUE(ApproxQuat(q, decoded, kQuatTolerance));
    }
}

TEST(NetQuantize_PackUnpack_ArbitraryQuatRoundTrip)
{
    // Normalized arbitrary rotation
    XMFLOAT4 q{0.3f, 0.5f, 0.4f, 0.7071f};
    const float mag = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    q.x /= mag;
    q.y /= mag;
    q.z /= mag;
    q.w /= mag;

    const XMFLOAT4 decoded = UnpackQuat48(PackQuat48(q));
    EXPECT_TRUE(ApproxQuat(q, decoded, kQuatTolerance));
}

TEST(NetQuantize_PackUnpack_Vec16RoundTripLargeRange)
{
    const XMFLOAT3 rangeMin{-10000.0f, -10000.0f, -10000.0f};
    const XMFLOAT3 rangeMax{10000.0f, 10000.0f, 10000.0f};

    const XMFLOAT3 inputs[] = {
        {0.0f, 0.0f, 0.0f},
        {123.45f, -678.9f, 4321.0f},
        {-9999.9f, 9999.9f, 0.0f},
    };
    for (const auto& v : inputs)
    {
        const XMFLOAT3 decoded = UnpackVec16(PackVec16(v, rangeMin, rangeMax), rangeMin, rangeMax);
        EXPECT_TRUE(ApproxVec3(v, decoded, kVec10kTolerance));
    }
}

TEST(NetQuantize_PackUnpack_Vec16TightRangeHigherPrecision)
{
    const XMFLOAT3 rangeMin{-100.0f, -100.0f, -100.0f};
    const XMFLOAT3 rangeMax{100.0f, 100.0f, 100.0f};

    const XMFLOAT3 v{42.125f, -17.333f, 99.5f};
    const XMFLOAT3 decoded = UnpackVec16(PackVec16(v, rangeMin, rangeMax), rangeMin, rangeMax);
    EXPECT_TRUE(ApproxVec3(v, decoded, kVec100mTolerance));
}

TEST(NetQuantize_PackUnpack_Vec16ClampsOutOfRange)
{
    const XMFLOAT3 rangeMin{-10.0f, -10.0f, -10.0f};
    const XMFLOAT3 rangeMax{10.0f, 10.0f, 10.0f};

    // Well beyond range on each axis
    const XMFLOAT3 v{100.0f, -100.0f, 0.0f};
    const XMFLOAT3 decoded = UnpackVec16(PackVec16(v, rangeMin, rangeMax), rangeMin, rangeMax);
    EXPECT_TRUE(ApproxFloat(decoded.x, rangeMax.x, 0.01f));
    EXPECT_TRUE(ApproxFloat(decoded.y, rangeMin.y, 0.01f));
    EXPECT_TRUE(ApproxFloat(decoded.z, 0.0f, 0.01f));
}

TEST(NetQuantize_PackUnpack_DegenerateRangeReturnsMin)
{
    const XMFLOAT3 rangeMin{5.0f, 5.0f, 5.0f};
    const XMFLOAT3 rangeMax{5.0f, 5.0f, 5.0f};
    const XMFLOAT3 v{1.0f, 2.0f, 3.0f};
    const XMFLOAT3 decoded = UnpackVec16(PackVec16(v, rangeMin, rangeMax), rangeMin, rangeMax);
    EXPECT_TRUE(ApproxVec3(decoded, rangeMin, 0.0001f));
}

// ============================================================================
// NetBuffer integration — Spark::Net::Quantize wire layer
// ============================================================================

TEST(NetQuantize_NetBuffer_WriteQuat48EmitsExactlySixBytes)
{
    Spark::Net::NetBuffer buf;
    const XMFLOAT4 q{0.0f, 0.0f, 0.0f, 1.0f};
    Spark::Net::Quantize::WriteQuat48(buf, q);
    EXPECT_EQ(buf.GetSize(), Spark::Net::Quantize::kQuat48Size);
}

TEST(NetQuantize_NetBuffer_WriteVec16EmitsExactlySixBytes)
{
    Spark::Net::NetBuffer buf;
    const XMFLOAT3 v{0.0f, 0.0f, 0.0f};
    const XMFLOAT3 rangeMin{-10.0f, -10.0f, -10.0f};
    const XMFLOAT3 rangeMax{10.0f, 10.0f, 10.0f};
    Spark::Net::Quantize::WriteVec16(buf, v, rangeMin, rangeMax);
    EXPECT_EQ(buf.GetSize(), Spark::Net::Quantize::kVec16Size);
}

TEST(NetQuantize_NetBuffer_TransformRoundTrip)
{
    Spark::Net::NetBuffer buf;
    const XMFLOAT3 pos{12.5f, -4.25f, 88.0f};
    XMFLOAT4 rot{0.2f, 0.4f, 0.1f, 0.8888f};
    const float mag = std::sqrt(rot.x * rot.x + rot.y * rot.y + rot.z * rot.z + rot.w * rot.w);
    rot.x /= mag;
    rot.y /= mag;
    rot.z /= mag;
    rot.w /= mag;

    const XMFLOAT3 rangeMin{-100.0f, -100.0f, -100.0f};
    const XMFLOAT3 rangeMax{100.0f, 100.0f, 100.0f};

    Spark::Net::Quantize::WriteVec16(buf, pos, rangeMin, rangeMax);
    Spark::Net::Quantize::WriteQuat48(buf, rot);

    // 12 bytes for a full pose — vs. 28 bytes naive (3×float pos + 4×float rot).
    EXPECT_EQ(buf.GetSize(), Spark::Net::Quantize::kVec16Size + Spark::Net::Quantize::kQuat48Size);

    const XMFLOAT3 posOut = Spark::Net::Quantize::ReadVec16(buf, rangeMin, rangeMax);
    const XMFLOAT4 rotOut = Spark::Net::Quantize::ReadQuat48(buf);
    EXPECT_FALSE(buf.HasError());
    EXPECT_TRUE(ApproxVec3(pos, posOut, kVec100mTolerance));
    EXPECT_TRUE(ApproxQuat(rot, rotOut, kQuatTolerance));
}

TEST(NetQuantize_NetBuffer_ShortReadSetsErrorFlag)
{
    Spark::Net::NetBuffer buf;
    buf.WriteUint8(0x00);
    buf.WriteUint8(0x00); // only 2 bytes — need 6

    const XMFLOAT4 rot = Spark::Net::Quantize::ReadQuat48(buf);
    EXPECT_TRUE(buf.HasError());
    // Identity on short-read
    EXPECT_TRUE(ApproxFloat(rot.w, 1.0f, 0.001f));
}

TEST(NetQuantize_NetBuffer_BandwidthReductionVsNaive)
{
    // Sanity check that the quantized form is strictly smaller than
    // the naive XMFLOAT3 + XMFLOAT4 serialization.
    constexpr size_t naive = sizeof(float) * 3 + sizeof(float) * 4;                                 // 28 bytes
    constexpr size_t packed = Spark::Net::Quantize::kVec16Size + Spark::Net::Quantize::kQuat48Size; // 12 bytes
    static_assert(packed < naive, "Packed transform must be smaller than naive");
    EXPECT_EQ(packed, static_cast<size_t>(12));
    EXPECT_EQ(naive, static_cast<size_t>(28));
}
