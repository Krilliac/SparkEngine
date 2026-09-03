/**
 * @file TestCoverageAI.cpp
 * @brief Deterministic real-class coverage for production AI subsystems.
 *
 * Several older tests intentionally use small stand-alone models.  These tests
 * exercise the production implementations and their error/edge paths instead.
 */

#include "TestFramework.h"

#include "Engine/AI/CoverSystem.h"
#include "Engine/AI/FormationSystem.h"
#include "Engine/AI/TacticalPointSystem.h"

#include <cstdint>
#include <vector>

namespace
{
    using namespace Spark::AI;

    struct FormationCoverageFixture
    {
        void SetUp()
        {
            auto& formations = FormationSystem::GetInstance();
            formations.Shutdown();
            formations.Initialize();
        }

        void TearDown() { FormationSystem::GetInstance().Shutdown(); }
    };

    struct TacticalCoverageFixture
    {
        void SetUp()
        {
            auto& points = TacticalPointSystem::GetInstance();
            points.Shutdown();
            points.Initialize();
        }

        void TearDown() { TacticalPointSystem::GetInstance().Shutdown(); }
    };

    struct CoverCoverageFixture
    {
        void SetUp()
        {
            auto& cover = CoverSystem::GetInstance();
            cover.Shutdown();
            cover.Initialize();
        }

        void TearDown() { CoverSystem::GetInstance().Shutdown(); }
    };

    TacticalPoint MakeTacticalPoint(float x, float z, TacticalPointType type, float quality,
                                    XMFLOAT3 normal = {0.0f, 0.0f, 1.0f})
    {
        TacticalPoint point;
        point.position = {x, 0.0f, z};
        point.type = type;
        point.quality = quality;
        point.normal = normal;
        return point;
    }

    CoverPoint MakeCoverPoint(float x, float z, XMFLOAT3 normal)
    {
        CoverPoint point;
        point.position = {x, 0.0f, z};
        point.normal = normal;
        return point;
    }
} // namespace

TEST(FormationCoverage_RequiresInitializationAndMembers)
{
    auto& formations = FormationSystem::GetInstance();
    formations.Shutdown();

    EXPECT_EQ(formations.CreateFormation(FormationType::Line, 1, 2), static_cast<uint32_t>(0));

    formations.Initialize();
    EXPECT_EQ(formations.CreateFormation(FormationType::Line, 1, 0), static_cast<uint32_t>(0));
    EXPECT_EQ(formations.GetFormationCount(), static_cast<size_t>(0));
    formations.Shutdown();
}

TEST_F(FormationCoverageFixture, GeneratesEveryFormationShape)
{
    auto& formations = FormationSystem::GetInstance();
    constexpr float spacing = 4.0f;

    const uint32_t line = formations.CreateFormation(FormationType::Line, 10, 3, spacing);
    const uint32_t wedge = formations.CreateFormation(FormationType::Wedge, 11, 4, spacing);
    const uint32_t column = formations.CreateFormation(FormationType::Column, 12, 3, spacing);
    const uint32_t circle = formations.CreateFormation(FormationType::Circle, 13, 4, spacing);
    const uint32_t spread = formations.CreateFormation(FormationType::Spread, 14, 5, spacing);

    EXPECT_EQ(formations.GetFormationCount(), static_cast<size_t>(5));

    const auto* lineData = formations.GetFormation(line);
    ASSERT_TRUE(lineData != nullptr);
    EXPECT_NEAR(lineData->slots[0].localOffset.x, -4.0f, 0.001f);
    EXPECT_NEAR(lineData->slots[1].localOffset.x, 0.0f, 0.001f);
    EXPECT_NEAR(lineData->slots[2].localOffset.x, 4.0f, 0.001f);

    const auto* wedgeData = formations.GetFormation(wedge);
    EXPECT_NEAR(wedgeData->slots[0].localOffset.x, -2.0f, 0.001f);
    EXPECT_NEAR(wedgeData->slots[1].localOffset.x, 2.0f, 0.001f);
    EXPECT_NEAR(wedgeData->slots[2].localOffset.z, -8.0f, 0.001f);

    const auto* columnData = formations.GetFormation(column);
    EXPECT_NEAR(columnData->slots[0].localOffset.z, -4.0f, 0.001f);
    EXPECT_NEAR(columnData->slots[2].localOffset.z, -12.0f, 0.001f);

    const auto* circleData = formations.GetFormation(circle);
    EXPECT_NEAR(circleData->slots[0].localOffset.x, 4.0f, 0.001f);
    EXPECT_NEAR(circleData->slots[1].localOffset.z, 4.0f, 0.001f);

    const auto* spreadData = formations.GetFormation(spread);
    EXPECT_EQ(spreadData->slots.size(), static_cast<size_t>(5));
    EXPECT_NEAR(spreadData->slots[3].localOffset.x, -2.0f, 0.001f);
    EXPECT_NEAR(spreadData->slots[3].localOffset.z, -4.0f, 0.001f);
}

