/**
 * @file GLTFSkinSkeleton.cpp
 * @brief Fail-closed glTF skin hierarchy validation and inverse-bind/bind-pose extraction.
 */

#include "GLTFSkinSkeleton.h"

#if SPARK_HAS_CGLTF

#include "GPUSkinning.h"

#include <cmath>
#include <unordered_map>

namespace Spark::Graphics::Detail::GLTF
{
    namespace
    {
        constexpr float kAffineRowTolerance = 1.0e-5f;
        constexpr double kMinInverseBindDeterminant = 1.0e-12;

        std::string NodeLabel(const cgltf_data& data, const cgltf_node* node)
        {
            const auto index = static_cast<size_t>(node - data.nodes);
            std::string label = "node " + std::to_string(index);
            if (node->name && node->name[0] != '\0')
            {
                label += " '" + std::string(node->name) + "'";
            }
            return label;
        }

        bool AllFinite(const float* values, size_t count)
        {
            for (size_t i = 0; i < count; ++i)
            {
                if (!std::isfinite(values[i]))
                {
                    return false;
                }
            }
            return true;
        }

        bool ValidateNodeGraph(const cgltf_data& data, std::string& error)
        {
            for (cgltf_size i = 0; i < data.nodes_count; ++i)
            {
                // cgltf only rejects cycles in cgltf_validate, and its world-transform helper
                // walks parents unbounded, so detect cycles before anything follows the chain.
                const cgltf_node* ancestor = data.nodes[i].parent;
                for (cgltf_size depth = 0; ancestor; ++depth)
                {
                    if (depth >= data.nodes_count)
                    {
                        error = "node hierarchy contains a cycle through " + NodeLabel(data, &data.nodes[i]);
                        return false;
                    }
                    ancestor = ancestor->parent;
                }

                const cgltf_node& node = data.nodes[i];
                if (!AllFinite(node.translation, 3) || !AllFinite(node.rotation, 4) || !AllFinite(node.scale, 3) ||
                    !AllFinite(node.matrix, 16))
                {
                    error = NodeLabel(data, &node) + " has a non-finite transform";
                    return false;
                }
            }
            return true;
        }

        bool ValidateSkinBinding(const cgltf_data& data, std::string& error)
        {
            if (data.skins_count != 1)
            {
                error = data.skins_count == 0 ? "glTF contains no skin; use the static-mesh loader"
                                              : "only one skin per glTF file is supported (found " +
                                                    std::to_string(data.skins_count) + ")";
                return false;
            }

            const cgltf_skin& skin = data.skins[0];
            if (skin.joints_count == 0)
            {
                error = "skin has no joints";
                return false;
            }
            if (skin.joints_count > kMaxBonesPerMesh)
            {
                error = "skin has " + std::to_string(skin.joints_count) + " joints; GPU skinning supports at most " +
                        std::to_string(kMaxBonesPerMesh);
                return false;
            }

            const cgltf_accessor* inverseBinds = skin.inverse_bind_matrices;
            if (inverseBinds &&
                (inverseBinds->type != cgltf_type_mat4 || inverseBinds->component_type != cgltf_component_type_r_32f ||
                 inverseBinds->normalized || inverseBinds->count < skin.joints_count))
            {
                error = "inverseBindMatrices must be a float MAT4 accessor with one matrix per joint";
                return false;
            }

            size_t skinnedMeshNodes = 0;
            for (cgltf_size i = 0; i < data.nodes_count; ++i)
            {
                const cgltf_node& node = data.nodes[i];
                if (node.mesh && node.skin != &skin)
                {
                    error = NodeLabel(data, &node) + " instances a mesh without the skin; skinned files may not "
                                                     "mix static mesh nodes";
                    return false;
                }
                skinnedMeshNodes += node.mesh ? 1u : 0u;
            }
            if (skinnedMeshNodes == 0)
            {
                error = "no node binds a mesh to the skin";
                return false;
            }
            return true;
        }

        /**
         * @brief Resolve each joint's parent joint (skin joint index, or -1 for the root).
         *
         * Requires a joint's direct parent node to be a joint unless no ancestor is a joint, and
         * exactly one root, so the joints form one connected tree.
         */
        bool ResolveJointParents(const cgltf_data& data, const cgltf_skin& skin, std::vector<int32_t>& parents,
                                 std::string& error)
        {
            std::unordered_map<const cgltf_node*, int32_t> jointIndexOfNode;
            for (cgltf_size j = 0; j < skin.joints_count; ++j)
            {
                if (!jointIndexOfNode.emplace(skin.joints[j], static_cast<int32_t>(j)).second)
                {
                    error = NodeLabel(data, skin.joints[j]) + " is listed more than once in skin joints";
                    return false;
                }
            }

            parents.assign(skin.joints_count, -1);
            size_t rootCount = 0;
            for (cgltf_size j = 0; j < skin.joints_count; ++j)
            {
                const cgltf_node* joint = skin.joints[j];
                const auto directParent = joint->parent ? jointIndexOfNode.find(joint->parent) : jointIndexOfNode.end();
                if (directParent != jointIndexOfNode.end())
                {
                    parents[j] = directParent->second;
                    continue;
                }

                for (const cgltf_node* ancestor = joint->parent; ancestor; ancestor = ancestor->parent)
                {
                    if (jointIndexOfNode.contains(ancestor))
                    {
                        error = NodeLabel(data, joint) + " is separated from its ancestor joint " +
                                NodeLabel(data, ancestor) + " by non-joint " + NodeLabel(data, joint->parent);
                        return false;
                    }
                }
                ++rootCount;
            }

            if (rootCount != 1)
            {
                error = "skin joints form " + std::to_string(rootCount) +
                        " disconnected trees; exactly one root joint is required";
                return false;
            }
            return true;
        }

