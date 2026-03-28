// TestSceneGraph2D.cpp - Tests for 2D scene graph system

#include "TestFramework.h"
#include "Engine/2D/SceneGraph2D.h"

using namespace Spark::Engine2D;

TEST(SceneGraph2D_NodeCreation)
{
    Node2D node("TestNode");
    EXPECT_EQ(node.GetName(), std::string("TestNode"));
    EXPECT_TRUE(node.IsVisible());
}

TEST(SceneGraph2D_SetPosition)
{
    Node2D node("N");
    node.SetPosition(100.0f, 200.0f);
    EXPECT_NEAR(node.GetTransform().x, 100.0f, 0.01f);
    EXPECT_NEAR(node.GetTransform().y, 200.0f, 0.01f);
}

TEST(SceneGraph2D_SetScale)
{
    Node2D node("N");
    node.SetScale(2.0f, 3.0f);
    EXPECT_NEAR(node.GetTransform().scaleX, 2.0f, 0.01f);
    EXPECT_NEAR(node.GetTransform().scaleY, 3.0f, 0.01f);
}

TEST(SceneGraph2D_SetRotation)
{
    Node2D node("N");
    node.SetRotation(1.57f);
    EXPECT_NEAR(node.GetTransform().rotation, 1.57f, 0.01f);
}

TEST(SceneGraph2D_Visibility)
{
    Node2D node("N");
    node.SetVisible(false);
    EXPECT_FALSE(node.IsVisible());
    node.SetVisible(true);
    EXPECT_TRUE(node.IsVisible());
}

TEST(SceneGraph2D_ZOrder)
{
    Node2D node("N");
    node.SetZOrder(5);
    EXPECT_EQ(node.GetZOrder(), 5);
}

TEST(SceneGraph2D_AddChild)
{
    Node2D parent("Parent");
    auto child = std::make_unique<Node2D>("Child");
    auto* childPtr = parent.AddChild(std::move(child));
    EXPECT_NE(childPtr, nullptr);
    EXPECT_EQ(parent.GetChildren().size(), static_cast<size_t>(1));
}

TEST(SceneGraph2D_FindChild)
{
    Node2D parent("Parent");
    parent.AddChild(std::make_unique<Node2D>("ChildA"));
    parent.AddChild(std::make_unique<Node2D>("ChildB"));

    auto* found = parent.FindChild("ChildB");
    EXPECT_NE(found, nullptr);
    EXPECT_EQ(found->GetName(), std::string("ChildB"));

    EXPECT_EQ(parent.FindChild("Missing"), nullptr);
}

TEST(SceneGraph2D_RemoveChild)
{
    Node2D parent("Parent");
    parent.AddChild(std::make_unique<Node2D>("Child"));
    EXPECT_EQ(parent.GetChildren().size(), static_cast<size_t>(1));
    parent.RemoveChild("Child");
    EXPECT_EQ(parent.GetChildren().size(), static_cast<size_t>(0));
}

TEST(SceneGraph2D_SpriteNode)
{
    SpriteNode sprite("TestSprite");
    sprite.SetTexture(42);
    EXPECT_EQ(sprite.GetTextureId(), static_cast<uint32_t>(42));
}

TEST(SceneGraph2D_TextNode)
{
    TextNode text("Label", "Hello World");
    EXPECT_EQ(text.GetText(), std::string("Hello World"));
    text.SetText("Updated");
    EXPECT_EQ(text.GetText(), std::string("Updated"));
}

TEST(SceneGraph2D_SetColor)
{
    Node2D node("N");
    EXPECT_NO_THROW(node.SetColor(1.0f, 0.0f, 0.0f, 0.5f));
}

TEST(SceneGraph2D_SetOpacity)
{
    Node2D node("N");
    EXPECT_NO_THROW(node.SetOpacity(0.5f));
}

TEST(SceneGraph2D_Update)
{
    Node2D parent("Parent");
    parent.SetPosition(10.0f, 20.0f);
    parent.AddChild(std::make_unique<Node2D>("Child"));
    EXPECT_NO_THROW(parent.Update());
}
