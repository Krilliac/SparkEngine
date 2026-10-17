/**
 * @file ShaderDaemonBridge.cpp
 * @brief Implementation of the CompiledShaderBlob wire codec.
 */

#include "ShaderDaemonBridge.h"

#include "../Utils/Serializer.h"

#include <utility>

namespace Spark::Graphics
{

    std::vector<uint8_t> EncodeCompiledShaderBlob(const CompiledShaderBlob& blob)
    {
        Spark::BinaryWriter w;
        w.Write<uint8_t>(kShaderDaemonBlobVersion);
        w.Write<uint8_t>(static_cast<uint8_t>(blob.target));
        w.Write<uint8_t>(static_cast<uint8_t>(blob.stage));
        w.Write<uint8_t>(blob.success ? 1u : 0u);

        w.Write<uint32_t>(static_cast<uint32_t>(blob.bytecode.size()));
        if (!blob.bytecode.empty())
            w.WriteBytes(blob.bytecode.data(), blob.bytecode.size());

        w.WriteString(blob.entryPoint);
        w.WriteString(blob.errors);

        w.Write<uint32_t>(blob.inputCount);
        w.Write<uint32_t>(blob.outputCount);
        w.Write<uint32_t>(blob.cbufferCount);
        w.Write<uint32_t>(blob.textureCount);
        w.Write<uint32_t>(blob.samplerCount);

        return w.TakeBuffer();
    }

    bool DecodeCompiledShaderBlob(const std::vector<uint8_t>& bytes, CompiledShaderBlob& out)
    {
        Spark::BinaryReader r(bytes);
        auto version = r.Read<uint8_t>();
        if (r.HasError() || version != kShaderDaemonBlobVersion)
            return false;

        // Decode into a local and publish only on success, so a malformed blob never
        // leaves `out` half-written.
        CompiledShaderBlob decoded;
        decoded.target = static_cast<ShaderTarget>(r.Read<uint8_t>());
        decoded.stage = static_cast<ShaderStage>(r.Read<uint8_t>());
        decoded.success = r.Read<uint8_t>() != 0;

        // The length comes from the daemon's opaque store: bound it by the bytes actually
        // present (and the frame cap) before it sizes an allocation. Resizing first let an
        // 8-byte blob claiming 0xFFFFFFFF bytes allocate ~4 GiB before the read failed.
        const auto bytecodeLen = r.Read<uint32_t>();
        if (r.HasError() || bytecodeLen > r.Remaining() || bytecodeLen > kMaxShaderDaemonBytecodeBytes)
            return false;
        decoded.bytecode.resize(bytecodeLen);
        if (bytecodeLen > 0 && !r.ReadBytes(decoded.bytecode.data(), bytecodeLen))
            return false;

        decoded.entryPoint = r.ReadString();
        decoded.errors = r.ReadString();

        decoded.inputCount = r.Read<uint32_t>();
        decoded.outputCount = r.Read<uint32_t>();
        decoded.cbufferCount = r.Read<uint32_t>();
        decoded.textureCount = r.Read<uint32_t>();
        decoded.samplerCount = r.Read<uint32_t>();

        if (r.HasError())
        {
            return false;
        }
        out = std::move(decoded);
        return true;
    }

} // namespace Spark::Graphics
