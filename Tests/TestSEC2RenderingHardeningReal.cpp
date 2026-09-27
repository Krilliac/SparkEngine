/**
 * @file TestSEC2RenderingHardeningReal.cpp
 * @brief SEC2 rendering lane: production-linked regressions for RHI and shader-cache
 *        trust-boundary fixes.
 *
 * Portable (every platform):
 *   - SEC2Render_Portable_ShaderBlobOversizedLengthLeavesOutputUntouched: a daemon cache
 *     blob whose nested bytecode length exceeds the bytes present used to resize the
 *     output vector to that length (up to ~4 GiB) before the read failed.
 *   - SEC2Render_Portable_ShaderBlobLengthAboveFrameCapRejected: a length above the
 *     16 MiB daemon frame cap is rejected even when the bytes are present.
 *   - SEC2Render_Portable_BufferRangeCheckRejectsOverflow: the shared UpdateBuffer
 *     range predicate rejects out-of-range and wrapping offset + size.
 *
 * Windows D3D11 (hardware or WARP):
 *   - SEC2Render_D3D11_UpdateBufferRejectsOutOfRange: UpdateBuffer used to memcpy past
 *     a Dynamic buffer's mapping; an out-of-range update must leave the contents intact.
 *
 * Windows D3D12 (hardware or WARP; not MinGW):
 *   - SEC2Render_D3D12_SetRenderTargetsClampsAndRejectsNull: count > 8 made
 *     OMSetRenderTargets read past the handle array; a null array with count > 0 was
 *     dereferenced.
 *   - SEC2Render_D3D12_DestroyedTexturesDeferAndRecycleDescriptors: destroying a texture
 *     released its resource immediately (GPU use-after-release) and leaked its RTV slot,
 *     so the 256-entry RTV heap ran dry after 256 create/destroy cycles.
 */

#include "TestFramework.h"

#include "Graphics/RHI/RHIResources.h"
#include "Graphics/ShaderDaemonBridge.h"

#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#ifdef _WIN32
#include "Graphics/RHI/D3D11/D3D11Device.h"
#include "Graphics/RHI/RHITypes.h"
#ifndef SPARK_NO_D3D12
#include "Graphics/RHI/D3D12/D3D12Device.h"
#endif
#include <windows.h>
#include <wrl/client.h>
#endif

namespace
{
    /// A daemon blob header (version, target, stage, success) followed by a bytecode
    /// length and @p payload bytes.
    std::vector<uint8_t> BlobWithBytecodeLength(uint32_t claimedLength, size_t payloadBytes)
    {
        std::vector<uint8_t> bytes{Spark::Graphics::kShaderDaemonBlobVersion, 0u, 0u, 1u};
        for (int shift = 0; shift < 32; shift += 8)
            bytes.push_back(static_cast<uint8_t>((claimedLength >> shift) & 0xFFu));
        bytes.resize(bytes.size() + payloadBytes, 0xABu);
        return bytes;
    }

    Spark::Graphics::CompiledShaderBlob SentinelBlob()
    {
        Spark::Graphics::CompiledShaderBlob blob;
        blob.bytecode = {0x11u, 0x22u, 0x33u};
        blob.entryPoint = "SentinelMain";
        blob.inputCount = 7;
        return blob;
    }
} // namespace

TEST(SEC2Render_Portable_ShaderBlobOversizedLengthLeavesOutputUntouched)
{
    // Claims 1 MiB of bytecode but carries 4 bytes. The old decoder resized the
    // output to 1 MiB before discovering the truncation.
    const auto truncated = BlobWithBytecodeLength(1u << 20, 4);
    auto out = SentinelBlob();
    EXPECT_FALSE(Spark::Graphics::DecodeCompiledShaderBlob(truncated, out));
    EXPECT_EQ(out.bytecode.size(), size_t(3));
    EXPECT_EQ(out.entryPoint, std::string("SentinelMain"));
    EXPECT_EQ(out.inputCount, 7u);

    // The worst case: 0xFFFFFFFF in an 8-byte blob must fail without a ~4 GiB allocation.
    const auto hostile = BlobWithBytecodeLength(std::numeric_limits<uint32_t>::max(), 0);
    EXPECT_FALSE(Spark::Graphics::DecodeCompiledShaderBlob(hostile, out));
    EXPECT_EQ(out.bytecode.size(), size_t(3));

    // A well-formed blob still decodes and replaces the output.
    Spark::Graphics::CompiledShaderBlob good;
    good.bytecode = {0xDEu, 0xADu};
    good.entryPoint = "VSMain";
    good.success = true;
    const auto encoded = Spark::Graphics::EncodeCompiledShaderBlob(good);
    ASSERT_TRUE(Spark::Graphics::DecodeCompiledShaderBlob(encoded, out));
    EXPECT_EQ(out.bytecode.size(), size_t(2));
    EXPECT_EQ(out.entryPoint, std::string("VSMain"));
    EXPECT_EQ(out.inputCount, 0u);
}

