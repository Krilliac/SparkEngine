#include "TestFramework.h"
#include "AssetPipeline/EditorAssetReference.h"
#include <string>

using SparkEditor::EditorAssetKind;
using SparkEditor::IsValidEditorAssetReference;

TEST(EditorAssetReference_AcceptsTypedSafeReferences)
{
    EXPECT_TRUE(IsValidEditorAssetReference("Assets/Models/arena.GLTF", EditorAssetKind::Mesh));
    EXPECT_TRUE(IsValidEditorAssetReference("Assets/Materials/arena.JSON", EditorAssetKind::Material));
}

TEST(EditorAssetReference_RejectsTraversalAndRootedForms)
{
    EXPECT_FALSE(IsValidEditorAssetReference("Assets/../outside.obj", EditorAssetKind::Mesh));
    EXPECT_FALSE(IsValidEditorAssetReference("Assets/Models/../../outside.obj", EditorAssetKind::Mesh));
    EXPECT_FALSE(IsValidEditorAssetReference("Assets/C:/outside.obj", EditorAssetKind::Mesh));
    EXPECT_FALSE(IsValidEditorAssetReference("Assets\\Models\\arena.obj", EditorAssetKind::Mesh));
    EXPECT_FALSE(IsValidEditorAssetReference("Assets//arena.obj", EditorAssetKind::Mesh));
    EXPECT_FALSE(IsValidEditorAssetReference("Assets/Models/", EditorAssetKind::Mesh));
}

TEST(EditorAssetReference_RejectsWrongTypeAndEmbeddedNul)
{
    EXPECT_FALSE(IsValidEditorAssetReference("Assets/Textures/arena.png", EditorAssetKind::Mesh));
    EXPECT_FALSE(IsValidEditorAssetReference("Assets/Models/arena.obj", EditorAssetKind::Material));
    const std::string embedded("Assets/Models/arena.obj\0/escape.obj", sizeof("Assets/Models/arena.obj\0/escape.obj") - 1);
    EXPECT_FALSE(IsValidEditorAssetReference(embedded, EditorAssetKind::Mesh));
}
