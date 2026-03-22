// TestSceneManager.cpp - Tests for scene hierarchy, metadata, and dirty-state tracking
// Standalone implementations for CI testing (no engine dependency)

#include "TestFramework.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>

namespace TestScene
{

    struct Vec3
    {
        float x = 0.0f, y = 0.0f, z = 0.0f;
    };

    struct SceneNode
    {
        std::string name;
        std::string type;
        Vec3 position;
        Vec3 rotation;
        Vec3 scale = {1, 1, 1};
        std::string modelPath;
        int parentIndex = -1;
        std::vector<int> childIndices;
        std::unordered_map<std::string, std::string> properties;
    };

    struct SceneMetadata
    {
        std::string sceneName;
        std::string author;
        float ambientR = 0.1f, ambientG = 0.1f, ambientB = 0.1f;
        float gravityX = 0.0f, gravityY = -9.81f, gravityZ = 0.0f;
    };

    class SceneManager
    {
      public:
        void NewScene(const std::string& name = "Untitled")
        {
            m_nodes.clear();
            m_nameIndex.clear();
            m_metadata = {};
            m_metadata.sceneName = name;
            m_dirty = false;
        }

        int AddNode(const SceneNode& node)
        {
            int idx = static_cast<int>(m_nodes.size());
            m_nodes.push_back(node);
            m_nodes.back().childIndices.clear();
            m_nameIndex[node.name] = idx;

            if (node.parentIndex >= 0 && node.parentIndex < static_cast<int>(m_nodes.size()))
            {
                m_nodes[node.parentIndex].childIndices.push_back(idx);
            }

            m_dirty = true;
            return idx;
        }

        void RemoveNode(int index)
        {
            if (index < 0 || index >= static_cast<int>(m_nodes.size()))
                return;

            // Collect subtree indices (BFS)
            std::vector<int> toRemove;
            toRemove.push_back(index);
            for (size_t i = 0; i < toRemove.size(); ++i)
            {
                for (int child : m_nodes[toRemove[i]].childIndices)
                    toRemove.push_back(child);
            }

            // Unlink from parent
            int parent = m_nodes[index].parentIndex;
            if (parent >= 0 && parent < static_cast<int>(m_nodes.size()))
            {
                auto& children = m_nodes[parent].childIndices;
                children.erase(std::remove(children.begin(), children.end(), index), children.end());
            }

            // Sort descending so we remove from back first
            std::sort(toRemove.begin(), toRemove.end(), std::greater<int>());
            for (int idx : toRemove)
            {
                m_nameIndex.erase(m_nodes[idx].name);
                m_nodes.erase(m_nodes.begin() + idx);
            }

            // Rebuild name index and fix parent/child indices
            RebuildIndices();
            m_dirty = true;
        }

        void SetParent(int childIndex, int parentIndex)
        {
            if (childIndex < 0 || childIndex >= static_cast<int>(m_nodes.size()))
                return;

            // Remove from old parent
            int oldParent = m_nodes[childIndex].parentIndex;
            if (oldParent >= 0 && oldParent < static_cast<int>(m_nodes.size()))
            {
                auto& children = m_nodes[oldParent].childIndices;
                children.erase(std::remove(children.begin(), children.end(), childIndex), children.end());
            }

            // Set new parent
            m_nodes[childIndex].parentIndex = parentIndex;
            if (parentIndex >= 0 && parentIndex < static_cast<int>(m_nodes.size()))
            {
                m_nodes[parentIndex].childIndices.push_back(childIndex);
            }

            m_dirty = true;
        }

        std::vector<int> GetRootNodes() const
        {
            std::vector<int> roots;
            for (int i = 0; i < static_cast<int>(m_nodes.size()); ++i)
            {
                if (m_nodes[i].parentIndex == -1)
                    roots.push_back(i);
            }
            return roots;
        }

        const SceneNode* GetNode(int index) const
        {
            if (index < 0 || index >= static_cast<int>(m_nodes.size()))
                return nullptr;
            return &m_nodes[index];
        }

        SceneNode* GetNode(int index)
        {
            if (index < 0 || index >= static_cast<int>(m_nodes.size()))
                return nullptr;
            return &m_nodes[index];
        }

        int FindNode(const std::string& name) const
        {
            auto it = m_nameIndex.find(name);
            return (it != m_nameIndex.end()) ? it->second : -1;
        }

        int GetNodeCount() const { return static_cast<int>(m_nodes.size()); }

        bool IsDirty() const { return m_dirty; }
        void MarkDirty() { m_dirty = true; }
        void ClearDirty() { m_dirty = false; }

