/**
 * @file NetQuantize.cpp
 * @brief Implementation of quantized transform read/write through NetBuffer.
 */

#include "NetQuantize.h"

#include "NetworkManager.h"

namespace Spark::Net::Quantize
{
    void WriteQuat48(NetBuffer& buf, const XMFLOAT4& q)
    {
        const Spark::Utils::Quantization::PackedQuat48 packed = Spark::Utils::Quantization::PackQuat48(q);
        buf.WriteBytes(packed.data.data(), packed.data.size());
    }

    XMFLOAT4 ReadQuat48(NetBuffer& buf)
    {
        Spark::Utils::Quantization::PackedQuat48 packed;
        buf.ReadBytes(packed.data.data(), packed.data.size());
        if (buf.HasError())
        {
            return {0.0f, 0.0f, 0.0f, 1.0f};
        }
        return Spark::Utils::Quantization::UnpackQuat48(packed);
    }

    void WriteVec16(NetBuffer& buf, const XMFLOAT3& v, const XMFLOAT3& rangeMin, const XMFLOAT3& rangeMax)
    {
        const Spark::Utils::Quantization::PackedVec16 packed =
            Spark::Utils::Quantization::PackVec16(v, rangeMin, rangeMax);
        buf.WriteUint16(packed.x);
        buf.WriteUint16(packed.y);
        buf.WriteUint16(packed.z);
    }

    XMFLOAT3 ReadVec16(NetBuffer& buf, const XMFLOAT3& rangeMin, const XMFLOAT3& rangeMax)
    {
        Spark::Utils::Quantization::PackedVec16 packed;
        packed.x = buf.ReadUint16();
        packed.y = buf.ReadUint16();
        packed.z = buf.ReadUint16();
        if (buf.HasError())
        {
            return rangeMin;
        }
        return Spark::Utils::Quantization::UnpackVec16(packed, rangeMin, rangeMax);
    }

} // namespace Spark::Net::Quantize
