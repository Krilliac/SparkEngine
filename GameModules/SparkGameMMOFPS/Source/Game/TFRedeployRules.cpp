/**
 * @file TFRedeployRules.cpp
 * @brief Shared client/server redeploy eligibility (see TFRedeployRules.h).
 */
#include "Game/TFRedeployRules.h"

#include "Data/TFDataTables.h"
#include "Game/TFPlayerSystem.h"
#include "Game/TFVehicleSystem.h"
#include "Net/TFRedeployProtocol.h"
#include "World/TFRegionSystem.h"
#include "World/TFWorldSetup.h"

#include <cmath>

namespace Terrafront::TFRedeployRules
{

    namespace
    {
        ExtraRule s_extraRule; ///< continents-lane hook (sanctuary rule); empty == allow
    }

    void SetExtraRule(ExtraRule rule)
    {
        s_extraRule = std::move(rule);
    }

    uint8_t CanRedeploy(const TFGameContext& ctx, PlayerId player, FactionId faction, RegionId region)
    {
        if (faction == FactionId::None || region == kInvalidRegion)
            return kTFRedeployBadRegion;

        PawnInfo pawn{};
        if (!ctx.players || !ctx.players->GetPawnByPlayer(player, pawn) || !pawn.alive)
            return kTFRedeployNotAlive;

        if (ctx.vehicles && ctx.vehicles->IsSeated(player))
            return kTFRedeployInVehicle;

        // Friendly + spawnable: owned by `faction` AND conduit-linked to the
        // faction skyanchor (the exact spawn-screen rule, so redeploy can never
        // reach a region a fresh spawn could not).
        if (!ctx.regions || !ctx.regions->CanSpawnAt(region, faction))
            return kTFRedeployBadRegion;

        FactionId capturing = FactionId::None;
        bool contested = false;
        ctx.regions->CaptureProgress(region, capturing, contested);
        if (contested)
            return kTFRedeployContested;

        if (s_extraRule)
        {
            if (const uint8_t r = s_extraRule(ctx, player, region))
                return r;
        }
        return kTFRedeployOk;
    }

    bool ResolveSpawnPos(const TFGameContext& ctx, RegionId region, FactionId faction, float outPos[3], float& outYaw)
    {
        if (!ctx.data || !ctx.data->IsLoaded() || !ctx.regions)
            return false;
        if (!ctx.regions->CanSpawnAt(region, faction))
            return false;
        const RegionDef* r = ctx.data->GetRegion(region);
        if (!r)
            return false;
        const float x = r->spawns.empty() ? r->centerX : r->spawns.front()[0];
        const float z = r->spawns.empty() ? r->centerZ : r->spawns.front()[1];
        outPos[0] = x;
        outPos[1] = (ctx.world ? ctx.world->TerrainHeightAt(x, z) : 0.0f) + 0.1f;
        outPos[2] = z;
        outYaw = std::atan2(2048.0f - x, 2048.0f - z); // face map center (FindRegionSpawn convention)
        return true;
    }

    const char* ReasonText(uint8_t reason)
    {
        switch (reason)
        {
        case kTFRedeployOk:
            return "OK";
        case kTFRedeployBadRegion:
            return "Region is not friendly / not linked";
        case kTFRedeployCooldown:
            return "Redeploy on cooldown";
        case kTFRedeployContested:
            return "Region is contested";
        case kTFRedeployInVehicle:
            return "Exit your vehicle first";
        case kTFRedeployNotAlive:
            return "You are down - use DEPLOY instead";
        case kTFRedeployBlocked:
            return "Redeploy not allowed here";
        default:
            return "Redeploy refused";
        }
    }

} // namespace Terrafront::TFRedeployRules
