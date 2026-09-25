/**
 * @file GLTFAnimationLoader.cpp
 * @brief Validated glTF 2.0 skin animation import (LINEAR TRS channels on skin joints) via cgltf.
 */

#include "GLTFAnimationLoader.h"
#include "GLTFSkinSkeleton.h"
#include "GLTFSkinnedMeshLoader.h"
#include "GLTFValidation.h"

#include <algorithm>
#include <cmath>
#include <new>
#include <unordered_map>
#include <unordered_set>

namespace Spark::Graphics::Detail
{
#if SPARK_HAS_CGLTF
    namespace
    {
        using Spark::Animation::AnimationClip;
        using Spark::Animation::BoneAnimation;
        using Spark::Animation::QuatKey;
        using Spark::Animation::VectorKey;

        constexpr float kIdentityTolerance = 1.0e-5f;
        constexpr float kMinQuaternionLength = 1.0e-6f;

        bool IsIdentity(const float (&matrix)[16])
        {
            for (size_t i = 0; i < 16; ++i)
            {
                const float expected = (i % 5 == 0) ? 1.0f : 0.0f;
                if (std::abs(matrix[i] - expected) > kIdentityTolerance)
                {
                    return false;
                }
            }
            return true;
        }

        std::string NodeLabel(const cgltf_data& data, const cgltf_node* node)
        {
            std::string label = "node " + std::to_string(static_cast<size_t>(node - data.nodes));
            if (node->name && node->name[0] != '\0')
            {
                label += " '" + std::string(node->name) + "'";
            }
            return label;
        }

        /// Read strictly increasing, non-negative, finite key times from a sampler input.
        bool ReadKeyTimes(const cgltf_accessor& input, std::vector<float>& times, std::string& error)
        {
            if (input.type != cgltf_type_scalar || input.component_type != cgltf_component_type_r_32f ||
                input.normalized)
            {
                error = "sampler input must be a non-normalized float SCALAR accessor";
                return false;
            }
            if (input.count == 0 || input.count > kMaxGLTFAnimationKeys)
            {
                error = "sampler input has " + std::to_string(input.count) + " keys; 1.." +
                        std::to_string(kMaxGLTFAnimationKeys) + " are supported";
                return false;
            }
            if (!GLTF::UnpackFloats(input, 1, times, error))
            {
                error = "sampler input: " + error;
                return false;
            }
            for (size_t k = 0; k < times.size(); ++k)
            {
                if (times[k] < 0.0f || (k > 0 && times[k] <= times[k - 1]))
                {
                    error = "sampler input times must be non-negative and strictly increasing (key " +
                            std::to_string(k) + ")";
                    return false;
                }
            }
            return true;
        }

        bool ReadKeyValues(const cgltf_accessor& output, cgltf_animation_path_type path, size_t keyCount,
                           std::vector<float>& values, std::string& error)
        {
            const bool rotation = path == cgltf_animation_path_type_rotation;
            const cgltf_type expectedType = rotation ? cgltf_type_vec4 : cgltf_type_vec3;
            const bool floatComponents = output.component_type == cgltf_component_type_r_32f && !output.normalized;
            const bool normalizedIntegers = rotation && output.normalized &&
                                            (output.component_type == cgltf_component_type_r_8 ||
                                             output.component_type == cgltf_component_type_r_8u ||
                                             output.component_type == cgltf_component_type_r_16 ||
                                             output.component_type == cgltf_component_type_r_16u);
            if (output.type != expectedType || !(floatComponents || normalizedIntegers))
            {
                error = rotation ? "rotation output must be a float or normalized integer VEC4 accessor"
                                 : "translation/scale output must be a non-normalized float VEC3 accessor";
                return false;
            }
            if (output.count != keyCount)
            {
                error = "sampler output has " + std::to_string(output.count) + " values for " +
                        std::to_string(keyCount) + " key times";
                return false;
            }
            if (!GLTF::UnpackFloats(output, rotation ? 4 : 3, values, error))
            {
                error = "sampler output: " + error;
                return false;
            }
            return true;
        }