TEST_F(FormationCoverageFixture, AssignmentRemovalQueriesAndDisbandAreSafe)
{
    auto& formations = FormationSystem::GetInstance();
    const uint32_t id = formations.CreateFormation(FormationType::Column, 77, 2, 2.0f);

    EXPECT_EQ(formations.AssignToSlot(9999, 1), -1);
    EXPECT_EQ(formations.AssignToSlot(id, 101), 0);
    EXPECT_EQ(formations.AssignToSlot(id, 102), 1);
    EXPECT_EQ(formations.AssignToSlot(id, 103), -1);

    formations.RemoveFromFormation(9999, 101);
    formations.RemoveFromFormation(id, 9999);
    formations.RemoveFromFormation(id, 101);
    EXPECT_EQ(formations.AssignToSlot(id, 103), 0);

    const auto invalidID = formations.GetSlotWorldPosition(9999, 0);
    const auto invalidLow = formations.GetSlotWorldPosition(id, -1);
    const auto invalidHigh = formations.GetSlotWorldPosition(id, 7);
    EXPECT_NEAR(invalidID.x, 0.0f, 0.001f);
    EXPECT_NEAR(invalidLow.z, 0.0f, 0.001f);
    EXPECT_NEAR(invalidHigh.y, 0.0f, 0.001f);

    const auto position = formations.GetSlotWorldPosition(id, 1);
    EXPECT_NEAR(position.x, 0.0f, 0.001f);
    EXPECT_NEAR(position.z, -4.0f, 0.001f);
    EXPECT_TRUE(formations.GetFormation(9999) == nullptr);

    EXPECT_NO_THROW(formations.Update(0.016f));
    formations.DisbandFormation(id);
    formations.DisbandFormation(id);
    EXPECT_EQ(formations.GetFormationCount(), static_cast<size_t>(0));
}

TEST_F(TacticalCoverageFixture, ScoresSortsAndLimitsProductionPoints)
{
    auto& points = TacticalPointSystem::GetInstance();
    const uint32_t nearCover =
        points.RegisterPoint(MakeTacticalPoint(2.0f, 0.0f, TacticalPointType::Cover, 0.8f, {-1.0f, 0.0f, 0.0f}));
    const uint32_t farCover =
        points.RegisterPoint(MakeTacticalPoint(8.0f, 0.0f, TacticalPointType::Cover, 1.0f, {-1.0f, 0.0f, 0.0f}));
    points.RegisterPoint(MakeTacticalPoint(1.0f, 1.0f, TacticalPointType::Flank, 1.0f));

    TacticalQuery query;
    query.queryOrigin = {0.0f, 0.0f, 0.0f};
    query.threatPosition = {20.0f, 0.0f, 0.0f};
    query.searchRadius = 10.0f;
    query.preferredType = TacticalPointType::Cover;
    query.minQuality = 0.0f;

    const auto result = points.FindPoints(query, 2);
    EXPECT_EQ(result.size(), static_cast<size_t>(2));
    EXPECT_EQ(result[0]->id, nearCover);
    EXPECT_EQ(result[1]->id, farCover);
    EXPECT_EQ(points.FindBestPoint(query)->id, nearCover);
    EXPECT_TRUE(points.FindPoints(query, 0).empty());
}

