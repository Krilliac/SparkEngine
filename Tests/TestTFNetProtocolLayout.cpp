/**
 * @file TestTFNetProtocolLayout.cpp
 * @brief TERRAFRONT wire-protocol layout freeze tests.
 *
 * TFNetProtocol.h / TFRepProtocol.h are header-only packed PODs — the frozen
 * network contract of the SparkGameMMOFPS module. These tests pin:
 *   - every message id (TFMsg 0x5400 block + rep channels 0x54F0-0x54FF),
 *   - every wire struct size (compile-time, duplicating the headers' own
 *     static_asserts so a header edit that also edits its assert still trips
 *     an independent check here),
 *   - absolute field offsets of the hot-path structs (offsetof),
 *   - a runtime memcpy round-trip of each struct through a raw byte buffer
 *     (what NetworkManager payloads actually do),
 *   - QuantPos / QuantAim quantization precision.
 *
 * CI-standalone: no sockets, no engine systems — headers only.
 */

#include "TestFramework.h"

#include "Net/TFNetProtocol.h"
#include "Net/TFRepProtocol.h"

#include <cstddef>
#include <cstring>

using namespace Terrafront;

namespace
{
    /// Serialize through a raw byte buffer and back — the exact operation
    /// TFDamageSystem/TFReplication perform on NetworkMessage payloads.
    template <typename T>
    T WireRoundTrip(const T& in)
    {
        unsigned char wire[sizeof(T)];
        std::memcpy(wire, &in, sizeof(T));
        T out{};
        std::memcpy(&out, wire, sizeof(T));
        return out;
    }

    /// Byte-exact equality (valid: packed structs with explicit _pad fields,
    /// all bytes deterministic after value-init).
    template <typename T>
    bool WireBytesEqual(const T& a, const T& b)
    {
        return std::memcmp(&a, &b, sizeof(T)) == 0;
    }
} // namespace

// ============================================================================
// Message ids — frozen numbering
// ============================================================================

static_assert(sizeof(TFMsg) == sizeof(uint16_t), "TFMsg must stay uint16_t");
static_assert(static_cast<uint16_t>(TFMsg::ClientInput)   == 0x5400);
static_assert(static_cast<uint16_t>(TFMsg::SpawnRequest)  == 0x5401);
static_assert(static_cast<uint16_t>(TFMsg::SpawnReply)    == 0x5402);
static_assert(static_cast<uint16_t>(TFMsg::FireEvent)     == 0x5403);
static_assert(static_cast<uint16_t>(TFMsg::HitConfirm)    == 0x5404);
static_assert(static_cast<uint16_t>(TFMsg::DamageEvent)   == 0x5405);
static_assert(static_cast<uint16_t>(TFMsg::KillEvent)     == 0x5406);
static_assert(static_cast<uint16_t>(TFMsg::RegionState)   == 0x5407);
static_assert(static_cast<uint16_t>(TFMsg::CaptureTick)   == 0x5408);
static_assert(static_cast<uint16_t>(TFMsg::VehicleEnter)  == 0x5409);
static_assert(static_cast<uint16_t>(TFMsg::VehicleExit)   == 0x540A);
static_assert(static_cast<uint16_t>(TFMsg::AegisDeploy)   == 0x540B);
static_assert(static_cast<uint16_t>(TFMsg::SquadMsg)      == 0x540C);
static_assert(static_cast<uint16_t>(TFMsg::ChatMsg)       == 0x540D);
static_assert(static_cast<uint16_t>(TFMsg::XPEvent)       == 0x540E);
static_assert(static_cast<uint16_t>(TFMsg::LoadoutChange) == 0x540F);
static_assert(static_cast<uint16_t>(TFMsg::FactionSelect) == 0x5410);
static_assert(static_cast<uint16_t>(TFMsg::WorldWelcome)  == 0x5411);

static_assert(kTFRepMsg_Create        == 0x54F0);
static_assert(kTFRepMsg_Update        == 0x54F1);
static_assert(kTFRepMsg_Destroy       == 0x54F2);
static_assert(kTFRepMsg_MoveState     == 0x54F3);
static_assert(kTFRepMsg_VehCreate     == 0x54F8);
static_assert(kTFRepMsg_VehUpdate     == 0x54F9);
static_assert(kTFRepMsg_VehDestroy    == 0x54FA);
static_assert(kTFRepMsg_VehSeats      == 0x54FB);
static_assert(kTFRepMsg_DeployCreate  == 0x54FC);
static_assert(kTFRepMsg_DeployUpdate  == 0x54FD);
static_assert(kTFRepMsg_DeployDestroy == 0x54FE);
static_assert(kTFVehMsg_Purchase      == 0x54FF);