        const SceneMetadata& GetMetadata() const { return m_metadata; }
        SceneMetadata& GetMetadata() { return m_metadata; }

      private:
        void RebuildIndices()
        {
            m_nameIndex.clear();
            for (int i = 0; i < static_cast<int>(m_nodes.size()); ++i)
            {
                m_nameIndex[m_nodes[i].name] = i;
                // Fix child indices by scanning for children
                m_nodes[i].childIndices.clear();
            }
            for (int i = 0; i < static_cast<int>(m_nodes.size()); ++i)
            {
                int p = m_nodes[i].parentIndex;
                if (p >= 0 && p < static_cast<int>(m_nodes.size()))
                    m_nodes[p].childIndices.push_back(i);
            }
        }

        std::vector<SceneNode> m_nodes;
        std::unordered_map<std::string, int> m_nameIndex;
        SceneMetadata m_metadata;
        bool m_dirty = false;
    };

} // namespace TestScene

// =============================================================================
// Tests
// =============================================================================

TEST(Scene_NewScene)
{
    TestScene::SceneManager mgr;
    mgr.NewScene("TestLevel");

    EXPECT_EQ(mgr.GetNodeCount(), 0);
    EXPECT_FALSE(mgr.IsDirty());
    EXPECT_EQ(mgr.GetMetadata().sceneName, std::string("TestLevel"));
}

TEST(Scene_AddNode)
{
    TestScene::SceneManager mgr;
    mgr.NewScene();

    TestScene::SceneNode node;
    node.name = "Ground";
    node.type = "plane";

    int idx = mgr.AddNode(node);
    EXPECT_EQ(idx, 0);
    EXPECT_EQ(mgr.GetNodeCount(), 1);
    EXPECT_TRUE(mgr.IsDirty());

    const auto* n = mgr.GetNode(0);
    EXPECT_TRUE(n != nullptr);
    EXPECT_EQ(n->name, std::string("Ground"));
    EXPECT_EQ(n->type, std::string("plane"));
}

TEST(Scene_FindNode)
{
    TestScene::SceneManager mgr;
    mgr.NewScene();

    TestScene::SceneNode a;
    a.name = "Alpha";
    TestScene::SceneNode b;
    b.name = "Beta";

    mgr.AddNode(a);
    mgr.AddNode(b);

    EXPECT_EQ(mgr.FindNode("Alpha"), 0);
    EXPECT_EQ(mgr.FindNode("Beta"), 1);
    EXPECT_EQ(mgr.FindNode("Gamma"), -1);
}

TEST(Scene_ParentChild)
{
    TestScene::SceneManager mgr;
    mgr.NewScene();

    TestScene::SceneNode parent;
    parent.name = "Parent";
    TestScene::SceneNode child;
    child.name = "Child";
    child.parentIndex = 0;

    mgr.AddNode(parent);
    mgr.AddNode(child);

    const auto* p = mgr.GetNode(0);
    EXPECT_EQ(static_cast<int>(p->childIndices.size()), 1);
    EXPECT_EQ(p->childIndices[0], 1);

    const auto* c = mgr.GetNode(1);
    EXPECT_EQ(c->parentIndex, 0);
}

TEST(Scene_Reparent)
{
    TestScene::SceneManager mgr;
    mgr.NewScene();

    TestScene::SceneNode a, b, c;
    a.name = "A";
    b.name = "B";
    c.name = "C";
    c.parentIndex = 0; // child of A

    mgr.AddNode(a);
    mgr.AddNode(b);
    mgr.AddNode(c);

    // C is child of A
    EXPECT_EQ(mgr.GetNode(0)->childIndices.size(), (size_t)1);
    EXPECT_EQ(mgr.GetNode(1)->childIndices.size(), (size_t)0);

    // Reparent C to B
    mgr.SetParent(2, 1);
    EXPECT_EQ(mgr.GetNode(0)->childIndices.size(), (size_t)0);
    EXPECT_EQ(mgr.GetNode(1)->childIndices.size(), (size_t)1);
    EXPECT_EQ(mgr.GetNode(2)->parentIndex, 1);
}

TEST(Scene_DetachFromParent)
{
    TestScene::SceneManager mgr;
    mgr.NewScene();

    TestScene::SceneNode parent, child;
    parent.name = "Parent";
    child.name = "Child";
    child.parentIndex = 0;

    mgr.AddNode(parent);
    mgr.AddNode(child);

    // Detach child to become root
    mgr.SetParent(1, -1);
    EXPECT_EQ(mgr.GetNode(1)->parentIndex, -1);
    EXPECT_EQ(mgr.GetNode(0)->childIndices.size(), (size_t)0);
}