TEST_F(TacticalCoverageFixture, AppliesDistanceQualityOccupancyAndRemovalFilters)
{
    auto& points = TacticalPointSystem::GetInstance();
    const uint32_t accepted = points.RegisterPoint(MakeTacticalPoint(-2.0f, -2.0f, TacticalPointType::Rally, 0.7f));
    const uint32_t lowQuality = points.RegisterPoint(MakeTacticalPoint(-1.0f, -1.0f, TacticalPointType::Rally, 0.2f));
    points.RegisterPoint(MakeTacticalPoint(-30.0f, -30.0f, TacticalPointType::Rally, 1.0f));

    TacticalQuery query;
    query.queryOrigin = {0.0f, 0.0f, 0.0f};
    query.searchRadius = 8.0f;
    query.preferredType = TacticalPointType::Rally;
    query.minQuality = 0.5f;

    EXPECT_EQ(points.FindPoints(query, 10).size(), static_cast<size_t>(1));
    points.MarkOccupied(accepted, 42);
    EXPECT_TRUE(points.FindBestPoint(query) == nullptr);

    query.requireUnoccupied = false;
    EXPECT_EQ(points.FindBestPoint(query)->id, accepted);
    points.MarkFree(accepted);
    points.MarkOccupied(9999, 1);
    points.MarkFree(9999);

    points.UpdatePointQuality(lowQuality, 2.0f);
    query.requireUnoccupied = true;
    query.minQuality = 0.99f;
    EXPECT_EQ(points.FindBestPoint(query)->id, lowQuality);
    points.UpdatePointQuality(lowQuality, -1.0f);
    EXPECT_EQ(points.FindPoints(query, 10).size(), static_cast<size_t>(0));
    points.UpdatePointQuality(9999, 0.5f);

    points.RemovePoint(9999);
    points.RemovePoint(accepted);
    EXPECT_EQ(points.GetPointCount(), static_cast<uint32_t>(2));
}

TEST_F(TacticalCoverageFixture, ZeroRadiusAndCoincidentThreatRemainDeterministic)
{
    auto& points = TacticalPointSystem::GetInstance();
    points.RegisterPoint(MakeTacticalPoint(0.0f, 0.0f, TacticalPointType::Cover, 0.75f));

    TacticalQuery query;
    query.queryOrigin = {0.0f, 0.0f, 0.0f};
    query.threatPosition = query.queryOrigin;
    query.searchRadius = 0.0f;
    query.minQuality = 0.0f;

    const auto* best = points.FindBestPoint(query);
    ASSERT_TRUE(best != nullptr);
    EXPECT_NEAR(best->quality, 0.75f, 0.001f);
}