// Input button bits (client prediction replays depend on these).
static_assert(TFB_Fire == 1 && TFB_AltFire == 2 && TFB_Jump == 4 && TFB_Crouch == 8);
static_assert(TFB_Sprint == 16 && TFB_Reload == 32 && TFB_Interact == 64);
static_assert(TFB_Ability == 128 && TFB_Melee == 256);

// ============================================================================
// Struct sizes — frozen wire layout (independent duplicate of header asserts)
// ============================================================================

static_assert(sizeof(TF_ClientInput)     == 20);
static_assert(sizeof(TF_SpawnRequest)    == 8);
static_assert(sizeof(TF_SpawnReply)      == 24);
static_assert(sizeof(TF_FireEvent)       == 32);
static_assert(sizeof(TF_HitConfirm)      == 8);
static_assert(sizeof(TF_DamageEvent)     == 8);
static_assert(sizeof(TF_KillEvent)       == 16);
static_assert(sizeof(TF_RegionState)     == 12);
static_assert(sizeof(TF_CaptureTick)     == 8);
static_assert(sizeof(TF_VehicleSeatOp)   == 8);
static_assert(sizeof(TF_AegisDeploy)     == 8);
static_assert(sizeof(TF_SquadMsg)        == 20);
static_assert(sizeof(TF_ChatMsg)         == 128);
static_assert(sizeof(TF_XPEvent)         == 12);
static_assert(sizeof(TF_LoadoutChange)   == 8);
static_assert(sizeof(TF_FactionSelect)   == 4);
static_assert(sizeof(TF_WorldWelcome)    == 16);

static_assert(sizeof(QuantPos)           == 12);
static_assert(sizeof(QuantAim)           == 4);
static_assert(sizeof(TF_RepPawnCreate)   == 40);
static_assert(sizeof(TF_RepDestroy)      == 4);
static_assert(sizeof(TF_RepUpdateHeader) == 6);
static_assert(sizeof(TF_RepPawnUpdate)   == 26);
static_assert(sizeof(TF_MoveState)       == 40);
static_assert(sizeof(TF_RepVehicleCreate) == 32);
static_assert(sizeof(TF_RepVehicleUpdate) == 26);
static_assert(sizeof(TF_RepVehicleSeats)  == 40);
static_assert(sizeof(TF_VehPurchase)      == 4);
static_assert(sizeof(TF_RepDeployCreate)  == 40);
static_assert(sizeof(TF_RepDeployUpdate)  == 12);
static_assert(sizeof(TF_RepDeployDestroy) == 4);

// ============================================================================
// Absolute field offsets — hot-path structs (packed => no hidden padding)
// ============================================================================

static_assert(offsetof(TF_ClientInput, seq)        == 0);
static_assert(offsetof(TF_ClientInput, buttons)    == 4);
static_assert(offsetof(TF_ClientInput, moveX)      == 6);
static_assert(offsetof(TF_ClientInput, moveY)      == 7);
static_assert(offsetof(TF_ClientInput, viewYaw)    == 8);
static_assert(offsetof(TF_ClientInput, viewPitch)  == 12);
static_assert(offsetof(TF_ClientInput, weaponSlot) == 16);

static_assert(offsetof(TF_FireEvent, seq)      == 0);
static_assert(offsetof(TF_FireEvent, weaponId) == 4);
static_assert(offsetof(TF_FireEvent, originX)  == 8);
static_assert(offsetof(TF_FireEvent, dirX)     == 20);

static_assert(offsetof(TF_RepPawnUpdate, entityId) == 0);
static_assert(offsetof(TF_RepPawnUpdate, pos)      == 4);
static_assert(offsetof(TF_RepPawnUpdate, aim)      == 16);
static_assert(offsetof(TF_RepPawnUpdate, health)   == 20);
static_assert(offsetof(TF_RepPawnUpdate, shield)   == 22);
static_assert(offsetof(TF_RepPawnUpdate, alive)    == 24);

static_assert(offsetof(TF_MoveState, lastAckedSeq) == 0);
static_assert(offsetof(TF_MoveState, posX)         == 4);
static_assert(offsetof(TF_MoveState, velX)         == 16);
static_assert(offsetof(TF_MoveState, yaw)          == 28);
static_assert(offsetof(TF_MoveState, grounded)     == 36);