        bool AppendTrack(const std::vector<float>& times, const std::vector<float>& values,
                         cgltf_animation_path_type path, BoneAnimation& channel, std::string& error)
        {
            if (path == cgltf_animation_path_type_rotation)
            {
                channel.rotationKeys.resize(times.size());
                for (size_t k = 0; k < times.size(); ++k)
                {
                    const float* q = &values[k * 4];
                    const float length = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
                    if (!(length >= kMinQuaternionLength))
                    {
                        error = "rotation key " + std::to_string(k) + " is a zero-length quaternion";
                        return false;
                    }
                    channel.rotationKeys[k] = {times[k], {q[0] / length, q[1] / length, q[2] / length, q[3] / length}};
                }
                return true;
            }

            std::vector<VectorKey>& keys =
                path == cgltf_animation_path_type_translation ? channel.positionKeys : channel.scaleKeys;
            keys.resize(times.size());
            for (size_t k = 0; k < times.size(); ++k)
            {
                keys[k] = {times[k], {values[k * 3], values[k * 3 + 1], values[k * 3 + 2]}};
            }
            return true;
        }

        /// Give every track the file leaves unanimated the joint's rest value (see BoneAnimation).
        void FillRestTracks(const cgltf_node& node, BoneAnimation& channel)
        {
            if (channel.positionKeys.empty())
            {
                channel.positionKeys.push_back({0.0f, {node.translation[0], node.translation[1], node.translation[2]}});
            }
            if (channel.rotationKeys.empty())
            {
                channel.rotationKeys.push_back(
                    {0.0f, {node.rotation[0], node.rotation[1], node.rotation[2], node.rotation[3]}});
            }
            if (channel.scaleKeys.empty())
            {
                channel.scaleKeys.push_back({0.0f, {node.scale[0], node.scale[1], node.scale[2]}});
            }
        }

        struct SkinBinding
        {
            Spark::Animation::Skeleton skeleton;
            std::unordered_map<const cgltf_node*, uint32_t> boneOfNode;
            /// False when the root joint's non-joint ancestors carry a transform.
            bool rootAncestorsAreIdentity = true;
        };

        bool ImportChannel(const cgltf_data& data, const cgltf_animation_channel& source, const SkinBinding& binding,
                           AnimationClip& clip, std::unordered_map<uint32_t, size_t>& channelOfBone, std::string& error)
        {
            const cgltf_node* node = source.target_node;
            if (!node)
            {
                error = "has no target node";
                return false;
            }
            if (source.target_path == cgltf_animation_path_type_weights)
            {
                error = "animates morph target weights, which are not supported";
                return false;
            }
            if (source.target_path != cgltf_animation_path_type_translation &&
                source.target_path != cgltf_animation_path_type_rotation &&
                source.target_path != cgltf_animation_path_type_scale)
            {
                error = "has an invalid target path";
                return false;
            }
            const auto bone = binding.boneOfNode.find(node);
            if (bone == binding.boneOfNode.end())
            {
                error = "targets non-joint " + NodeLabel(data, node) + "; only skin joints can be animated";
                return false;
            }
            if (node->has_matrix)
            {
                error = "targets " + NodeLabel(data, node) + ", which uses a matrix; animated nodes must use TRS";
                return false;
            }
            const uint32_t boneIndex = bone->second;
            if (binding.skeleton.bones[boneIndex].parentIndex < 0 && !binding.rootAncestorsAreIdentity)
            {
                error = "animates root joint " + NodeLabel(data, node) +
                        " under a non-identity non-joint ancestor; apply the armature transform before export";
                return false;
            }

            const cgltf_animation_sampler* sampler = source.sampler;
            if (!sampler || !sampler->input || !sampler->output)
            {
                error = "has no sampler input/output";
                return false;
            }
            if (sampler->interpolation != cgltf_interpolation_type_linear)
            {
                error = "uses STEP or CUBICSPLINE interpolation; only LINEAR is supported";
                return false;
            }

            std::vector<float> times;
            std::vector<float> values;
            if (!ReadKeyTimes(*sampler->input, times, error) ||
                !ReadKeyValues(*sampler->output, source.target_path, times.size(), values, error))
            {
                return false;
            }

            auto [slot, inserted] = channelOfBone.try_emplace(boneIndex, clip.channels.size());
            if (inserted)
            {
                BoneAnimation channel;
                channel.boneName = binding.skeleton.bones[boneIndex].name;
                channel.boneIndex = static_cast<int32_t>(boneIndex);
                clip.channels.push_back(std::move(channel));
            }
            BoneAnimation& channel = clip.channels[slot->second];
            const bool occupied =
                (source.target_path == cgltf_animation_path_type_translation && !channel.positionKeys.empty()) ||
                (source.target_path == cgltf_animation_path_type_rotation && !channel.rotationKeys.empty()) ||
                (source.target_path == cgltf_animation_path_type_scale && !channel.scaleKeys.empty());
            if (occupied)
            {
                error = "duplicates another channel's target path on " + NodeLabel(data, node);
                return false;
            }
            if (!AppendTrack(times, values, source.target_path, channel, error))
            {
                return false;
            }
            clip.duration = std::max(clip.duration, times.back());
            return true;
        }

