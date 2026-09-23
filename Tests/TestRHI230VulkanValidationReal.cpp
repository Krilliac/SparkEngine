/**
 * @file TestRHI230VulkanValidationReal.cpp
 * @brief RHI-230: Vulkan backend exercised under the Khronos validation layer.
 *
 * Every test builds a real VulkanDevice with enableDebugLayer=true, attaches a
 * VkDebugUtilsMessengerEXT that counts ERROR-severity messages, drives one RHI
 * path, and asserts the count stays at zero. Each test is the regression for a
 * validation defect found while running the backend on Mesa Lavapipe.
 *
 * Lavapipe is a CPU implementation: a clean run here proves API-usage
 * correctness (layouts, barriers, descriptor lifetime, submission state), not
 * hardware certification. Tests skip when no Vulkan ICD or no
 * VK_LAYER_KHRONOS_validation is installed.
 *
 * SPIR-V fixtures were produced with glslangValidator -V and
 * spirv-opt --strip-debug from these GLSL 450 sources:
 *   VS: fullscreen triangle from gl_VertexIndex, no vertex inputs
 *   PS (solid): outColor = vec4(0, 1, 0, 1)
 *   PS (tint):  layout(set = 0, binding = 0) uniform Tint { vec4 color; }; outColor = color
 */

#include "TestFramework.h"

#ifdef SPARK_VULKAN_SUPPORT

#include "Graphics/RHI/Vulkan/VulkanDevice.h"

