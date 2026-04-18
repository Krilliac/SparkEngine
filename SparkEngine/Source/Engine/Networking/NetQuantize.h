/**
 * @file NetQuantize.h
 * @brief NetBuffer-aware helpers for quantized transform replication.
 *
 * Wraps `Utils::Quantization` primitives with `NetBuffer` write/read calls so
 * replication code can serialize a rotation in 6 bytes instead of 16, and a
 * bounded-AABB position in 6 bytes instead of 12. Roughly halves the
 * per-entity transform bandwidth compared with naive XMFLOAT4/XMFLOAT3
 * emission, which is the Technique #26 target from the advanced-techniques
 * catalog.
 *
 * Callers are responsible for choosing a sensible AABB for PackVec16 —
 * larger ranges buy reach at the cost of per-axis precision (precision is
 * `(max - min) / 65535`). For a typical 10 km world cube that's ~15 cm per
 * tick, comfortably inside interpolation tolerance for snapshot netcode.
 */

#pragma once

#include "../../Utils/Quantization.h"

namespace Spark::Net
{
    class NetBuffer;
}

namespace Spark::Net::Quantize
{
    /// @brief Write a smallest-three 48-bit quaternion into @p buf (6 bytes).
    void WriteQuat48(NetBuffer& buf, const XMFLOAT4& q);

    /// @brief Read a smallest-three 48-bit quaternion from @p buf (consumes 6 bytes).
    ///        On short-read the buffer's error flag is set and identity is returned.
    XMFLOAT4 ReadQuat48(NetBuffer& buf);

    /// @brief Write a 16-bit-per-component bounded position into @p buf (6 bytes).
    void WriteVec16(NetBuffer& buf, const XMFLOAT3& v, const XMFLOAT3& rangeMin, const XMFLOAT3& rangeMax);

    /// @brief Read a 16-bit-per-component bounded position from @p buf (consumes 6 bytes).
    ///        On short-read the buffer's error flag is set and rangeMin is returned.
    XMFLOAT3 ReadVec16(NetBuffer& buf, const XMFLOAT3& rangeMin, const XMFLOAT3& rangeMax);

    /// @brief Byte count emitted by WriteQuat48 / WriteVec16 — useful for bandwidth
    ///        budgeting and test assertions.
    constexpr size_t kQuat48Size = 6;
    constexpr size_t kVec16Size = 6;

} // namespace Spark::Net::Quantize
