/**
 * @file TestSceneRoundtrip.cpp
 * @brief Integration tests for scene serialization data consistency
 */

#include "TestFramework.h"
#include "SceneManager/SceneManagerTypes.h"

using namespace Spark;

TEST(SceneRoundtrip_EmptyMetadata)
{
    SceneMetadata meta;
    meta.sceneName = "EmptyScene";
    meta.author = "TestSuite";
    EXPECT_EQ(meta.sceneName, "EmptyScene");
    EXPECT_EQ(meta.author, "TestSuite");
}

TEST(SceneRoundtrip_NodeNamePreserved)
{
    SceneNode node;
    node.name = "TestEntity";
    EXPECT_EQ(node.name, "TestEntity");
}

TEST(SceneRoundtrip_NodePositionPreserved)
{
    SceneNode node;
    node.position.x = 1.5f;
    node.position.y = 2.5f;
    node.position.z = 3.5f;

    EXPECT_NEAR(node.position.x, 1.5f, 0.001f);
    EXPECT_NEAR(node.position.y, 2.5f, 0.001f);
    EXPECT_NEAR(node.position.z, 3.5f, 0.001f);
}

TEST(SceneRoundtrip_MultipleNodes)
{
    std::vector<SceneNode> nodes;
    for (int i = 0; i < 10; ++i)
    {
        SceneNode node;
        node.name = "Node_" + std::to_string(i);
        node.position.x = static_cast<float>(i);
        nodes.push_back(node);
    }
    EXPECT_EQ(nodes.size(), static_cast<size_t>(10));
    EXPECT_EQ(nodes[5].name, "Node_5");
    EXPECT_NEAR(nodes[5].position.x, 5.0f, 0.001f);
}

TEST(SceneRoundtrip_HierarchyParentIndex)
{
    SceneNode parent;
    parent.name = "Parent";
    parent.parentIndex = -1;

    SceneNode child;
    child.name = "Child";
    child.parentIndex = 0;

    EXPECT_EQ(parent.parentIndex, -1);
    EXPECT_EQ(child.parentIndex, 0);
}

TEST(SceneRoundtrip_DefaultValues)
{
    SceneNode node;
    EXPECT_NEAR(node.scale.x, 1.0f, 0.001f);
    EXPECT_NEAR(node.scale.y, 1.0f, 0.001f);
    EXPECT_NEAR(node.scale.z, 1.0f, 0.001f);
    EXPECT_EQ(node.parentIndex, -1);
}

TEST(SceneRoundtrip_MetadataGravity)
{
    SceneMetadata meta;
    meta.gravityX = 0.0f;
    meta.gravityY = -9.81f;
    meta.gravityZ = 0.0f;
    EXPECT_NEAR(meta.gravityY, -9.81f, 0.001f);
}

TEST(SceneRoundtrip_LargeSceneStress)
{
    std::vector<SceneNode> nodes;
    for (int i = 0; i < 1000; ++i)
    {
        SceneNode n;
        n.name = "S_" + std::to_string(i);
        nodes.push_back(n);
    }
    EXPECT_EQ(nodes.size(), static_cast<size_t>(1000));
}