        bool ImportAnimation(const cgltf_data& data, size_t animationIndex, const SkinBinding& binding,
                             AnimationClip& clip, std::string& error)
        {
            const cgltf_animation& animation = data.animations[animationIndex];
            if (animation.channels_count == 0)
            {
                error = "has no channels";
                return false;
            }

            clip.duration = 0.0f;
            // glTF key times are seconds, which is what the engine samples directly.
            clip.ticksPerSecond = 1.0f;
            clip.loop = true;

            std::unordered_map<uint32_t, size_t> channelOfBone;
            std::vector<const cgltf_node*> nodeOfChannel;
            for (cgltf_size c = 0; c < animation.channels_count; ++c)
            {
                const size_t channelsBefore = clip.channels.size();
                if (!ImportChannel(data, animation.channels[c], binding, clip, channelOfBone, error))
                {
                    error = "channel " + std::to_string(c) + " " + error;
                    return false;
                }
                if (clip.channels.size() > channelsBefore)
                {
                    nodeOfChannel.push_back(animation.channels[c].target_node);
                }
            }
            for (size_t i = 0; i < clip.channels.size(); ++i)
            {
                FillRestTracks(*nodeOfChannel[i], clip.channels[i]);
            }
            return true;
        }

        bool LoadClips(GLTF::Document& document, std::vector<AnimationClip>& clips, std::string& error)
        {
            SkinBinding binding;
            std::vector<uint32_t> boneOfJoint;
            if (!GLTF::LoadSkinSkeleton(document, binding.skeleton, boneOfJoint, error))
            {
                return false;
            }

            const cgltf_data& data = *document.data;
            const cgltf_skin& skin = data.skins[0];
            for (cgltf_size j = 0; j < skin.joints_count; ++j)
            {
                binding.boneOfNode.emplace(skin.joints[j], boneOfJoint[j]);
                if (!binding.skeleton.bones.empty() && boneOfJoint[j] == 0 && skin.joints[j]->parent)
                {
                    float ancestors[16] = {};
                    cgltf_node_transform_world(skin.joints[j]->parent, ancestors);
                    binding.rootAncestorsAreIdentity = IsIdentity(ancestors);
                }
            }

            std::unordered_set<std::string> names;
            clips.reserve(data.animations_count);
            for (cgltf_size a = 0; a < data.animations_count; ++a)
            {
                const cgltf_animation& animation = data.animations[a];
                AnimationClip clip;
                clip.name =
                    animation.name && animation.name[0] != '\0' ? animation.name : "animation_" + std::to_string(a);
                const std::string label = "animation " + std::to_string(a) + " '" + clip.name + "' ";
                if (!names.insert(clip.name).second)
                {
                    error = label + "duplicates another animation's name";
                    return false;
                }
                if (!ImportAnimation(data, a, binding, clip, error))
                {
                    error = label + error;
                    return false;
                }
                clips.push_back(std::move(clip));
            }
            return true;
        }
    } // namespace
#endif

    bool LoadGLTFAnimationClips(const std::filesystem::path& path, std::vector<Spark::Animation::AnimationClip>& clips,
                                std::string& error)
    {
        clips.clear();
        error.clear();

#if !SPARK_HAS_CGLTF
        (void)path;
        error = "cgltf support is not available in this build";
        return false;
#else
        // Clips are bound to the skeleton AnimationManager::LoadSkeleton() builds, and that skeleton is
        // only accepted when the whole skinned mesh imports. Run the same importer first so a file whose
        // skin is valid but whose geometry or weights are not cannot yield clips for a missing skeleton.
        GLTFSkinnedMeshData skinnedMesh;
        if (!LoadGLTFSkinnedMesh(path, skinnedMesh, error))
        {
            error = "skinned mesh rejected: " + error;
            return false;
        }

        GLTF::Document document;
        if (!GLTF::ParseDocument(path, document, error))
        {
            return false;
        }

        bool loaded = false;
        try
        {
            loaded = LoadClips(document, clips, error);
        }
        catch (const std::bad_alloc&)
        {
            error = "out of memory while loading glTF animations";
        }
        if (!loaded)
        {
            clips.clear();
        }
        return loaded;
#endif
    }
} // namespace Spark::Graphics::Detail
