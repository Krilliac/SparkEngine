/**
 * @file TestTFObservation.cpp
 * @brief TF-110: tf_observe's text form (Game/TFObservation.h) is sorted and
 *        quantized so a server and its clients can be diffed line by line,
 *        and TFAimAngles matches the BuildViewRay convention tf_aim_at relies on.
 *
 * Header-only (TestTFAbilityWire.cpp pattern). The server/client fill paths
 * are exercised by the multi-process harness, not here.
 */
#include "TestFramework.h"

#include "Game/TFObservation.h"

#include <cmath>
#include <cstddef>
#include <sstream>
#include <string>
#include <vector>

using namespace Terrafront;

namespace
{
    std::vector<std::string> Lines(const std::string& text)
    {
        std::vector<std::string> lines;
        std::istringstream stream(text);
        std::string line;
        while (std::getline(stream, line))
            lines.push_back(line);
        return lines;
    }

    /// Everything after the per-process header line.
    std::vector<std::string> Body(const std::string& text)
    {
        std::vector<std::string> lines = Lines(text);
        if (!lines.empty())
            lines.erase(lines.begin());
        return lines;
    }

    TFObservedPawn Pawn(PlayerId id, float x, float y, float z)
    {
        TFObservedPawn p;
        p.id = id;
        p.faction = 1;
        p.cls = 2;
        p.health = 100;
        p.pos[0] = x;
        p.pos[1] = y;
        p.pos[2] = z;
        return p;
    }
} // namespace

TEST(TF110_Observe_FormatIsSortedAndQuantized)
{
    TFObservation obs;
    obs.role = "server";
    obs.self = 7;
    obs.continentKey = "cindral";
    obs.pawns = {Pawn(9, 1.13f, 0.0f, -2.37f), Pawn(3, -0.05f, 10.0f, 4.0f)};
    obs.regionOwners = {{5, 2}, {1, 3}};
    TFObservedVehicle vehicle;
    vehicle.netId = 42;
    vehicle.def = 3;
    vehicle.driver = 9;
    vehicle.hp = 800;
    obs.vehicles = {vehicle};

    const std::vector<std::string> lines = Lines(FormatObservation(obs));
    ASSERT_EQ(lines.size(), std::size_t{7});
    EXPECT_EQ(
        lines[0],
        std::string("[TF-OBSERVE] role=server clock=0.000 self=7 continent=cindral pawns=2 regions=2 vehicles=1"));
    EXPECT_EQ(lines[1], std::string("[TF-OBSERVE] self id=7 loadout=default flux=0 rank=0"));
    // Sorted by id; 1.13 -> 1.25, -2.37 -> -2.25, and -0.05 -> 0.00 (never "-0.00").
    EXPECT_EQ(lines[2], std::string("[TF-OBSERVE] pawn id=3 faction=1 class=2 health=100 pos=0.00,10.00,4.00"));
    EXPECT_EQ(lines[3], std::string("[TF-OBSERVE] pawn id=9 faction=1 class=2 health=100 pos=1.25,0.00,-2.25"));
    EXPECT_EQ(lines[4], std::string("[TF-OBSERVE] region id=1 owner=3"));
    EXPECT_EQ(lines[5], std::string("[TF-OBSERVE] region id=5 owner=2"));
    EXPECT_EQ(lines[6], std::string("[TF-OBSERVE] vehicle net=42 def=3 driver=9 hp=800 pos=0.00,0.00,0.00"));
}

TEST(TF110_Observe_IdenticalViewsFormatIdentically)
{
    // The server's view and a client's interpolated view of the same world:
    // different container order, sub-quantum position noise, different role
    // and clock. The body lines must match exactly.
    TFObservation server;
    server.role = "server";
    server.clock = 12.5;
    server.self = kInvalidPlayer;
    server.pawns = {Pawn(1, 10.02f, 2.0f, 3.0f), Pawn(2, -4.0f, 2.0f, 8.51f)};
    server.regionOwners = {{0, 1}, {1, 2}};

    TFObservation client = server;
    client.role = "client";
    client.clock = 3.25;
    client.pawns = {Pawn(2, -3.93f, 2.04f, 8.55f), Pawn(1, 9.97f, 1.99f, 3.08f)};
    client.regionOwners = {{1, 2}, {0, 1}};

    const std::string serverText = FormatObservation(server);
    const std::string clientText = FormatObservation(client);
    EXPECT_TRUE(Lines(serverText).front() != Lines(clientText).front());
    EXPECT_TRUE(Body(serverText) == Body(clientText));

    // A real divergence (over half a quantum) must show up.
    client.pawns[0].pos[0] = -3.5f;
    EXPECT_FALSE(Body(serverText) == Body(FormatObservation(client)));
}

TEST(TF110_Observe_EmptyWorldEmitsHeaderOnly)
{
    TFObservation obs;
    obs.role = "client";
    const std::vector<std::string> lines = Lines(FormatObservation(obs));
    ASSERT_EQ(lines.size(), std::size_t{2});
    EXPECT_EQ(lines[0].rfind("[TF-OBSERVE] role=client ", 0), std::size_t{0});
    EXPECT_TRUE(lines[0].find("pawns=0 regions=0 vehicles=0") != std::string::npos);
    EXPECT_EQ(lines[1], std::string("[TF-OBSERVE] self id=4294967295 loadout=default flux=0 rank=0"));
}

TEST(TF110_Observe_AimAnglesMatchViewRay)
{
    const float eye[3] = {1.0f, 2.0f, 3.0f};
    const float targets[][3] = {{11.0f, 2.0f, 3.0f}, {1.0f, 7.0f, 13.0f}, {-4.0f, -1.0f, -6.0f}};
    for (const auto& target : targets)
    {
        float yaw = 0.0f;
        float pitch = 0.0f;
        TFAimAngles(eye, target, yaw, pitch);

        // BuildViewRay: dir = (cos(p) sin(y), -sin(p), cos(p) cos(y)).
        const float dir[3] = {std::cos(pitch) * std::sin(yaw), -std::sin(pitch), std::cos(pitch) * std::cos(yaw)};
        const float to[3] = {target[0] - eye[0], target[1] - eye[1], target[2] - eye[2]};
        const float len = std::sqrt(to[0] * to[0] + to[1] * to[1] + to[2] * to[2]);
        EXPECT_NEAR(dir[0], to[0] / len, 1.0e-5f);
        EXPECT_NEAR(dir[1], to[1] / len, 1.0e-5f);
        EXPECT_NEAR(dir[2], to[2] / len, 1.0e-5f);
    }
}