static_assert(offsetof(TF_ChatMsg, fromPlayer) == 0);
static_assert(offsetof(TF_ChatMsg, channel)    == 4);
static_assert(offsetof(TF_ChatMsg, text)       == 6);

static_assert(offsetof(TF_RepVehicleSeats, seats)     == 4);
static_assert(offsetof(TF_RepVehicleSeats, seatCount) == 36);

// ============================================================================
// Runtime round-trips (memcpy through a byte buffer, field-level compare)
// ============================================================================

TEST(TFNetProtocol_ClientInput_RoundTrip)
{
    TF_ClientInput in{};
    in.seq = 0xA1B2C3D4u;
    in.buttons = TFB_Fire | TFB_Sprint | TFB_Melee;
    in.moveX = -127;
    in.moveY = 88;
    in.viewYaw = 2.5f;
    in.viewPitch = -0.75f;
    in.weaponSlot = 2;

    const TF_ClientInput out = WireRoundTrip(in);
    EXPECT_EQ(out.seq, 0xA1B2C3D4u);
    EXPECT_EQ(out.buttons, static_cast<uint16_t>(TFB_Fire | TFB_Sprint | TFB_Melee));
    EXPECT_EQ(static_cast<int>(out.moveX), -127);
    EXPECT_EQ(static_cast<int>(out.moveY), 88);
    EXPECT_NEAR(out.viewYaw, 2.5f, 0.0f);
    EXPECT_NEAR(out.viewPitch, -0.75f, 0.0f);
    EXPECT_EQ(out.weaponSlot, static_cast<uint16_t>(2));
    EXPECT_TRUE(WireBytesEqual(in, out));
}

TEST(TFNetProtocol_SpawnFlow_RoundTrip)
{
    TF_SpawnRequest req{};
    req.classId = static_cast<uint8_t>(ClassId::Striker);
    req.spawnKind = 2; // aegis
    req.regionId = 9;
    req.aegisEntity = 4242;
    const TF_SpawnRequest req2 = WireRoundTrip(req);
    EXPECT_TRUE(WireBytesEqual(req, req2));
    EXPECT_EQ(req2.aegisEntity, 4242u);

    TF_SpawnReply rep{};
    rep.accepted = 1;
    rep.entityId = 77;
    rep.posX = 2048.0f;
    rep.posY = 64.5f;
    rep.posZ = 3600.0f;
    rep.respawnDelay = 8.0f;
    const TF_SpawnReply rep2 = WireRoundTrip(rep);
    EXPECT_TRUE(WireBytesEqual(rep, rep2));
    EXPECT_NEAR(rep2.posZ, 3600.0f, 0.0f);
}

TEST(TFNetProtocol_CombatMessages_RoundTrip)
{
    TF_FireEvent fe{};
    fe.seq = 1001;
    fe.weaponId = 21;
    fe.originX = 1.0f; fe.originY = 2.0f; fe.originZ = 3.0f;
    fe.dirX = 0.0f; fe.dirY = 0.0f; fe.dirZ = 1.0f;
    EXPECT_TRUE(WireBytesEqual(fe, WireRoundTrip(fe)));

    TF_HitConfirm hc{};
    hc.victimEntity = 55;
    hc.damage = 224;
    hc.headshot = 1;
    hc.killed = 0;
    const TF_HitConfirm hc2 = WireRoundTrip(hc);
    EXPECT_EQ(hc2.damage, static_cast<uint16_t>(224));
    EXPECT_EQ(static_cast<int>(hc2.headshot), 1);

    TF_DamageEvent de{};
    de.attackerEntity = 12;
    de.damage = 103;
    de.damageKind = 1;
    de.dirOctant = 7;
    EXPECT_TRUE(WireBytesEqual(de, WireRoundTrip(de)));

    TF_KillEvent ke{};
    ke.killerPlayer = 3;
    ke.victimPlayer = 8;
    ke.weaponId = 11;
    ke.killerFaction = static_cast<uint8_t>(FactionId::AUC);
    ke.victimFaction = static_cast<uint8_t>(FactionId::HLX);
    ke.headshot = 1;
    const TF_KillEvent ke2 = WireRoundTrip(ke);
    EXPECT_TRUE(WireBytesEqual(ke, ke2));
    EXPECT_EQ(static_cast<int>(ke2.victimFaction), static_cast<int>(FactionId::HLX));
}