TEST_F(CoverCoverageFixture, GeometryAnalysisHandlesInvalidAndAllHeightClasses)
{
    auto& cover = CoverSystem::GetInstance();
    cover.AnalyzeStaticGeometry({{0.0f, 0.0f, 0.0f}}, {});
    EXPECT_EQ(cover.GetCoverCount(), static_cast<uint32_t>(0));

    const std::vector<XMFLOAT3> positions = {
        {0.0f, 0.5f, 0.0f}, {10.0f, 1.5f, 0.0f}, {20.0f, 2.0f, 0.0f}, {30.0f, 0.0f, 0.0f}, {40.0f, 0.0f, 0.0f}};
    const std::vector<XMFLOAT3> normals = {
        {-1.0f, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}, {1.0f, 0.5f, 0.0f}, {0.1f, 0.0f, 0.1f}};
    cover.AnalyzeStaticGeometry(positions, normals);
    EXPECT_EQ(cover.GetCoverCount(), static_cast<uint32_t>(3));

    const auto* low = cover.FindNearestCover({0.0f, 0.5f, 0.0f}, {1.0f, 0.0f, 0.0f}, 2.0f);
    const auto* high = cover.FindNearestCover({10.0f, 1.5f, 0.0f}, {1.0f, 0.0f, 0.0f}, 2.0f);
    const auto* tall = cover.FindNearestCover({20.0f, 2.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, 2.0f);
    ASSERT_TRUE(low != nullptr);
    EXPECT_EQ(static_cast<int>(low->height), static_cast<int>(CoverHeight::Low));
    EXPECT_TRUE(low->canFireOver);
    ASSERT_TRUE(high != nullptr);
    EXPECT_EQ(static_cast<int>(high->height), static_cast<int>(CoverHeight::High));
    EXPECT_TRUE(high->canLeanLeft);
    EXPECT_FALSE(high->canFireOver);
    ASSERT_TRUE(tall != nullptr);
    EXPECT_FALSE(tall->canLeanLeft);
    EXPECT_FALSE(tall->canLeanRight);
}

TEST_F(CoverCoverageFixture, QueriesSortFilterAndTrackOccupancy)
{
    auto& cover = CoverSystem::GetInstance();
    const uint32_t nearest = cover.RegisterCoverPoint(MakeCoverPoint(2.0f, 0.0f, {-1.0f, 0.0f, 0.0f}));
    const uint32_t farther = cover.RegisterCoverPoint(MakeCoverPoint(5.0f, 0.0f, {-1.0f, 0.0f, 0.0f}));
    const uint32_t opposite = cover.RegisterCoverPoint(MakeCoverPoint(3.0f, 0.0f, {1.0f, 0.0f, 0.0f}));
    cover.RegisterCoverPoint(MakeCoverPoint(50.0f, 0.0f, {-1.0f, 0.0f, 0.0f}));

    EXPECT_TRUE(cover.FindCoverFromThreat({0, 0, 0}, {10, 0, 0}, 10.0f, 0).empty());
    auto result = cover.FindCoverFromThreat({0, 0, 0}, {10, 0, 0}, 10.0f, 8);
    EXPECT_EQ(result.size(), static_cast<size_t>(2));
    EXPECT_EQ(result[0]->id, nearest);
    EXPECT_EQ(result[1]->id, farther);

    cover.MarkOccupied(nearest, 77);
    const auto* selected = cover.FindNearestCover({0, 0, 0}, {1, 0, 0}, 10.0f);
    ASSERT_TRUE(selected != nullptr);
    EXPECT_EQ(selected->id, farther);

    cover.MarkFree(nearest);
    cover.MarkOccupied(9999, 1);
    cover.MarkFree(9999);
    EXPECT_EQ(cover.FindNearestCover({0, 0, 0}, {1, 0, 0}, 10.0f)->id, nearest);
    EXPECT_EQ(cover.FindNearestCover({0, 0, 0}, {-1, 0, 0}, 10.0f)->id, opposite);
    EXPECT_TRUE(cover.FindNearestCover({0, 0, 0}, {1, 0, 0}, 1.0f) == nullptr);
}

TEST_F(CoverCoverageFixture, CoincidentThreatUsesStableDefaultDirection)
{
    auto& cover = CoverSystem::GetInstance();
    const uint32_t id = cover.RegisterCoverPoint(MakeCoverPoint(0.0f, 0.0f, {0.0f, 0.0f, -1.0f}));
    const auto result = cover.FindCoverFromThreat({0, 0, 0}, {0, 0, 0}, 1.0f, 1);
    EXPECT_EQ(result.size(), static_cast<size_t>(1));
    EXPECT_EQ(result[0]->id, id);
}
