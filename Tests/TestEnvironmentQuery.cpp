/**
 * @file TestEnvironmentQuery.cpp
 * @brief Tests for Spark::AI::EQS (Environment Query System)
 */

#include "TestFramework.h"
#include "Engine/AI/EnvironmentQuery.h"

using namespace Spark::AI;

TEST(EQS_GridGeneratorBasic)
{
    GridGenerator gen({0.0f, 0.0f, 0.0f}, 5.0f, 2.0f);
    auto items = gen.Generate();

    // Should generate a grid within a circle of radius 5
    EXPECT_GT(static_cast<int>(items.size()), 0);

    // All items should be within radius
    for (const auto& item : items)
    {
        float distSq = item.position.x * item.position.x + item.position.z * item.position.z;
        EXPECT_LE(distSq, 25.01f); // radius^2 + epsilon
    }
}

TEST(EQS_RingGenerator)
{
    RingGenerator gen({0.0f, 0.0f, 0.0f}, 5.0f, 10.0f, 16);
    auto items = gen.Generate();

    EXPECT_GT(static_cast<int>(items.size()), 0);

    // All items should be between inner and outer radius
    for (const auto& item : items)
    {
        float dist = std::sqrt(item.position.x * item.position.x + item.position.z * item.position.z);
        EXPECT_GE(dist, 4.9f);  // inner - epsilon
        EXPECT_LE(dist, 10.1f); // outer + epsilon
    }
}

