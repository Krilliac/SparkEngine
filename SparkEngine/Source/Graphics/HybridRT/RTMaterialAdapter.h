/**
 * @file RTMaterialAdapter.h
 * @brief Convert engine-side PBR material properties into the flat
 *        per-instance layout consumed by the Metal RT kernels.
 *
 * The RT kernels operate on `Spark::RHI::Metal::MaterialParams` — a
 * compact 48-byte structure (albedo + emissive + roughness/metallic)
 * matched to the Metal constant-buffer layout. The rest of the engine
 * talks in `Spark::Graphics::PBRProperties` (with factors, IOR,
 * normal-scale, etc.). The two don't align automatically — factors
 * must be baked into the colour channels, and only the fields the
 * kernels actually read are forwarded.
 *
 * Keeping the converter in its own TU means:
 *   - `MaterialSystem` stays RT-agnostic.
 *   - `MetalRayTracing` stays engine-agnostic.
 *   - Callers pull both headers and call one function.
 *
 * Off macOS the return value still compiles (the struct is declared
 * only when `SPARK_PLATFORM_MACOS` is defined, so the header is
 * macOS-only). Non-macOS consumers should gate calls behind
 * `#ifdef SPARK_PLATFORM_MACOS` the same way the RT system itself does.
 */

#pragma once

#include "../../Core/Platform.h"

#ifdef SPARK_PLATFORM_MACOS

#include "../MaterialSystem.h" // PBRProperties
#include "../RHI/Metal/MetalRayTracing.h"

namespace Spark::Graphics
{
    /**
     * @brief Pack `PBRProperties` into the RT kernel's `MaterialParams`.
     *
     *   - `albedo.rgb` = `pbr.albedoColor.rgb`, alpha preserved.
     *   - `emissive.rgb` = `pbr.emissiveColor * pbr.emissiveFactor` — the
     *     factor is baked in so the kernel sees final radiance and
     *     doesn't need the factor separately.
     *   - `roughnessMetallic.x` = `pbr.roughnessFactor`, `.y` =
     *     `pbr.metallicFactor`. The remaining two floats are reserved
     *     (pad to float4 for constant-buffer alignment).
     */
    inline Spark::RHI::Metal::MaterialParams MaterialParamsFromPBR(const PBRProperties& pbr)
    {
        Spark::RHI::Metal::MaterialParams out;
        out.albedo[0] = pbr.albedoColor.x;
        out.albedo[1] = pbr.albedoColor.y;
        out.albedo[2] = pbr.albedoColor.z;
        out.albedo[3] = pbr.albedoColor.w;

        out.emissive[0] = pbr.emissiveColor.x * pbr.emissiveFactor;
        out.emissive[1] = pbr.emissiveColor.y * pbr.emissiveFactor;
        out.emissive[2] = pbr.emissiveColor.z * pbr.emissiveFactor;
        out.emissive[3] = 0.0f;

        out.roughnessMetallic[0] = pbr.roughnessFactor;
        out.roughnessMetallic[1] = pbr.metallicFactor;
        out.roughnessMetallic[2] = 0.0f;
        out.roughnessMetallic[3] = 0.0f;
        return out;
    }

} // namespace Spark::Graphics

#endif // SPARK_PLATFORM_MACOS
