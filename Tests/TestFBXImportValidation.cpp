/**
 * @file TestFBXImportValidation.cpp
 * @brief Validation coverage for FBX importer data classes used by asset pipeline.
 */

#include "TestFramework.h"
#include "Graphics/FBXImporter.h"

using namespace Spark::Graphics;

namespace
{
    FBXMeshData MakeMinimalMesh()
    {
        FBXMeshData mesh;
        mesh.vertices = {
            0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f,
        };
        mesh.indices = {0, 1, 2};
        mesh.vertexCount = 3;
        return mesh;
    }
} // namespace

TEST(FBXImporter_ValidateForPipeline_StaticMesh)
{
    auto& importer = FBXImporter::GetInstance();

    FBXImportResult result;
    result.success = true;
    result.meshes.push_back(MakeMinimalMesh());

    std::string error;
    EXPECT_TRUE(importer.ValidateForPipeline(result, false, false, &error));
    EXPECT_TRUE(error.empty());
}

TEST(FBXImporter_ValidateForPipeline_SkeletalMesh)
{
    auto& importer = FBXImporter::GetInstance();

    FBXImportResult result;
    result.success = true;
    result.meshes.push_back(MakeMinimalMesh());

    FBXBoneData rootBone;
    rootBone.name = "Root";
    rootBone.parentIndex = -1;
    result.bones.push_back(rootBone);

    std::string error;
    EXPECT_TRUE(importer.ValidateForPipeline(result, true, false, &error));
    EXPECT_TRUE(error.empty());
}

TEST(FBXImporter_ValidateForPipeline_AnimationClip)
{
    auto& importer = FBXImporter::GetInstance();

    FBXImportResult result;
    result.success = true;
    result.meshes.push_back(MakeMinimalMesh());

    FBXBoneData rootBone;
    rootBone.name = "Root";
    rootBone.parentIndex = -1;
    result.bones.push_back(rootBone);

    FBXAnimationData clip;
    clip.name = "Walk";
    clip.duration = 1.0f;

    FBXBoneTrack track;
    track.boneName = "Root";
    FBXKeyframe keyframe;
    keyframe.time = 0.0f;
    keyframe.position = {0.0f, 0.0f, 0.0f};
    track.keyframes.push_back(keyframe);
    clip.boneTracks.push_back(track);
    result.animations.push_back(clip);

    std::string error;
    EXPECT_TRUE(importer.ValidateForPipeline(result, true, true, &error));
    EXPECT_TRUE(error.empty());
}