#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
    using namespace Spark::RHI;
    using Spark::RHI::Vulkan::VulkanDevice;

    // glslangValidator -V + spirv-opt --strip-debug of tri.vert (see comment above)
    const uint32_t kFullscreenTriangleVS[] = {
        0x07230203, 0x00010000, 0x0008000b, 0x00000028, 0x00000000, 0x00020011, 0x00000001, 0x0006000b, 0x00000001,
        0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001, 0x0007000f, 0x00000000,
        0x00000004, 0x6e69616d, 0x00000000, 0x0000000d, 0x0000001a, 0x00030047, 0x0000000b, 0x00000002, 0x00050048,
        0x0000000b, 0x00000000, 0x0000000b, 0x00000000, 0x00050048, 0x0000000b, 0x00000001, 0x0000000b, 0x00000001,
        0x00050048, 0x0000000b, 0x00000002, 0x0000000b, 0x00000003, 0x00050048, 0x0000000b, 0x00000003, 0x0000000b,
        0x00000004, 0x00040047, 0x0000001a, 0x0000000b, 0x0000002a, 0x00020013, 0x00000002, 0x00030021, 0x00000003,
        0x00000002, 0x00030016, 0x00000006, 0x00000020, 0x00040017, 0x00000007, 0x00000006, 0x00000004, 0x00040015,
        0x00000008, 0x00000020, 0x00000000, 0x0004002b, 0x00000008, 0x00000009, 0x00000001, 0x0004001c, 0x0000000a,
        0x00000006, 0x00000009, 0x0006001e, 0x0000000b, 0x00000007, 0x00000006, 0x0000000a, 0x0000000a, 0x00040020,
        0x0000000c, 0x00000003, 0x0000000b, 0x0004003b, 0x0000000c, 0x0000000d, 0x00000003, 0x00040015, 0x0000000e,
        0x00000020, 0x00000001, 0x0004002b, 0x0000000e, 0x0000000f, 0x00000000, 0x00040017, 0x00000010, 0x00000006,
        0x00000002, 0x0004002b, 0x00000008, 0x00000011, 0x00000003, 0x0004001c, 0x00000012, 0x00000010, 0x00000011,
        0x0004002b, 0x00000006, 0x00000013, 0xbf800000, 0x0005002c, 0x00000010, 0x00000014, 0x00000013, 0x00000013,
        0x0004002b, 0x00000006, 0x00000015, 0x40400000, 0x0005002c, 0x00000010, 0x00000016, 0x00000015, 0x00000013,
        0x0005002c, 0x00000010, 0x00000017, 0x00000013, 0x00000015, 0x0006002c, 0x00000012, 0x00000018, 0x00000014,
        0x00000016, 0x00000017, 0x00040020, 0x00000019, 0x00000001, 0x0000000e, 0x0004003b, 0x00000019, 0x0000001a,
        0x00000001, 0x00040020, 0x0000001c, 0x00000007, 0x00000012, 0x00040020, 0x0000001e, 0x00000007, 0x00000010,
        0x0004002b, 0x00000006, 0x00000021, 0x00000000, 0x0004002b, 0x00000006, 0x00000022, 0x3f800000, 0x00040020,
        0x00000026, 0x00000003, 0x00000007, 0x00050036, 0x00000002, 0x00000004, 0x00000000, 0x00000003, 0x000200f8,
        0x00000005, 0x0004003b, 0x0000001c, 0x0000001d, 0x00000007, 0x0004003d, 0x0000000e, 0x0000001b, 0x0000001a,
        0x0003003e, 0x0000001d, 0x00000018, 0x00050041, 0x0000001e, 0x0000001f, 0x0000001d, 0x0000001b, 0x0004003d,
        0x00000010, 0x00000020, 0x0000001f, 0x00050051, 0x00000006, 0x00000023, 0x00000020, 0x00000000, 0x00050051,
        0x00000006, 0x00000024, 0x00000020, 0x00000001, 0x00070050, 0x00000007, 0x00000025, 0x00000023, 0x00000024,
        0x00000021, 0x00000022, 0x00050041, 0x00000026, 0x00000027, 0x0000000d, 0x0000000f, 0x0003003e, 0x00000027,
        0x00000025, 0x000100fd, 0x00010038,
    };

    // glslangValidator -V + spirv-opt --strip-debug of solid.frag (see comment above)
    const uint32_t kSolidGreenPS[] = {
        0x07230203, 0x00010000, 0x0008000b, 0x0000000d, 0x00000000, 0x00020011, 0x00000001, 0x0006000b, 0x00000001,
        0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001, 0x0006000f, 0x00000004,
        0x00000004, 0x6e69616d, 0x00000000, 0x00000009, 0x00030010, 0x00000004, 0x00000007, 0x00040047, 0x00000009,
        0x0000001e, 0x00000000, 0x00020013, 0x00000002, 0x00030021, 0x00000003, 0x00000002, 0x00030016, 0x00000006,
        0x00000020, 0x00040017, 0x00000007, 0x00000006, 0x00000004, 0x00040020, 0x00000008, 0x00000003, 0x00000007,
        0x0004003b, 0x00000008, 0x00000009, 0x00000003, 0x0004002b, 0x00000006, 0x0000000a, 0x00000000, 0x0004002b,
        0x00000006, 0x0000000b, 0x3f800000, 0x0007002c, 0x00000007, 0x0000000c, 0x0000000a, 0x0000000b, 0x0000000a,
        0x0000000b, 0x00050036, 0x00000002, 0x00000004, 0x00000000, 0x00000003, 0x000200f8, 0x00000005, 0x0003003e,
        0x00000009, 0x0000000c, 0x000100fd, 0x00010038,
    };

    // glslangValidator -V + spirv-opt --strip-debug of ubo.frag (see comment above)
    const uint32_t kUniformTintPS[] = {
        0x07230203, 0x00010000, 0x0008000b, 0x00000012, 0x00000000, 0x00020011, 0x00000001, 0x0006000b, 0x00000001,
        0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001, 0x0006000f, 0x00000004,
        0x00000004, 0x6e69616d, 0x00000000, 0x00000009, 0x00030010, 0x00000004, 0x00000007, 0x00040047, 0x00000009,
        0x0000001e, 0x00000000, 0x00030047, 0x0000000a, 0x00000002, 0x00050048, 0x0000000a, 0x00000000, 0x00000023,
        0x00000000, 0x00040047, 0x0000000c, 0x00000021, 0x00000000, 0x00040047, 0x0000000c, 0x00000022, 0x00000000,
        0x00020013, 0x00000002, 0x00030021, 0x00000003, 0x00000002, 0x00030016, 0x00000006, 0x00000020, 0x00040017,
        0x00000007, 0x00000006, 0x00000004, 0x00040020, 0x00000008, 0x00000003, 0x00000007, 0x0004003b, 0x00000008,
        0x00000009, 0x00000003, 0x0003001e, 0x0000000a, 0x00000007, 0x00040020, 0x0000000b, 0x00000002, 0x0000000a,
        0x0004003b, 0x0000000b, 0x0000000c, 0x00000002, 0x00040015, 0x0000000d, 0x00000020, 0x00000001, 0x0004002b,
        0x0000000d, 0x0000000e, 0x00000000, 0x00040020, 0x0000000f, 0x00000002, 0x00000007, 0x00050036, 0x00000002,
        0x00000004, 0x00000000, 0x00000003, 0x000200f8, 0x00000005, 0x00050041, 0x0000000f, 0x00000010, 0x0000000c,
        0x0000000e, 0x0004003d, 0x00000007, 0x00000011, 0x00000010, 0x0003003e, 0x00000009, 0x00000011, 0x000100fd,
        0x00010038,
    };


    struct ValidationCounter
    {
        uint32_t errors = 0;
        std::string firstError;
    };

    VKAPI_ATTR VkBool32 VKAPI_CALL CountValidationMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                          VkDebugUtilsMessageTypeFlagsEXT,
                                                          const VkDebugUtilsMessengerCallbackDataEXT* data,
                                                          void* userData)
    {
        auto* counter = static_cast<ValidationCounter*>(userData);
        if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
        {
            if (counter->errors == 0 && data && data->pMessage)
                counter->firstError = data->pMessage;
            ++counter->errors;
        }
        return VK_FALSE;
    }

    bool ValidationLayerInstalled()
    {
        uint32_t count = 0;
        vkEnumerateInstanceLayerProperties(&count, nullptr);
        std::vector<VkLayerProperties> layers(count);
        vkEnumerateInstanceLayerProperties(&count, layers.data());
        for (const auto& layer : layers)
        {
            if (std::strcmp(layer.layerName, "VK_LAYER_KHRONOS_validation") == 0)
                return true;
        }
        return false;
    }

    /// A lane that sets SPARK_REQUIRE_VULKAN_VALIDATION=1 (the vulkan-lavapipe CI row) must not pass by
    /// skipping every test, so a missing driver or layer becomes a failure there.
    [[noreturn]] void SkipOrFail(const char* reason)
    {
        if (std::getenv("SPARK_REQUIRE_VULKAN_VALIDATION") != nullptr)
            throw std::runtime_error(std::string("required Vulkan validation unavailable: ") + reason);
        SKIP_TEST(reason);
    }

    /// Real VulkanDevice with the validation layer on and an error counter attached.
    struct ValidatedDevice
    {
        VulkanDevice device;
        ValidationCounter counter;
        VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;

        ValidatedDevice()
        {
            if (!ValidationLayerInstalled())
                SkipOrFail("VK_LAYER_KHRONOS_validation is not installed");

            RHIDeviceDesc desc;
            desc.enableDebugLayer = true;
            desc.applicationName = "RHI230Validation";
            if (!device.Initialize(desc))
                SkipOrFail("no Vulkan ICD available (install mesa-vulkan-drivers for Lavapipe)");

            VkDebugUtilsMessengerCreateInfoEXT info = {};
            info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
            info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            info.messageType =
                VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
            info.pfnUserCallback = CountValidationMessage;
            info.pUserData = &counter;
            auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(device.GetVkInstance(), "vkCreateDebugUtilsMessengerEXT"));
            if (create)
                create(device.GetVkInstance(), &info, nullptr, &messenger);
        }

        ~ValidatedDevice()
        {
            device.WaitForIdle();
            DestroyMessenger();
            device.Shutdown();
        }

        void DestroyMessenger()
        {
            if (messenger == VK_NULL_HANDLE)
                return;
            auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(device.GetVkInstance(), "vkDestroyDebugUtilsMessengerEXT"));
            if (destroy)
                destroy(device.GetVkInstance(), messenger, nullptr);
            messenger = VK_NULL_HANDLE;
        }

        bool HasMessenger() const { return messenger != VK_NULL_HANDLE; }

        void ExpectClean()
        {
            EXPECT_TRUE(HasMessenger());
            if (counter.errors != 0)
                std::cerr << "  first validation error: " << counter.firstError << "\n";
            EXPECT_EQ(counter.errors, 0u);
        }
    };

    std::unique_ptr<IRHITexture> MakeColorTarget(IRHIDevice& device, uint32_t size = 16)
    {
        RHITextureDesc desc;
        desc.width = size;
        desc.height = size;
        desc.format = PixelFormat::R8G8B8A8_UNORM;
        desc.usage = RHITextureUsage::RenderTarget | RHITextureUsage::ShaderResource | RHITextureUsage::TransferSrc;
        desc.debugName = "RHI230ColorTarget";
        return device.CreateTexture(desc);
    }

    std::unique_ptr<IRHIShader> MakeShader(IRHIDevice& device, RHIShaderStage stage, const uint32_t* words,
                                           size_t byteSize)
    {
        RHIShaderDesc desc;
        desc.stage = stage;
        desc.language = ShaderLanguage::SPIRV;
        desc.bytecode = words;
        desc.bytecodeSize = byteSize;
        return device.CreateShader(desc);
    }

    std::unique_ptr<IRHIPipelineState> MakeFullscreenPipeline(IRHIDevice& device, IRHIShader* vs, IRHIShader* ps)
    {
        RHIPipelineStateDesc desc;
        desc.numRenderTargets = 1;
        desc.renderTargetFormats[0] = PixelFormat::R8G8B8A8_UNORM;
        desc.depthStencilFormat = PixelFormat::Unknown;
        desc.depthStencil.depthEnable = false;
        desc.depthStencil.depthWrite = false;
        desc.rasterizer.cullMode = RHICullMode::None;
        desc.debugName = "RHI230Fullscreen";
        return device.CreatePipelineState(desc, vs, ps);
    }

    void BindFullViewport(IRHICommandList& cmd, uint32_t size)
    {
        RHIViewport viewport;
        viewport.width = static_cast<float>(size);
        viewport.height = static_cast<float>(size);
        cmd.SetViewport(viewport);
        RHIScissorRect scissor;
        scissor.right = static_cast<int32_t>(size);
        scissor.bottom = static_cast<int32_t>(size);
        cmd.SetScissorRect(scissor);
    }

} // namespace