TEST(EQS_ConeGenerator)
{
    ConeGenerator gen({0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, 10.0f, 45.0f, 16);
    auto items = gen.Generate();

    EXPECT_GT(static_cast<int>(items.size()), 0);

    // All items should be roughly in the forward X direction
    for (const auto& item : items)
    {
        EXPECT_GE(item.position.x, -0.1f); // Cone points forward in +X
    }
}

TEST(EQS_DistanceTest)
{
    DistanceTest test({0.0f, 0.0f, 0.0f}, 3.0f, 7.0f);

    EQSItem close;
    close.position = {1.0f, 0.0f, 0.0f}; // dist 1, too close
    EXPECT_FALSE(test.Test(close));

    EQSItem inRange;
    inRange.position = {5.0f, 0.0f, 0.0f}; // dist 5, in range
    EXPECT_TRUE(test.Test(inRange));

    EQSItem tooFar;
    tooFar.position = {10.0f, 0.0f, 0.0f}; // dist 10, too far
    EXPECT_FALSE(test.Test(tooFar));
}

TEST(EQS_CoverTest)
{
    // Threat at origin, agent at (10, 0, 0)
    // Cover test: items with high perpendicular distance from threat-agent line
    CoverTest test({0.0f, 0.0f, 0.0f}, {10.0f, 0.0f, 0.0f}, 3.0f, true);

    // Point directly between threat and agent — NOT in cover
    EQSItem exposed;
    exposed.position = {5.0f, 0.0f, 0.0f};
    EXPECT_FALSE(test.Test(exposed));

    // Point far off to the side — IN cover
    EQSItem covered;
    covered.position = {5.0f, 0.0f, 10.0f};
    EXPECT_TRUE(test.Test(covered));
}

TEST(EQS_PredicateTest)
{
    PredicateTest test("HeightCheck", [](const EQSItem& item) { return item.position.y > 5.0f; });

    EQSItem low;
    low.position = {0.0f, 2.0f, 0.0f};
    EXPECT_FALSE(test.Test(low));

    EQSItem high;
    high.position = {0.0f, 10.0f, 0.0f};
    EXPECT_TRUE(test.Test(high));
}

TEST(EQS_DistanceScorer)
{
    DistanceScorer scorer({0.0f, 0.0f, 0.0f}, 20.0f, true); // prefer closer

    EQSItem close;
    close.position = {2.0f, 0.0f, 0.0f};
    float closeScore = scorer.Score(close);

    EQSItem farItem;
    farItem.position = {15.0f, 0.0f, 0.0f};
    float farScore = scorer.Score(farItem);

    EXPECT_GT(closeScore, farScore);
    EXPECT_GE(closeScore, 0.0f);
    EXPECT_LE(closeScore, 1.0f);
}

TEST(EQS_DirectionScorer)
{
    DirectionScorer scorer({0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}); // prefer +X

    EQSItem forward;
    forward.position = {10.0f, 0.0f, 0.0f};
    float fwdScore = scorer.Score(forward);

    EQSItem backward;
    backward.position = {-10.0f, 0.0f, 0.0f};
    float backScore = scorer.Score(backward);

    EXPECT_GT(fwdScore, backScore);
    EXPECT_GT(fwdScore, 0.5f);  // Should be close to 1.0
    EXPECT_LT(backScore, 0.5f); // Should be close to 0.0
}

TEST(EQS_QueryPipeline)
{
    EQSQuery query;

    // Generate grid around origin
    query.AddGenerator(std::make_unique<GridGenerator>(XMFLOAT3{0.0f, 0.0f, 0.0f}, 10.0f, 2.0f));

    // Filter: only keep points at distance 3-8 from origin
    query.AddTest(std::make_unique<DistanceTest>(XMFLOAT3{0.0f, 0.0f, 0.0f}, 3.0f, 8.0f));

    // Score: prefer points closer to (5, 0, 5)
    auto scorer = std::make_unique<DistanceScorer>(XMFLOAT3{5.0f, 0.0f, 5.0f}, 15.0f, true);
    query.AddScorer(std::move(scorer));

    EQSResult result = query.Execute();

    EXPECT_GT(result.totalGenerated, 0);
    EXPECT_GT(result.totalFiltered, 0);
    EXPECT_LE(result.totalFiltered, result.totalGenerated);
    EXPECT_TRUE(result.HasValidItem());

    // Best item should be close to (5, 0, 5) and within distance range
    EQSItem best = result.GetBestItem();
    EXPECT_FALSE(best.filtered);
    EXPECT_GT(best.score, 0.0f);
}

TEST(EQS_EmptyResult)
{
    EQSQuery query;

    // Generate but filter everything out
    query.AddGenerator(std::make_unique<GridGenerator>(XMFLOAT3{0.0f, 0.0f, 0.0f}, 5.0f, 1.0f));
    query.AddTest(std::make_unique<DistanceTest>(XMFLOAT3{100.0f, 0.0f, 0.0f}, 0.0f, 0.1f)); // Nothing near (100,0,0)

    EQSResult result = query.Execute();

    EXPECT_GT(result.totalGenerated, 0);
    EXPECT_EQ(result.totalFiltered, 0);
    EXPECT_FALSE(result.HasValidItem());
}

TEST(EQS_TopItems)
{
    EQSQuery query;
    query.AddGenerator(std::make_unique<GridGenerator>(XMFLOAT3{0.0f, 0.0f, 0.0f}, 10.0f, 1.0f));
    query.AddScorer(std::make_unique<DistanceScorer>(XMFLOAT3{5.0f, 0.0f, 5.0f}, 20.0f, true));

    EQSResult result = query.Execute();
    auto top3 = result.GetTopItems(3);

    EXPECT_LE(static_cast<int>(top3.size()), 3);
    if (top3.size() >= 2)
    {
        EXPECT_GE(top3[0].score, top3[1].score); // Sorted descending
    }
}

TEST(EQS_QueryTimeMeasured)
{
    EQSQuery query;
    query.AddGenerator(std::make_unique<GridGenerator>(XMFLOAT3{0.0f, 0.0f, 0.0f}, 20.0f, 1.0f));
    query.AddScorer(std::make_unique<DistanceScorer>(XMFLOAT3{0.0f, 0.0f, 0.0f}, 30.0f, true));

    EQSResult result = query.Execute();
    EXPECT_GE(result.queryTimeMs, 0.0f);
}