TEST(TFNetProtocol_TerritoryMessages_RoundTrip)
{
    TF_RegionState rs{};
    rs.regionId = 9;
    rs.owner = static_cast<uint8_t>(FactionId::MRA);
    rs.contested = 1;
    rs.captureProgress = 0.62f;
    rs.capturingFaction = static_cast<uint8_t>(FactionId::HLX);
    rs.pointOwners[0] = 1; rs.pointOwners[1] = 3; rs.pointOwners[2] = 0;
    const TF_RegionState rs2 = WireRoundTrip(rs);
    EXPECT_TRUE(WireBytesEqual(rs, rs2));
    EXPECT_EQ(static_cast<int>(rs2.pointOwners[1]), 3);

    TF_CaptureTick ct{};
    ct.regionId = 10;
    ct.capturingFaction = static_cast<uint8_t>(FactionId::AUC);
    ct.contested = 0;
    ct.progress = 0.25f;
    EXPECT_TRUE(WireBytesEqual(ct, WireRoundTrip(ct)));

    TF_WorldWelcome ww{};
    ww.yourPlayerId = 6;
    ww.yourFaction = static_cast<uint8_t>(FactionId::MRA);
    ww.regionCount = 13;
    ww.territoryHash = 0xDEADBEEFu;
    ww.serverTimeMs = 123456u;
    const TF_WorldWelcome ww2 = WireRoundTrip(ww);
    EXPECT_EQ(ww2.territoryHash, 0xDEADBEEFu);
    EXPECT_EQ(static_cast<int>(ww2.regionCount), 13);
}

TEST(TFNetProtocol_ChatAndSquad_RoundTrip)
{
    TF_ChatMsg cm{};
    cm.fromPlayer = 4;
    cm.channel = static_cast<uint8_t>(ChatChannel::Squad);
    std::strncpy(cm.text, "push fluxwell, aegis up at south ridge", sizeof(cm.text) - 1);
    const TF_ChatMsg cm2 = WireRoundTrip(cm);
    EXPECT_TRUE(WireBytesEqual(cm, cm2));
    EXPECT_EQ(std::string(cm2.text), std::string("push fluxwell, aegis up at south ridge"));

    TF_SquadMsg sm{};
    sm.op = static_cast<uint8_t>(SquadOp::SetWaypoint);
    sm.squadId = 2;
    sm.targetPlayer = kInvalidPlayer;
    sm.wpX = 2048.0f; sm.wpY = 60.0f; sm.wpZ = 900.0f;
    EXPECT_TRUE(WireBytesEqual(sm, WireRoundTrip(sm)));
}

TEST(TFRepProtocol_PawnChannel_RoundTrip)
{
    TF_RepPawnCreate pc{};
    pc.entityId = 501;
    pc.ownerPlayer = 3;
    pc.faction = static_cast<uint8_t>(FactionId::HLX);
    pc.classId = static_cast<uint8_t>(ClassId::Bulwark);
    pc.alive = 1;
    pc.posX = 3536.0f; pc.posY = 62.0f; pc.posZ = 620.0f;
    pc.yaw = -1.5f; pc.pitch = 0.1f;
    pc.health = 550.0f; pc.shield = 500.0f;
    EXPECT_TRUE(WireBytesEqual(pc, WireRoundTrip(pc)));

    TF_RepPawnUpdate pu{};
    pu.entityId = 501;
    const float pos[3] = {123.456f, 64.01f, -78.99f};
    pu.pos = QuantPos::From(pos);
    pu.aim = QuantAim::From(1.25f, -0.5f);
    pu.health = 431;
    pu.shield = 12;
    pu.alive = 1;
    const TF_RepPawnUpdate pu2 = WireRoundTrip(pu);
    EXPECT_TRUE(WireBytesEqual(pu, pu2));
    EXPECT_TRUE(pu2.pos == pu.pos);
    EXPECT_TRUE(pu2.aim == pu.aim);

    TF_MoveState ms{};
    ms.lastAckedSeq = 999;
    ms.posX = 1.0f; ms.posY = 2.0f; ms.posZ = 3.0f;
    ms.velX = -4.0f; ms.velY = 0.0f; ms.velZ = 7.5f;
    ms.yaw = 3.0f; ms.pitch = -1.0f;
    ms.grounded = 1;
    EXPECT_TRUE(WireBytesEqual(ms, WireRoundTrip(ms)));

    TF_RepUpdateHeader hdr{};
    hdr.serverTime = 128.5f;
    hdr.entityCount = 40;
    const TF_RepUpdateHeader hdr2 = WireRoundTrip(hdr);
    EXPECT_EQ(hdr2.entityCount, static_cast<uint16_t>(40));
}

