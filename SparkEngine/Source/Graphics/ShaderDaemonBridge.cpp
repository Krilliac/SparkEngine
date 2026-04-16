/**
 * @file ShaderDaemonBridge.cpp
 * @brief Implementation of the CompiledShaderBlob wire codec.
 */

#include "ShaderDaemonBridge.h"

#include "../Utils/Serializer.h"

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

        out.target = static_cast<ShaderTarget>(r.Read<uint8_t>());
        out.stage = static_cast<ShaderStage>(r.Read<uint8_t>());
        out.success = r.Read<uint8_t>() != 0;

        auto bytecodeLen = r.Read<uint32_t>();
        if (r.HasError())
            return false;
        out.bytecode.resize(bytecodeLen);
        if (bytecodeLen > 0 && !r.ReadBytes(out.bytecode.data(), bytecodeLen))
            return false;

        out.entryPoint = r.ReadString();
        out.errors = r.ReadString();

        out.inputCount = r.Read<uint32_t>();
        out.outputCount = r.Read<uint32_t>();
        out.cbufferCount = r.Read<uint32_t>();
        out.textureCount = r.Read<uint32_t>();
        out.samplerCount = r.Read<uint32_t>();

        return !r.HasError();
    }

} // namespace Spark::Graphics