TEST(Scene_RootNodes)
{
    TestScene::SceneManager mgr;
    mgr.NewScene();

    TestScene::SceneNode a, b, c;
    a.name = "Root1";
    b.name = "Root2";
    c.name = "Child";
    c.parentIndex = 0;

    mgr.AddNode(a);
    mgr.AddNode(b);
    mgr.AddNode(c);

    auto roots = mgr.GetRootNodes();
    EXPECT_EQ(static_cast<int>(roots.size()), 2);
    EXPECT_EQ(roots[0], 0);
    EXPECT_EQ(roots[1], 1);
}

TEST(Scene_RemoveNode)
{
    TestScene::SceneManager mgr;
    mgr.NewScene();

    TestScene::SceneNode a, b, c;
    a.name = "A";
    b.name = "B";
    c.name = "C";

    mgr.AddNode(a);
    mgr.AddNode(b);
    mgr.AddNode(c);

    mgr.RemoveNode(1); // Remove "B"
    EXPECT_EQ(mgr.GetNodeCount(), 2);
    EXPECT_EQ(mgr.FindNode("B"), -1);
    EXPECT_TRUE(mgr.FindNode("A") >= 0);
    EXPECT_TRUE(mgr.FindNode("C") >= 0);
}

TEST(Scene_RemoveSubtree)
{
    TestScene::SceneManager mgr;
    mgr.NewScene();

    TestScene::SceneNode root, child1, child2;
    root.name = "Root";
    child1.name = "Child1";
    child1.parentIndex = 0;
    child2.name = "Child2";
    child2.parentIndex = 0;

    mgr.AddNode(root);
    mgr.AddNode(child1);
    mgr.AddNode(child2);

    EXPECT_EQ(mgr.GetNodeCount(), 3);

    // Remove root — should remove children too
    mgr.RemoveNode(0);
    EXPECT_EQ(mgr.GetNodeCount(), 0);
}

TEST(Scene_DirtyFlag)
{
    TestScene::SceneManager mgr;
    mgr.NewScene("Test");

    EXPECT_FALSE(mgr.IsDirty());

    TestScene::SceneNode node;
    node.name = "Box";
    mgr.AddNode(node);
    EXPECT_TRUE(mgr.IsDirty());

    mgr.ClearDirty();
    EXPECT_FALSE(mgr.IsDirty());

    mgr.MarkDirty();
    EXPECT_TRUE(mgr.IsDirty());

    // NewScene clears dirty
    mgr.NewScene();
    EXPECT_FALSE(mgr.IsDirty());
}

TEST(Scene_Metadata)
{
    TestScene::SceneManager mgr;
    mgr.NewScene("Level01");

    auto& meta = mgr.GetMetadata();
    EXPECT_EQ(meta.sceneName, std::string("Level01"));

    meta.author = "TestAuthor";
    meta.gravityY = -20.0f;
    meta.ambientR = 0.5f;

    EXPECT_EQ(mgr.GetMetadata().author, std::string("TestAuthor"));
    EXPECT_NEAR(mgr.GetMetadata().gravityY, -20.0f, 0.001f);
    EXPECT_NEAR(mgr.GetMetadata().ambientR, 0.5f, 0.001f);
}

TEST(Scene_NodeProperties)
{
    TestScene::SceneManager mgr;
    mgr.NewScene();

    TestScene::SceneNode node;
    node.name = "Enemy";
    node.type = "model";
    node.properties["health"] = "100";
    node.properties["faction"] = "hostile";

    mgr.AddNode(node);

    auto* n = mgr.GetNode(0);
    EXPECT_EQ(n->properties.at("health"), std::string("100"));
    EXPECT_EQ(n->properties.at("faction"), std::string("hostile"));
    EXPECT_EQ(n->properties.size(), (size_t)2);
}

TEST(Scene_GetNodeOutOfRange)
{
    TestScene::SceneManager mgr;
    mgr.NewScene();

    EXPECT_TRUE(mgr.GetNode(-1) == nullptr);
    EXPECT_TRUE(mgr.GetNode(0) == nullptr);
    EXPECT_TRUE(mgr.GetNode(100) == nullptr);
}

