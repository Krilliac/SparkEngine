/**
 * @file LightingSystemInternalWindowsTypes.cpp
 * @brief Windows light/shadow type string conversions for LightingSystem
 *
 * LightTypeToString / StringToLightType / ShadowTechniqueToString /
 * StringToShadowTechnique split out of LightingSystemInternalWindows.cpp
 * (which keeps constant buffer creation, shadow map management, and
 * per-frame buffer updates). The Linux counterparts live in
 * LightingSystemLinuxTypes.cpp.
 */
#include "Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS

#include "LightingSystem.h"
#include "../Utils/Hash.h"

#include <string>

// ============================================================================
// UTILITY FUNCTIONS
// ============================================================================

std::string LightTypeToString(LightType type)
{
    switch (type)
    {
    case LightType::Directional:
        return "directional";
    case LightType::Point:
        return "point";
    case LightType::Spot:
        return "spot";
    case LightType::Area:
        return "area";
    case LightType::Environment:
        return "environment";
    default:
        return "unknown";
    }
}

LightType StringToLightType(const std::string& str)
{
    using namespace Spark::HashLiterals;
    switch (Spark::FNV1a64(str))
    {
    case "directional"_hash64:
        return LightType::Directional;
    case "point"_hash64:
        return LightType::Point;
    case "spot"_hash64:
        return LightType::Spot;
    case "area"_hash64:
        return LightType::Area;
    case "environment"_hash64:
        return LightType::Environment;
    default:
        return LightType::Directional;
    }
}

std::string ShadowTechniqueToString(ShadowTechnique technique)
{
    switch (technique)
    {
    case ShadowTechnique::None:
        return "none";
    case ShadowTechnique::Basic:
        return "basic";
    case ShadowTechnique::PCF:
        return "pcf";
    case ShadowTechnique::VSM:
        return "vsm";
    case ShadowTechnique::CSM:
        return "csm";
    case ShadowTechnique::PCSS:
        return "pcss";
    default:
        return "unknown";
    }
}

ShadowTechnique StringToShadowTechnique(const std::string& str)
{
    using namespace Spark::HashLiterals;
    switch (Spark::FNV1a64(str))
    {
    case "none"_hash64:
        return ShadowTechnique::None;
    case "basic"_hash64:
        return ShadowTechnique::Basic;
    case "pcf"_hash64:
        return ShadowTechnique::PCF;
    case "vsm"_hash64:
        return ShadowTechnique::VSM;
    case "csm"_hash64:
        return ShadowTechnique::CSM;
    case "pcss"_hash64:
        return ShadowTechnique::PCSS;
    default:
        return ShadowTechnique::PCF;
    }
}

#endif // SPARK_PLATFORM_WINDOWS