TEST(TFRepProtocol_VehicleAndDeployChannels_RoundTrip)
{
    TF_RepVehicleCreate vc{};
    vc.entityId = 900;
    vc.vehId = static_cast<uint8_t>(VehicleId::Aegis);
    vc.faction = static_cast<uint8_t>(FactionId::AUC);
    vc.deployed = 1;
    vc.posX = 560.0f; vc.posY = 60.0f; vc.posZ = 620.0f;
    vc.yaw = 0.5f;
    vc.health = 5200.0f; vc.maxHealth = 5200.0f;
    EXPECT_TRUE(WireBytesEqual(vc, WireRoundTrip(vc)));

    TF_RepVehicleUpdate vu{};
    vu.entityId = 900;
    const float vpos[3] = {561.25f, 60.5f, 622.75f};
    vu.pos = QuantPos::From(vpos);
    vu.yaw10k = 5000;
    vu.pitch10k = -300;
    vu.roll10k = 150;
    vu.health = 4980;
    vu.deployed = 1;
    EXPECT_TRUE(WireBytesEqual(vu, WireRoundTrip(vu)));

    TF_RepVehicleSeats vs{};
    vs.entityId = 900;
    for (uint32_t i = 0; i < 8; ++i)
        vs.seats[i] = kInvalidPlayer;
    vs.seats[0] = 3;
    vs.seats[1] = 7;
    vs.seatCount = 8;
    vs.deployed = 0;
    const TF_RepVehicleSeats vs2 = WireRoundTrip(vs);
    EXPECT_TRUE(WireBytesEqual(vs, vs2));
    EXPECT_EQ(vs2.seats[2], kInvalidPlayer);

    TF_VehPurchase vp{};
    vp.vehId = static_cast<uint8_t>(VehicleId::Ravager);
    EXPECT_TRUE(WireBytesEqual(vp, WireRoundTrip(vp)));

    TF_RepDeployCreate dc{};
    dc.entityId = 1200;
    dc.ownerPlayer = 5;
    dc.kind = static_cast<uint8_t>(DeployableKind::FabTurret);
    dc.faction = static_cast<uint8_t>(FactionId::MRA);
    dc.posX = 1450.0f; dc.posY = 61.0f; dc.posZ = 3150.0f;
    dc.yaw = 1.0f;
    dc.health = 800.0f; dc.maxHealth = 800.0f;
    dc.lifeSec = 120.0f;
    EXPECT_TRUE(WireBytesEqual(dc, WireRoundTrip(dc)));

    TF_RepDeployUpdate du{};
    du.entityId = 1200;
    du.health = 300.0f;
    du.lifeSec = 45.5f;
    EXPECT_TRUE(WireBytesEqual(du, WireRoundTrip(du)));

    TF_RepDestroy rd{};
    rd.entityId = 501;
    EXPECT_EQ(WireRoundTrip(rd).entityId, 501u);
    TF_RepDeployDestroy dd{};
    dd.entityId = 1200;
    EXPECT_EQ(WireRoundTrip(dd).entityId, 1200u);
}

// ============================================================================
// Quantization precision (replication value types)
// ============================================================================

TEST(TFRepProtocol_QuantPos_CentimeterPrecision)
{
    const float in[3] = {2048.123f, -64.999f, 3600.005f};
    float out[3] = {};
    QuantPos::From(in).To(out);
    // cm quantization: worst-case error 0.5 cm per axis, plus float
    // representation error at kilometre magnitudes (granularity at 3600 m is
    // ~0.24 mm) — tolerance must sit strictly above the theoretical bound.
    EXPECT_NEAR(out[0], in[0], 0.0055f);
    EXPECT_NEAR(out[1], in[1], 0.0055f);
    EXPECT_NEAR(out[2], in[2], 0.0055f);

    // Exact-cm values are lossless — the dirty-check identity the server
    // relies on (quantize twice == quantize once).
    const QuantPos q = QuantPos::From(in);
    float mid[3];
    q.To(mid);
    EXPECT_TRUE(QuantPos::From(mid) == q);
}

TEST(TFRepProtocol_QuantAim_TenThousandthRadPrecision)
{
    const QuantAim a = QuantAim::From(3.0f, -1.4f);
    EXPECT_NEAR(a.Yaw(), 3.0f, 0.0001f);
    EXPECT_NEAR(a.Pitch(), -1.4f, 0.0001f);

    // Wrap: 2*pi + 0.5 must decode near 0.5, not overflow the int16.
    const QuantAim b = QuantAim::From(6.78318531f, 0.0f);
    EXPECT_NEAR(b.Yaw(), 0.5f, 0.001f);
}