TEST(Scene_NodeTransform)
{
    TestScene::SceneManager mgr;
    mgr.NewScene();

    TestScene::SceneNode node;
    node.name = "Cube";
    node.type = "cube";
    node.position = {10.0f, 5.0f, -3.0f};
    node.rotation = {45.0f, 90.0f, 0.0f};
    node.scale = {2.0f, 2.0f, 2.0f};

    mgr.AddNode(node);

    const auto* n = mgr.GetNode(0);
    EXPECT_NEAR(n->position.x, 10.0f, 0.001f);
    EXPECT_NEAR(n->position.y, 5.0f, 0.001f);
    EXPECT_NEAR(n->rotation.x, 45.0f, 0.001f);
    EXPECT_NEAR(n->scale.x, 2.0f, 0.001f);
}

// =============================================================================
// Tests — Scene Switching
// =============================================================================

TEST(Scene_SwitchClearsOldScene)
{
    TestScene::SceneManager mgr;
    mgr.NewScene("Level01");

    // Populate first scene
    TestScene::SceneNode a;
    a.name = "Player";
    a.type = "model";
    mgr.AddNode(a);

    TestScene::SceneNode b;
    b.name = "Enemy";
    b.type = "model";
    mgr.AddNode(b);
    EXPECT_EQ(mgr.GetNodeCount(), 2);

    // Switch to a new scene — old content should be gone
    mgr.NewScene("Level02");
    EXPECT_EQ(mgr.GetNodeCount(), 0);
    EXPECT_EQ(mgr.GetMetadata().sceneName, std::string("Level02"));
    EXPECT_EQ(mgr.FindNode("Player"), -1);
    EXPECT_EQ(mgr.FindNode("Enemy"), -1);
    EXPECT_FALSE(mgr.IsDirty());
}

TEST(Scene_SwitchPreservesNewNodes)
{
    TestScene::SceneManager mgr;
    mgr.NewScene("OldLevel");

    TestScene::SceneNode old;
    old.name = "OldObject";
    mgr.AddNode(old);

    // Switch and add new content
    mgr.NewScene("NewLevel");
    TestScene::SceneNode fresh;
    fresh.name = "NewObject";
    fresh.type = "cube";
    mgr.AddNode(fresh);

    EXPECT_EQ(mgr.GetNodeCount(), 1);
    EXPECT_EQ(mgr.FindNode("NewObject"), 0);
    EXPECT_EQ(mgr.FindNode("OldObject"), -1);
}

TEST(Scene_SwitchResetsMetadata)
{
    TestScene::SceneManager mgr;
    mgr.NewScene("Scene1");

    auto& meta1 = mgr.GetMetadata();
    meta1.author = "Alice";
    meta1.gravityY = -20.0f;
    meta1.ambientR = 0.8f;

    // Switch — metadata should be reset to defaults
    mgr.NewScene("Scene2");
    const auto& meta2 = mgr.GetMetadata();
    EXPECT_EQ(meta2.sceneName, std::string("Scene2"));
    EXPECT_EQ(meta2.author, std::string(""));
    EXPECT_NEAR(meta2.gravityY, -9.81f, 0.001f);
    EXPECT_NEAR(meta2.ambientR, 0.1f, 0.001f);
}

TEST(Scene_MultipleSwitches)
{
    TestScene::SceneManager mgr;

    for (int i = 0; i < 5; ++i)
    {
        mgr.NewScene("Level_" + std::to_string(i));
        TestScene::SceneNode node;
        node.name = "Node_" + std::to_string(i);
        mgr.AddNode(node);
        EXPECT_EQ(mgr.GetNodeCount(), 1);
        EXPECT_EQ(mgr.FindNode("Node_" + std::to_string(i)), 0);
    }

    // After last switch, only the last node should exist
    EXPECT_EQ(mgr.GetMetadata().sceneName, std::string("Level_4"));
    EXPECT_EQ(mgr.FindNode("Node_4"), 0);
    EXPECT_EQ(mgr.FindNode("Node_0"), -1);
}

TEST(Scene_MutableNodeModification)
{
    TestScene::SceneManager mgr;
    mgr.NewScene();

    TestScene::SceneNode node;
    node.name = "Movable";
    node.type = "cube";
    node.position = {0.0f, 0.0f, 0.0f};
    mgr.AddNode(node);

    // Modify node through mutable accessor
    auto* n = mgr.GetNode(0);
    EXPECT_TRUE(n != nullptr);
    n->position = {5.0f, 10.0f, 15.0f};
    n->properties["speed"] = "42";

    // Verify modifications persisted
    const auto* cn = mgr.GetNode(0);
    EXPECT_NEAR(cn->position.x, 5.0f, 0.001f);
    EXPECT_NEAR(cn->position.y, 10.0f, 0.001f);
    EXPECT_EQ(cn->properties.at("speed"), std::string("42"));
}