TEST(SEC2Render_Portable_ShaderBlobLengthAboveFrameCapRejected)
{
    // Every byte is present, but no daemon frame can carry more than the cap.
    const uint32_t overCap = Spark::Graphics::kMaxShaderDaemonBytecodeBytes + 1u;
    const auto blob = BlobWithBytecodeLength(overCap, overCap + 64u);
    auto out = SentinelBlob();
    EXPECT_FALSE(Spark::Graphics::DecodeCompiledShaderBlob(blob, out));
    EXPECT_EQ(out.bytecode.size(), size_t(3));
}

TEST(SEC2Render_Portable_BufferRangeCheckRejectsOverflow)
{
    using Spark::RHI::IsBufferRangeValid;
    EXPECT_TRUE(IsBufferRangeValid(16, 0, 16));
    EXPECT_TRUE(IsBufferRangeValid(16, 8, 8));
    EXPECT_FALSE(IsBufferRangeValid(16, 0, 0));
    EXPECT_FALSE(IsBufferRangeValid(16, 0, 17));
    EXPECT_FALSE(IsBufferRangeValid(16, 8, 9));
    EXPECT_FALSE(IsBufferRangeValid(16, 17, 1));
    // offset + size wraps to a small value: the old `offset + size > bufferSize` form passed this.
    constexpr uint64_t kMax = std::numeric_limits<uint64_t>::max();
    EXPECT_FALSE(IsBufferRangeValid(16, kMax, 2));
    EXPECT_FALSE(IsBufferRangeValid(16, 2, kMax));
}

#ifdef _WIN32

namespace
{
    using Microsoft::WRL::ComPtr;

    std::vector<uint8_t> ReadBackBuffer(Spark::RHI::D3D11::D3D11Device& device, Spark::RHI::IRHIBuffer& buffer)
    {
        std::vector<uint8_t> bytes;
        D3D11_BUFFER_DESC stagingDesc{};
        stagingDesc.ByteWidth = static_cast<UINT>(buffer.GetSize());
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Buffer> staging;
        if (FAILED(device.GetD3D11Device()->CreateBuffer(&stagingDesc, nullptr, &staging)))
            return bytes;

        auto* context = device.GetD3D11Context();
        context->CopyResource(staging.Get(), static_cast<ID3D11Buffer*>(buffer.GetNativeHandle()));
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
            return bytes;
        const auto* begin = static_cast<const uint8_t*>(mapped.pData);
        bytes.assign(begin, begin + buffer.GetSize());
        context->Unmap(staging.Get(), 0);
        return bytes;
    }

    bool AllBytesAre(const std::vector<uint8_t>& bytes, uint8_t value)
    {
        for (uint8_t b : bytes)
            if (b != value)
                return false;
        return !bytes.empty();
    }
} // namespace

TEST(SEC2Render_D3D11_UpdateBufferRejectsOutOfRange)
{
    Spark::RHI::D3D11::D3D11Device device;
    Spark::RHI::RHIDeviceDesc deviceDesc;
    deviceDesc.preferredBackend = Spark::RHI::GraphicsBackend::D3D11;
    deviceDesc.applicationName = "SparkTests_SEC2";
    if (!device.Initialize(deviceDesc))
        SKIP_TEST("No D3D11 device available (hardware or WARP)");

    constexpr size_t kSize = 16;
    const std::vector<uint8_t> original(kSize, 0xAAu);
    Spark::RHI::RHIBufferDesc desc;
    desc.size = kSize;
    desc.usage = Spark::RHI::RHIBufferUsage::Vertex;
    desc.access = Spark::RHI::RHIBufferAccess::Dynamic;
    desc.initialData = original.data();
    desc.debugName = "SEC2_DynamicBuffer";
    auto buffer = device.CreateBuffer(desc);
    ASSERT_TRUE(buffer != nullptr);
    ASSERT_TRUE(AllBytesAre(ReadBackBuffer(device, *buffer), 0xAAu));

    // Bytes 8..23 of a 16-byte buffer. The old code mapped with WRITE_DISCARD and
    // memcpy'd 8 bytes past the mapping; the contents visibly changed.
    const std::vector<uint8_t> hostile(32, 0xBBu);
    device.UpdateBuffer(buffer.get(), hostile.data(), 16, 8);
    // A wrapping offset and a null source must be dropped too, not dereferenced.
    device.UpdateBuffer(buffer.get(), hostile.data(), 2, std::numeric_limits<size_t>::max());
    device.UpdateBuffer(buffer.get(), nullptr, kSize, 0);
    EXPECT_TRUE(AllBytesAre(ReadBackBuffer(device, *buffer), 0xAAu));

    // An in-range update still lands.
    const std::vector<uint8_t> valid(kSize, 0xCCu);
    device.UpdateBuffer(buffer.get(), valid.data(), kSize, 0);
    EXPECT_TRUE(AllBytesAre(ReadBackBuffer(device, *buffer), 0xCCu));

    buffer.reset();
    device.Shutdown();
}

#ifndef SPARK_NO_D3D12