// ============================================================================
// Device lifecycle
// ============================================================================

TEST(VulkanValidation_InitShutdownClean)
{
    ValidatedDevice v;
    v.ExpectClean();
}

// ============================================================================
// Frame lifecycle: EndFrame must never submit a command buffer that was not
// recorded, and the immediate list must not be re-begun while still pending.
// ============================================================================

TEST(VulkanValidation_EmptyFrameLoopClean)
{
    ValidatedDevice v;
    for (int frame = 0; frame < 4; ++frame)
    {
        v.device.BeginFrame();
        v.device.EndFrame();
    }
    v.device.WaitForIdle();
    v.ExpectClean();
}

TEST(VulkanValidation_RecordedFrameLoopClean)
{
    ValidatedDevice v;
    auto target = MakeColorTarget(v.device);
    ASSERT_TRUE(target != nullptr);

    const float clear[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    for (int frame = 0; frame < 4; ++frame)
    {
        v.device.BeginFrame();
        IRHICommandList* cmd = v.device.GetImmediateCommandList();
        cmd->Begin();
        cmd->ClearRenderTarget(target.get(), clear);
        cmd->End();
        v.device.EndFrame();
    }
    v.device.WaitForIdle();
    v.ExpectClean();
}

// ============================================================================
// Image layouts: a freshly created texture is UNDEFINED; clears and copies
// must transition it before use.
// ============================================================================

TEST(VulkanValidation_ClearFreshTextureClean)
{
    ValidatedDevice v;
    auto target = MakeColorTarget(v.device);
    ASSERT_TRUE(target != nullptr);

    IRHICommandList* cmd = v.device.GetImmediateCommandList();
    const float clear[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    cmd->Begin();
    cmd->ClearRenderTarget(target.get(), clear);
    cmd->End();
    v.device.ExecuteCommandList(cmd);
    v.device.WaitForIdle();
    v.ExpectClean();
}

TEST(VulkanValidation_CopyTextureClean)
{
    ValidatedDevice v;
    auto source = MakeColorTarget(v.device);
    auto dest = MakeColorTarget(v.device);
    ASSERT_TRUE(source != nullptr && dest != nullptr);

    IRHICommandList* cmd = v.device.GetImmediateCommandList();
    const float clear[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    cmd->Begin();
    cmd->ClearRenderTarget(source.get(), clear);
    cmd->CopyTexture(dest.get(), source.get());
    cmd->End();
    v.device.ExecuteCommandList(cmd);
    v.device.WaitForIdle();
    v.ExpectClean();
}

// ============================================================================
// Dynamic rendering: SetRenderTargets must be closed with vkCmdEndRendering
// before End(), and attachments must be in COLOR_ATTACHMENT_OPTIMAL.
// ============================================================================

TEST(VulkanValidation_DrawToRenderTargetClean)
{
    ValidatedDevice v;
    constexpr uint32_t size = 16;
    auto target = MakeColorTarget(v.device, size);
    auto vs = MakeShader(v.device, RHIShaderStage::Vertex, kFullscreenTriangleVS, sizeof(kFullscreenTriangleVS));
    auto ps = MakeShader(v.device, RHIShaderStage::Pixel, kSolidGreenPS, sizeof(kSolidGreenPS));
    ASSERT_TRUE(target && vs && ps);
    auto pipeline = MakeFullscreenPipeline(v.device, vs.get(), ps.get());
    ASSERT_TRUE(pipeline != nullptr);

    IRHICommandList* cmd = v.device.GetImmediateCommandList();
    IRHITexture* targets[] = {target.get()};
    cmd->Begin();
    cmd->SetRenderTargets(targets, 1, nullptr);
    BindFullViewport(*cmd, size);
    cmd->SetPipelineState(pipeline.get());
    cmd->Draw(3, 0);
    cmd->End();
    v.device.ExecuteCommandList(cmd);
    v.device.WaitForIdle();
    v.ExpectClean();
}

// The redundant-bind cache must not survive into a new recording: the second
// command buffer starts with no pipeline bound.
TEST(VulkanValidation_PipelineRebindAcrossRecordingsClean)
{
    ValidatedDevice v;
    constexpr uint32_t size = 16;
    auto target = MakeColorTarget(v.device, size);
    auto vs = MakeShader(v.device, RHIShaderStage::Vertex, kFullscreenTriangleVS, sizeof(kFullscreenTriangleVS));
    auto ps = MakeShader(v.device, RHIShaderStage::Pixel, kSolidGreenPS, sizeof(kSolidGreenPS));
    ASSERT_TRUE(target && vs && ps);
    auto pipeline = MakeFullscreenPipeline(v.device, vs.get(), ps.get());
    ASSERT_TRUE(pipeline != nullptr);

    IRHICommandList* cmd = v.device.GetImmediateCommandList();
    IRHITexture* targets[] = {target.get()};
    for (int pass = 0; pass < 2; ++pass)
    {
        cmd->Begin();
        cmd->SetRenderTargets(targets, 1, nullptr);
        BindFullViewport(*cmd, size);
        cmd->SetPipelineState(pipeline.get());
        cmd->Draw(3, 0);
        cmd->End();
        v.device.ExecuteCommandList(cmd);
        v.device.WaitForIdle();
    }
    v.ExpectClean();
}

// ============================================================================
// Descriptors: constant buffers bound through SetConstantBuffer must reach the
// draw, via push descriptors or a pool-allocated set.
// ============================================================================

TEST(VulkanValidation_ConstantBufferBindingClean)
{
    ValidatedDevice v;
    constexpr uint32_t size = 16;
    auto target = MakeColorTarget(v.device, size);
    auto vs = MakeShader(v.device, RHIShaderStage::Vertex, kFullscreenTriangleVS, sizeof(kFullscreenTriangleVS));
    auto ps = MakeShader(v.device, RHIShaderStage::Pixel, kUniformTintPS, sizeof(kUniformTintPS));
    ASSERT_TRUE(target && vs && ps);
    auto pipeline = MakeFullscreenPipeline(v.device, vs.get(), ps.get());
    ASSERT_TRUE(pipeline != nullptr);

    const float tint[4] = {1.0f, 0.0f, 1.0f, 1.0f};
    RHIBufferDesc cbDesc;
    cbDesc.size = sizeof(tint);
    cbDesc.usage = RHIBufferUsage::Constant;
    cbDesc.access = RHIBufferAccess::Dynamic;
    cbDesc.initialData = tint;
    auto constants = v.device.CreateBuffer(cbDesc);
    ASSERT_TRUE(constants != nullptr);

    IRHICommandList* cmd = v.device.GetImmediateCommandList();
    IRHITexture* targets[] = {target.get()};
    cmd->Begin();
    cmd->SetRenderTargets(targets, 1, nullptr);
    BindFullViewport(*cmd, size);
    cmd->SetPipelineState(pipeline.get());
    cmd->SetConstantBuffer(RHIShaderStage::Pixel, 0, constants.get());
    cmd->Draw(3, 0);
    cmd->End();
    v.device.ExecuteCommandList(cmd);
    v.device.WaitForIdle();
    v.ExpectClean();
}

// ============================================================================
// Buffer memory: UpdateBuffer on a GPU-only (Static) buffer must not map
// device-local memory.
// ============================================================================

TEST(VulkanValidation_UpdateStaticBufferClean)
{
    ValidatedDevice v;
    RHIBufferDesc desc;
    desc.size = 64;
    desc.usage = RHIBufferUsage::Constant;
    desc.access = RHIBufferAccess::Static;
    auto buffer = v.device.CreateBuffer(desc);
    ASSERT_TRUE(buffer != nullptr);

    std::vector<uint8_t> payload(64, 0x5A);
    v.device.UpdateBuffer(buffer.get(), payload.data(), payload.size(), 0);
    v.device.WaitForIdle();
    v.ExpectClean();
}

// ============================================================================
// Command buffer lifetime: a deferred list destroyed right after Execute must
// not free a command buffer that is still pending on the queue.
// ============================================================================

TEST(VulkanValidation_DeferredListDestroyedAfterExecuteClean)
{
    ValidatedDevice v;
    auto target = MakeColorTarget(v.device);
    ASSERT_TRUE(target != nullptr);

    {
        auto deferred = v.device.CreateDeferredCommandList();
        ASSERT_TRUE(deferred != nullptr);
        const float clear[4] = {0.0f, 1.0f, 1.0f, 1.0f};
        deferred->Begin();
        deferred->ClearRenderTarget(target.get(), clear);
        deferred->End();
        v.device.ExecuteCommandList(deferred.get());
    }
    v.device.WaitForIdle();
    v.ExpectClean();
}

// ============================================================================
// Shader toolchain: malformed SPIR-V must be rejected before it reaches
// vkCreateShaderModule; valid SPIR-V must be accepted.
// ============================================================================

TEST(VulkanShaderToolchain_RejectsMalformedSpirv)
{
    ValidatedDevice v;

    // Not a multiple of four bytes.
    const uint8_t truncated[7] = {0x03, 0x02, 0x23, 0x07, 0x00, 0x00, 0x01};
    RHIShaderDesc desc;
    desc.stage = RHIShaderStage::Vertex;
    desc.language = ShaderLanguage::SPIRV;
    desc.bytecode = truncated;
    desc.bytecodeSize = sizeof(truncated);
    EXPECT_TRUE(v.device.CreateShader(desc) == nullptr);

    // Right size, wrong magic number (HLSL/DXBC-style bytes mislabeled as SPIR-V).
    const uint32_t wrongMagic[8] = {0x43425844u, 0, 0, 0, 0, 0, 0, 0};
    desc.bytecode = wrongMagic;
    desc.bytecodeSize = sizeof(wrongMagic);
    EXPECT_TRUE(v.device.CreateShader(desc) == nullptr);

    // Header only: a module needs at least the 5-word header plus instructions.
    const uint32_t headerOnly[5] = {0x07230203u, 0x00010000u, 0, 1, 0};
    desc.bytecode = headerOnly;
    desc.bytecodeSize = sizeof(headerOnly);
    EXPECT_TRUE(v.device.CreateShader(desc) == nullptr);

    v.ExpectClean();
}

TEST(VulkanShaderToolchain_AcceptsValidSpirv)
{
    ValidatedDevice v;
    auto vs = MakeShader(v.device, RHIShaderStage::Vertex, kFullscreenTriangleVS, sizeof(kFullscreenTriangleVS));
    auto ps = MakeShader(v.device, RHIShaderStage::Pixel, kSolidGreenPS, sizeof(kSolidGreenPS));
    EXPECT_TRUE(vs != nullptr);
    EXPECT_TRUE(ps != nullptr);
    v.ExpectClean();
}

// ============================================================================
// Real render + readback: the pixels come from Lavapipe executing the SPIR-V,
// not from the CPU-synthesized RenderCanonicalGoldenScene route.
// ============================================================================

namespace
{
    // Clears to red, draws the fullscreen triangle with @p ps, reads the target back.
    std::vector<uint8_t> RenderFullscreen(ValidatedDevice& v, uint32_t size, IRHIShader* ps, IRHIBuffer* constants)
    {
        auto target = MakeColorTarget(v.device, size);
        auto vs = MakeShader(v.device, RHIShaderStage::Vertex, kFullscreenTriangleVS, sizeof(kFullscreenTriangleVS));
        if (!target || !vs || !ps)
            return {};
        auto pipeline = MakeFullscreenPipeline(v.device, vs.get(), ps);
        if (!pipeline)
            return {};

        IRHICommandList* cmd = v.device.GetImmediateCommandList();
        IRHITexture* targets[] = {target.get()};
        const float red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
        cmd->Begin();
        cmd->ClearRenderTarget(target.get(), red);
        cmd->SetRenderTargets(targets, 1, nullptr);
        BindFullViewport(*cmd, size);
        cmd->SetPipelineState(pipeline.get());
        if (constants)
            cmd->SetConstantBuffer(RHIShaderStage::Pixel, 0, constants);
        cmd->Draw(3, 0);
        cmd->End();
        v.device.ExecuteCommandList(cmd);
        v.device.WaitForIdle();
        return v.device.ReadbackTexture(target.get());
    }

    size_t CountPixels(const std::vector<uint8_t>& rgba, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
    {
        size_t matches = 0;
        for (size_t i = 0; i + 3 < rgba.size(); i += 4)
        {
            if (rgba[i] == r && rgba[i + 1] == g && rgba[i + 2] == b && rgba[i + 3] == a)
                ++matches;
        }
        return matches;
    }
} // namespace

TEST(VulkanGolden_FullscreenTriangleReadback)
{
    ValidatedDevice v;
    constexpr uint32_t size = 16;
    auto ps = MakeShader(v.device, RHIShaderStage::Pixel, kSolidGreenPS, sizeof(kSolidGreenPS));
    ASSERT_TRUE(ps != nullptr);

    const std::vector<uint8_t> pixels = RenderFullscreen(v, size, ps.get(), nullptr);
    ASSERT_EQ(pixels.size(), static_cast<size_t>(size * size * 4));
    // The oversized triangle covers the whole target: no red clear color may survive.
    EXPECT_EQ(CountPixels(pixels, 0, 255, 0, 255), static_cast<size_t>(size * size));
    v.ExpectClean();
}

// A constant buffer bound with SetConstantBuffer must be what the shader reads. Before the descriptor flush
// existed this drew with an unbound set (VUID-vkCmdDraw-None-08600) and crashed Lavapipe.
TEST(VulkanGolden_ConstantBufferTintReadback)
{
    ValidatedDevice v;
    constexpr uint32_t size = 8;
    auto ps = MakeShader(v.device, RHIShaderStage::Pixel, kUniformTintPS, sizeof(kUniformTintPS));
    ASSERT_TRUE(ps != nullptr);

    const float magenta[4] = {1.0f, 0.0f, 1.0f, 1.0f};
    RHIBufferDesc cbDesc;
    cbDesc.size = sizeof(magenta);
    cbDesc.usage = RHIBufferUsage::Constant;
    cbDesc.access = RHIBufferAccess::Static; // device-local: exercises the staging upload path
    cbDesc.initialData = magenta;
    auto constants = v.device.CreateBuffer(cbDesc);
    ASSERT_TRUE(constants != nullptr);

    const std::vector<uint8_t> pixels = RenderFullscreen(v, size, ps.get(), constants.get());
    ASSERT_EQ(pixels.size(), static_cast<size_t>(size * size * 4));
    EXPECT_EQ(CountPixels(pixels, 255, 0, 255, 255), static_cast<size_t>(size * size));

    // UpdateBuffer on the device-local buffer must reach the GPU through staging, not a host map.
    const float cyan[4] = {0.0f, 1.0f, 1.0f, 1.0f};
    v.device.UpdateBuffer(constants.get(), cyan, sizeof(cyan), 0);
    const std::vector<uint8_t> updated = RenderFullscreen(v, size, ps.get(), constants.get());
    EXPECT_EQ(CountPixels(updated, 0, 255, 255, 255), static_cast<size_t>(size * size));
    v.ExpectClean();
}

TEST(VulkanValidation_ReadbackRejectsDepthTexture)
{
    ValidatedDevice v;
    RHITextureDesc desc;
    desc.width = 8;
    desc.height = 8;
    desc.format = PixelFormat::D32_FLOAT;
    desc.usage = RHITextureUsage::DepthStencil;
    auto depth = v.device.CreateTexture(desc);
    ASSERT_TRUE(depth != nullptr);
    EXPECT_TRUE(v.device.ReadbackTexture(depth.get()).empty());
    v.ExpectClean();
}

// ============================================================================
// Swap chain: acquire/present/resize on a VK_EXT_headless_surface. Before the
// fix Present never acquired an image, waited on a semaphore nothing signaled,
// double-destroyed image views and leaked the surface.
// ============================================================================

TEST(VulkanValidation_HeadlessSwapChainPresentAndResizeClean)
{
    ValidatedDevice v;
    if (!v.device.SupportsHeadlessSurface())
        SKIP_TEST("ICD does not expose VK_EXT_headless_surface");

    VkHeadlessSurfaceCreateInfoEXT surfaceInfo = {};
    surfaceInfo.sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT;
    auto createSurface = reinterpret_cast<PFN_vkCreateHeadlessSurfaceEXT>(
        vkGetInstanceProcAddr(v.device.GetVkInstance(), "vkCreateHeadlessSurfaceEXT"));
    ASSERT_TRUE(createSurface != nullptr);
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    ASSERT_TRUE(createSurface(v.device.GetVkInstance(), &surfaceInfo, nullptr, &surface) == VK_SUCCESS);

    RHISwapChainDesc desc;
    desc.width = 32;
    desc.height = 32;
    desc.bufferCount = 2;
    desc.vsync = true;
    {
        // The swap chain owns the surface and destroys it before the device shuts down.
        Spark::RHI::Vulkan::VulkanSwapChain swapChain(v.device.GetVkInstance(), v.device.GetVkDevice(),
                                                      v.device.GetVkPhysicalDevice(), surface, desc,
                                                      v.device.GetQueueFamilies(), v.device.GetPresentQueue());
        ASSERT_TRUE(swapChain.IsValid());

        const float blue[4] = {0.0f, 0.0f, 1.0f, 1.0f};
        auto presentFrames = [&](int frames)
        {
            for (int frame = 0; frame < frames; ++frame)
            {
                v.device.BeginFrame();
                IRHITexture* backBuffer = swapChain.GetBackBuffer();
                EXPECT_TRUE(backBuffer != nullptr);
                IRHICommandList* cmd = v.device.GetImmediateCommandList();
                cmd->Begin();
                cmd->ClearRenderTarget(backBuffer, blue);
                cmd->End();
                v.device.EndFrame();
                EXPECT_TRUE(swapChain.Present(true));
            }
        };

        presentFrames(3);
        EXPECT_TRUE(swapChain.Resize(48, 24));
        EXPECT_EQ(swapChain.GetWidth(), 48u);
        EXPECT_EQ(swapChain.GetHeight(), 24u);
        presentFrames(3);
        EXPECT_FALSE(swapChain.Resize(0, 0)); // minimized: keep the current chain
        EXPECT_TRUE(swapChain.IsValid());
        v.device.WaitForIdle();
    }
    v.ExpectClean();
}

#endif // SPARK_VULKAN_SUPPORT