        void CopyMatrix(const float (&source)[16], XMFLOAT4X4& target)
        {
            // glTF stores column-major column vectors; read row-major, that is the transposed
            // (row-vector) DirectXMath matrix with the translation in _41.._43.
            for (size_t row = 0; row < 4; ++row)
            {
                for (size_t column = 0; column < 4; ++column)
                {
                    target.m[row][column] = source[row * 4 + column];
                }
            }
        }

        bool ValidateInverseBind(const float (&matrix)[16], std::string& error)
        {
            if (!AllFinite(matrix, 16))
            {
                error = "is not finite";
                return false;
            }
            if (std::abs(matrix[3]) > kAffineRowTolerance || std::abs(matrix[7]) > kAffineRowTolerance ||
                std::abs(matrix[11]) > kAffineRowTolerance || std::abs(matrix[15] - 1.0f) > kAffineRowTolerance)
            {
                error = "is not affine (bottom row must be 0,0,0,1)";
                return false;
            }

            const double a = matrix[0], b = matrix[4], c = matrix[8];
            const double d = matrix[1], e = matrix[5], f = matrix[9];
            const double g = matrix[2], h = matrix[6], i = matrix[10];
            const double determinant = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
            if (!(std::abs(determinant) >= kMinInverseBindDeterminant))
            {
                error = "is singular";
                return false;
            }
            return true;
        }

        /// Build a parent-before-child skeleton; @p boneOfJoint maps skin joint index to bone index.
        bool BuildSkeleton(const cgltf_data& data, const cgltf_skin& skin, const std::vector<int32_t>& parents,
                           Spark::Animation::Skeleton& skeleton, std::vector<uint32_t>& boneOfJoint, std::string& error)
        {
            // Stable topological order: joints already listed parent-first keep their indices.
            const size_t jointCount = skin.joints_count;
            std::vector<uint32_t> order;
            std::vector<bool> placed(jointCount, false);
            boneOfJoint.assign(jointCount, 0);
            order.reserve(jointCount);
            while (order.size() < jointCount)
            {
                const size_t placedBefore = order.size();
                for (size_t j = 0; j < jointCount; ++j)
                {
                    if (!placed[j] && (parents[j] < 0 || placed[static_cast<size_t>(parents[j])]))
                    {
                        placed[j] = true;
                        boneOfJoint[j] = static_cast<uint32_t>(order.size());
                        order.push_back(static_cast<uint32_t>(j));
                        break;
                    }
                }
                if (order.size() == placedBefore)
                {
                    error = "joint hierarchy contains a cycle";
                    return false;
                }
            }

            skeleton = {};
            skeleton.name = skin.name && skin.name[0] != '\0' ? skin.name : "skin";
            skeleton.bones.resize(jointCount);
            for (size_t boneIndex = 0; boneIndex < jointCount; ++boneIndex)
            {
                const uint32_t joint = order[boneIndex];
                const cgltf_node* node = skin.joints[joint];
                Spark::Animation::Bone& bone = skeleton.bones[boneIndex];
                bone.name = node->name && node->name[0] != '\0' ? node->name : "joint_" + std::to_string(joint);
                bone.parentIndex = parents[joint] < 0 ? -1 : static_cast<int32_t>(boneOfJoint[parents[joint]]);
                if (!skeleton.boneNameToIndex.emplace(bone.name, static_cast<int32_t>(boneIndex)).second)
                {
                    error = "duplicate joint name '" + bone.name + "'";
                    return false;
                }

                float inverseBind[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
                if (skin.inverse_bind_matrices &&
                    !cgltf_accessor_read_float(skin.inverse_bind_matrices, joint, inverseBind, 16))
                {
                    error = "failed to read the inverse bind matrix of joint " + std::to_string(joint);
                    return false;
                }
                std::string matrixError;
                if (!ValidateInverseBind(inverseBind, matrixError))
                {
                    error = "inverse bind matrix of joint " + std::to_string(joint) + " (" + NodeLabel(data, node) +
                            ") " + matrixError;
                    return false;
                }
                CopyMatrix(inverseBind, bone.offsetMatrix);

                float localBind[16] = {};
                if (bone.parentIndex < 0)
                {
                    cgltf_node_transform_world(node, localBind);
                }
                else
                {
                    cgltf_node_transform_local(node, localBind);
                }
                if (!AllFinite(localBind, 16))
                {
                    error = "bind pose of " + NodeLabel(data, node) + " is not finite";
                    return false;
                }
                CopyMatrix(localBind, bone.localBindPose);
            }
            return true;
        }

    } // namespace

    bool ValidateSkinHierarchy(const cgltf_data& data, std::vector<int32_t>& jointParents, std::string& error)
    {
        return ValidateNodeGraph(data, error) && ValidateSkinBinding(data, error) &&
               ResolveJointParents(data, data.skins[0], jointParents, error);
    }

    bool BuildSkinSkeleton(const cgltf_data& data, const std::vector<int32_t>& jointParents,
                           Spark::Animation::Skeleton& skeleton, std::vector<uint32_t>& boneOfJoint, std::string& error)
    {
        return BuildSkeleton(data, data.skins[0], jointParents, skeleton, boneOfJoint, error);
    }
} // namespace Spark::Graphics::Detail::GLTF

#endif