namespace
{
    bool TryCreateD3D12Device(Spark::RHI::D3D12::D3D12Device& device)
    {
        Spark::RHI::RHIDeviceDesc desc;
        desc.enableDebugLayer = false;
        desc.applicationName = "SparkTests_SEC2_D3D12";
        return device.Initialize(desc);
    }

    Spark::RHI::RHITextureDesc D3D12RenderTargetDesc(const char* name)
    {
        Spark::RHI::RHITextureDesc desc;
        desc.width = 8;
        desc.height = 8;
        desc.format = Spark::RHI::PixelFormat::R8G8B8A8_UNORM;
        desc.usage = Spark::RHI::RHITextureUsage::RenderTarget | Spark::RHI::RHITextureUsage::ShaderResource;
        desc.debugName = name;
        return desc;
    }
} // namespace

TEST(SEC2Render_D3D12_SetRenderTargetsClampsAndRejectsNull)
{
    Spark::RHI::D3D12::D3D12Device device;
    if (!TryCreateD3D12Device(device))
        SKIP_TEST("No D3D12 device available (hardware or WARP)");

    auto target = device.CreateTexture(D3D12RenderTargetDesc("SEC2_D3D12_Target"));
    ASSERT_TRUE(target != nullptr);
    ASSERT_TRUE(target->GetRenderTargetView() != nullptr);

    auto* cmd = device.GetImmediateCommandList();
    ASSERT_TRUE(cmd != nullptr);
    cmd->Begin();
    // Nine entries: one more than D3D12 can bind. Previously OMSetRenderTargets was
    // told 9 and read a ninth handle from past the 8-entry stack array.
    Spark::RHI::IRHITexture* nine[9] = {target.get(), target.get(), target.get(), target.get(), target.get(),
                                        target.get(), target.get(), target.get(), target.get()};
    cmd->SetRenderTargets(nine, 9, nullptr);
    // A null array with a non-zero count was dereferenced at renderTargets[0].
    cmd->SetRenderTargets(nullptr, 3, nullptr);
    cmd->End();
    device.ExecuteCommandList(cmd);
    device.WaitForIdle();

    EXPECT_TRUE(SUCCEEDED(device.GetD3D12Device()->GetDeviceRemovedReason()));

    target.reset();
    device.Shutdown();
}

TEST(SEC2Render_D3D12_DestroyedTexturesDeferAndRecycleDescriptors)
{
    Spark::RHI::D3D12::D3D12Device device;
    if (!TryCreateD3D12Device(device))
        SKIP_TEST("No D3D12 device available (hardware or WARP)");

    // A texture the GPU is still using: clear it, submit, and destroy it before the
    // fence is waited on. Its resource must stay alive in the release queue.
    {
        auto inFlight = device.CreateTexture(D3D12RenderTargetDesc("SEC2_D3D12_InFlight"));
        ASSERT_TRUE(inFlight != nullptr);
        auto* cmd = static_cast<Spark::RHI::D3D12::D3D12CommandList*>(device.GetImmediateCommandList());
        auto* native = static_cast<Spark::RHI::D3D12::D3D12Texture*>(inFlight.get());
        cmd->Begin();
        cmd->TransitionBarrier(native, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
        cmd->FlushBarriers();
        Spark::RHI::IRHITexture* targets[] = {inFlight.get()};
        cmd->SetRenderTargets(targets, 1, nullptr);
        const float clearColor[4] = {0.25f, 0.5f, 0.75f, 1.0f};
        cmd->ClearRenderTarget(inFlight.get(), clearColor);
        cmd->TransitionBarrier(native, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
        cmd->End();
        device.ExecuteCommandList(cmd);

        const size_t pendingBefore = device.GetPendingReleaseCount();
        inFlight.reset();
        EXPECT_EQ(device.GetPendingReleaseCount(), pendingBefore + 1);

        device.WaitForIdle();
        device.BeginFrame(); // processes releases whose fence has completed
        EXPECT_EQ(device.GetPendingReleaseCount(), size_t(0));
        device.EndFrame();
    }

    // The RTV heap holds 256 descriptors. Without recycling, cycle 257 gets no RTV.
    constexpr uint32_t kCycles = Spark::RHI::D3D12::RTV_HEAP_SIZE + 44;
    uint32_t missingRtv = 0;
    for (uint32_t i = 0; i < kCycles; ++i)
    {
        device.BeginFrame();
        auto texture = device.CreateTexture(D3D12RenderTargetDesc("SEC2_D3D12_Churn"));
        if (!texture || texture->GetRenderTargetView() == nullptr)
            ++missingRtv;
        texture.reset();
        device.EndFrame();
    }
    EXPECT_EQ(missingRtv, 0u);
    EXPECT_TRUE(SUCCEEDED(device.GetD3D12Device()->GetDeviceRemovedReason()));

    device.WaitForIdle();
    device.Shutdown();
    EXPECT_EQ(device.GetPendingReleaseCount(), size_t(0));
}

#endif // SPARK_NO_D3D12
#endif // _WIN32
