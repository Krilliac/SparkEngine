/**
 * @file SceneTextFormat.cpp
 * @brief SceneManager's text scene parsers and writer (see SceneTextFormat.h).
 */

#include "SceneManager/SceneTextFormat.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace Spark
{
    namespace
    {
        bool IsFinite(const DirectX::XMFLOAT3& value)
        {
            return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
        }

        /// A versioned row's type is read with operator>> and written unquoted at the
        /// start of a line, so it must be one whitespace-free token that the reader
        /// does not skip as a comment or brace line.
        bool IsRowType(const std::string& type)
        {
            if (type.empty() || type.front() == '#' || type.front() == '/' || type.front() == '{' ||
                type.front() == '}')
                return false;
            return std::none_of(type.begin(), type.end(),
                                [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; });
        }

        /// Rebuild the derived child lists after every parent reference has been
        /// read, which permits forward references while rejecting dangling,
        /// self-referencing and cyclic hierarchies. Linear: each node's parent chain
        /// is walked once, and a walk stops at the first node already proven to
        /// reach a root.
        bool ValidateAndRebuildHierarchy(std::vector<SceneNode>& nodes)
        {
            for (auto& node : nodes)
                node.childIndices.clear();

            const int count = static_cast<int>(nodes.size());
            for (int i = 0; i < count; ++i)
            {
                const SceneNode& node = nodes[static_cast<size_t>(i)];
                if (node.type.empty() || node.name.empty() || !IsFinite(node.position) || !IsFinite(node.rotation) ||
                    !IsFinite(node.scale))
                    return false;
                if (node.parentIndex < -1 || node.parentIndex >= count || node.parentIndex == i)
                    return false;
            }

            enum class Walk : uint8_t
            {
                Unvisited,
                OnPath,
                ReachesRoot
            };
            std::vector<Walk> state(nodes.size(), Walk::Unvisited);
            std::vector<int> path;
            for (int start = 0; start < count; ++start)
            {
                path.clear();
                int current = start;
                while (current >= 0 && state[static_cast<size_t>(current)] == Walk::Unvisited)
                {
                    state[static_cast<size_t>(current)] = Walk::OnPath;
                    path.push_back(current);
                    current = nodes[static_cast<size_t>(current)].parentIndex;
                }
                if (current >= 0 && state[static_cast<size_t>(current)] == Walk::OnPath)
                    return false; // the chain returned to a node on this walk: a cycle
                for (const int visited : path)
                    state[static_cast<size_t>(visited)] = Walk::ReachesRoot;
            }

            for (int i = 0; i < count; ++i)
            {
                const int parent = nodes[static_cast<size_t>(i)].parentIndex;
                if (parent >= 0)
                    nodes[static_cast<size_t>(parent)].childIndices.push_back(i);
            }
            return true;
        }

        /// Text after a "# key:" prefix of @p prefixLength characters plus its one
        /// separating space; empty when the line ends before that.
        std::string HeaderValue(const std::string& line, size_t prefixLength)
        {
            return line.size() > prefixLength + 1 ? line.substr(prefixLength + 1) : std::string();
        }

        bool ParseFiniteTriple(const std::string& text, float& x, float& y, float& z)
        {
            std::istringstream values(text);
            return (values >> x >> y >> z) && std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
        }

        std::string Trim(const std::string& value)
        {
            const auto first = value.find_first_not_of(" \t\r");
            if (first == std::string::npos)
                return std::string{};
            const auto last = value.find_last_not_of(" \t\r");
            return value.substr(first, last - first + 1);
        }

        /// "x y z" or "x, y, z" with an optional trailing '#' comment, all finite.
        bool ParseIniVector(const std::string& value, DirectX::XMFLOAT3& output)
        {
            std::string normalized = value;
            const auto comment = normalized.find('#');
            if (comment != std::string::npos)
                normalized.erase(comment);
            std::replace(normalized.begin(), normalized.end(), ',', ' ');
            std::istringstream values(normalized);
            DirectX::XMFLOAT3 parsed{};
            if (!(values >> parsed.x >> parsed.y >> parsed.z))
                return false;
            values >> std::ws;
            if (!values.eof() || !IsFinite(parsed))
                return false;
            output = parsed;
            return true;
        }
    } // namespace

    SceneTextDialect DetectSceneTextDialect(std::string_view content)
    {
        // SaveScene writes the versioned format for both .json and .scene, so its
        // own .scene output must route back to the versioned reader before either
        // older syntax is considered.
        constexpr std::string_view kSerializedHeader = "# SparkEngine Scene v1.0";
        if (content.starts_with(kSerializedHeader) &&
            (content.size() == kSerializedHeader.size() || content[kSerializedHeader.size()] == '\r' ||
             content[kSerializedHeader.size()] == '\n'))
            return SceneTextDialect::Versioned;

        if (content.contains("[Scene]") || content.contains("[Object]") || content.contains("[Camera]") ||
            content.contains("[SpawnPoint]"))
            return SceneTextDialect::Ini;
        return SceneTextDialect::LegacyObjects;
    }

    bool ParseVersionedSceneText(std::string_view content, SceneMetadata& metadata, std::vector<SceneNode>& nodes)
    {
        // Lines starting with '#' are comments or metadata headers; data lines are
        //   type name posX posY posZ [rotX rotY rotZ scaleX scaleY scaleZ parentIndex
        //                             [modelPath materialPath propertyCount {key value}]]
        SceneMetadata stagedMetadata = metadata;
        std::vector<SceneNode> stagedNodes;
        std::istringstream ss{std::string(content)};
        std::string line;

        while (std::getline(ss, line))
        {
            if (line.empty())
                continue;
            if (line[0] == '#')
            {
                if (line.starts_with("# name:"))
                    stagedMetadata.sceneName = HeaderValue(line, 7);
                else if (line.starts_with("# gravity:"))
                {
                    if (!ParseFiniteTriple(HeaderValue(line, 10), stagedMetadata.gravityX, stagedMetadata.gravityY,
                                           stagedMetadata.gravityZ))
                        return false;
                }
                else if (line.starts_with("# author:"))
                    stagedMetadata.author = HeaderValue(line, 9);
                else if (line.starts_with("# version:"))
                    stagedMetadata.version = HeaderValue(line, 10);
                else if (line.starts_with("# description:"))
                    stagedMetadata.description = HeaderValue(line, 14);
                else if (line.starts_with("# ambient:"))
                {
                    if (!ParseFiniteTriple(HeaderValue(line, 10), stagedMetadata.ambientLightR,
                                           stagedMetadata.ambientLightG, stagedMetadata.ambientLightB))
                        return false;
                }
                continue;
            }
            if (line[0] == '/' || line[0] == '{' || line[0] == '}')
                continue;

            if (stagedNodes.size() >= kMaxSceneTextNodes)
                return false;

            std::istringstream ls(line);
            SceneNode node;
            // std::quoted also accepts historical unquoted names, while new saves
            // can round-trip names containing whitespace and quotes.
            if (!(ls >> node.type >> std::quoted(node.name) >> node.position.x >> node.position.y >> node.position.z) ||
                !IsRowType(node.type) || node.name.empty() || !IsFinite(node.position))
                return false;

            // Extended fields are optional for old line-oriented files, but once
            // present they are an all-or-nothing, finite record.
            ls >> std::ws;
            if (!ls.eof())
            {
                if (!(ls >> node.rotation.x >> node.rotation.y >> node.rotation.z >> node.scale.x >> node.scale.y >>
                      node.scale.z >> node.parentIndex) ||
                    !IsFinite(node.rotation) || !IsFinite(node.scale))
                    return false;

                ls >> std::ws;
                if (!ls.eof())
                {
                    size_t propertyCount = 0;
                    if (!(ls >> std::quoted(node.modelPath) >> std::quoted(node.materialPath) >> propertyCount))
                        return false;
                    // Each property needs input bytes, so a huge count fails at end of line.
                    for (size_t property = 0; property < propertyCount; ++property)
                    {
                        std::string key;
                        std::string value;
                        if (!(ls >> std::quoted(key) >> std::quoted(value)))
                            return false;
                        node.properties.emplace(std::move(key), std::move(value));
                    }
                    ls >> std::ws;
                    if (!ls.eof())
                        return false;
                }
            }
            stagedNodes.push_back(std::move(node));
        }

        if (stagedNodes.empty() || !ValidateAndRebuildHierarchy(stagedNodes))
            return false;
        metadata = std::move(stagedMetadata);
        nodes = std::move(stagedNodes);
        return true;
    }

    bool ParseIniSceneText(std::string_view content, SceneMetadata& metadata, std::vector<SceneNode>& nodes)
    {
        // A malformed field invalidates the whole candidate, so a valid prefix is
        // never published as a partly loaded scene.
        SceneMetadata stagedMetadata{};
        std::vector<SceneNode> stagedNodes;
        std::istringstream ss{std::string(content)};
        std::string line;
        std::string currentSection;
        SceneNode currentNode;
        bool hasNode = false;
        bool nodeHasPosition = false;
        bool nodeInvalid = false;
        bool parseError = false;

        auto flushNode = [&]()
        {
            if (!hasNode)
                return;
            const bool requiresPosition = currentNode.type == "Camera" || currentNode.type == "SpawnPoint";
            if (nodeInvalid || currentNode.type.empty() || (requiresPosition && !nodeHasPosition) ||
                stagedNodes.size() >= kMaxSceneTextNodes)
            {
                parseError = true;
            }
            else
            {
                if (currentNode.name.empty())
                    currentNode.name = currentNode.type + "_" + std::to_string(stagedNodes.size());
                if (currentNode.type == "SpawnPoint")
                {
                    const auto tag = currentNode.properties.find("tag");
                    if (tag == currentNode.properties.end() || tag->second.empty())
                        parseError = true;
                }
                stagedNodes.push_back(std::move(currentNode));
            }
            currentNode = SceneNode{};
            hasNode = false;
            nodeHasPosition = false;
            nodeInvalid = false;
        };

        while (std::getline(ss, line) && !parseError)
        {
            line = Trim(line);
            if (line.empty() || line[0] == '#' || line[0] == ';')
                continue;

            if (line.front() == '[')
            {
                if (line.back() != ']')
                {
                    parseError = true;
                    continue;
                }
                flushNode();
                currentSection = Trim(line.substr(1, line.size() - 2));

                if (currentSection == "Object" || currentSection == "Terrain" || currentSection == "SpawnPoint" ||
                    currentSection == "Camera")
                {
                    hasNode = true;
                    if (currentSection == "SpawnPoint" || currentSection == "Camera")
                        currentNode.type = currentSection;
                }
                continue;
            }

            const auto eqPos = line.find('=');
            if (eqPos == std::string::npos)
            {
                if (hasNode || currentSection == "Scene")
                    parseError = true;
                continue;
            }
            const std::string key = Trim(line.substr(0, eqPos));
            const std::string value = Trim(line.substr(eqPos + 1));
            if (key.empty())
            {
                parseError = true;
                continue;
            }

            if (currentSection == "Scene")
            {
                DirectX::XMFLOAT3 vector{};
                if (key == "name")
                    stagedMetadata.sceneName = value;
                else if (key == "author")
                    stagedMetadata.author = value;
                else if (key == "version")
                    stagedMetadata.version = value;
                else if (key == "description")
                    stagedMetadata.description = value;
                else if (key == "ambientLight")
                {
                    if (!ParseIniVector(value, vector))
                        parseError = true;
                    else
                    {
                        stagedMetadata.ambientLightR = vector.x;
                        stagedMetadata.ambientLightG = vector.y;
                        stagedMetadata.ambientLightB = vector.z;
                    }
                }
                else if (key == "gravity")
                {
                    if (!ParseIniVector(value, vector))
                        parseError = true;
                    else
                    {
                        stagedMetadata.gravityX = vector.x;
                        stagedMetadata.gravityY = vector.y;
                        stagedMetadata.gravityZ = vector.z;
                    }
                }
            }
            else if (hasNode)
            {
                if (key == "type")
                {
                    if (currentSection == "Camera" || currentSection == "SpawnPoint")
                        currentNode.properties[key] = value;
                    else
                        currentNode.type = value;
                }
                else if (key == "name")
                    currentNode.name = value;
                else if (key == "model")
                    currentNode.modelPath = value;
                else if (key == "position")
                {
                    nodeHasPosition = true;
                    nodeInvalid = !ParseIniVector(value, currentNode.position) || nodeInvalid;
                }
                else if (key == "rotation")
                    nodeInvalid = !ParseIniVector(value, currentNode.rotation) || nodeInvalid;
                else if (key == "scale")
                    nodeInvalid = !ParseIniVector(value, currentNode.scale) || nodeInvalid;
                else if (key == "material")
                    currentNode.materialPath = value;
                else
                    currentNode.properties[key] = value;
            }
        }
        flushNode();

        if (parseError || stagedNodes.empty() || !ValidateAndRebuildHierarchy(stagedNodes))
            return false;
        std::unordered_map<std::string, int> names;
        for (int i = 0; i < static_cast<int>(stagedNodes.size()); ++i)
        {
            if (!names.emplace(stagedNodes[static_cast<size_t>(i)].name, i).second)
                return false;
        }
        metadata = std::move(stagedMetadata);
        nodes = std::move(stagedNodes);
        return true;
    }

    bool ParseLegacyObjectLines(std::string_view content, std::vector<LegacyObjectRow>& rows)
    {
        std::vector<LegacyObjectRow> stagedRows;
        std::istringstream ss{std::string(content)};
        std::string line;
        int lineNumber = 0;

        while (std::getline(ss, line))
        {
            ++lineNumber;
            const auto comment = line.find('#');
            if (comment != std::string::npos)
                line.erase(comment);
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
                line.pop_back();
            if (line.empty())
                continue;
            if (stagedRows.size() >= kMaxSceneTextNodes)
                return false;

            std::istringstream ls(line);
            LegacyObjectRow row;
            row.lineNumber = lineNumber;
            ls >> row.type;
            if (row.type.empty() || !(ls >> row.position.x >> row.position.y >> row.position.z) ||
                !IsFinite(row.position))
                return false;
            ls >> std::ws;

            // Every primitive constructor requires positive dimensions; a zero,
            // negative or non-finite value would trip its precondition.
            auto positiveFloat = [&ls](float& value) { return bool(ls >> value) && std::isfinite(value) && value > 0; };
            const bool hasParameters = ls.peek() != EOF;

            if (row.type == "Cube" || row.type == "Pyramid")
            {
                if (hasParameters && !positiveFloat(row.primary))
                    return false;
            }
            else if (row.type == "Plane")
            {
                row.primary = 10.0f;
                row.secondary = 10.0f;
                if (hasParameters && (!positiveFloat(row.primary) || !positiveFloat(row.secondary)))
                    return false;
            }
            else if (row.type == "Sphere")
            {
                row.primary = 0.5f;
                if (hasParameters && (!positiveFloat(row.primary) || !(ls >> row.slices) || !(ls >> row.stacks)))
                    return false;
                if (row.slices < kMinLegacySphereSlices || row.slices > kMaxLegacySphereTessellation ||
                    row.stacks < kMinLegacySphereStacks || row.stacks > kMaxLegacySphereTessellation)
                    return false;
            }
            else if (row.type == "Ramp")
            {
                row.primary = 2.0f;
                row.secondary = 1.0f;
                if (hasParameters && (!positiveFloat(row.primary) || !positiveFloat(row.secondary)))
                    return false;
            }
            else if (row.type == "Wall")
            {
                row.primary = 1.0f;
                row.secondary = 2.0f;
                if (hasParameters && (!positiveFloat(row.primary) || !positiveFloat(row.secondary)))
                    return false;
            }
            else
                return false;

            ls >> std::ws;
            if (!ls.eof())
                return false;
            stagedRows.push_back(std::move(row));
        }

        if (stagedRows.empty())
            return false;
        rows = std::move(stagedRows);
        return true;
    }

    bool SerializeVersionedSceneText(const SceneMetadata& metadata, const std::vector<SceneNode>& nodes,
                                     std::string& text)
    {
        std::vector<SceneNode> written;
        written.reserve(nodes.size());
        std::vector<int> remap(nodes.size(), -1);
        for (size_t oldIndex = 0; oldIndex < nodes.size(); ++oldIndex)
        {
            if (!nodes[oldIndex].type.empty())
            {
                remap[oldIndex] = static_cast<int>(written.size());
                written.push_back(nodes[oldIndex]);
            }
        }
        for (auto& node : written)
        {
            if (node.parentIndex >= 0 && node.parentIndex < static_cast<int>(remap.size()))
            {
                const int mappedParent = remap[static_cast<size_t>(node.parentIndex)];
                node.parentIndex = mappedParent >= 0 ? mappedParent : -2;
            }
            else if (node.parentIndex < -1)
                node.parentIndex = -2; // validation below reports this as malformed
        }

        const auto oneLine = [](const std::string& value) { return value.find('\n') == std::string::npos; };
        if (written.empty() || written.size() > kMaxSceneTextNodes || !ValidateAndRebuildHierarchy(written) ||
            !std::isfinite(metadata.gravityX) || !std::isfinite(metadata.gravityY) ||
            !std::isfinite(metadata.gravityZ) || !std::isfinite(metadata.ambientLightR) ||
            !std::isfinite(metadata.ambientLightG) || !std::isfinite(metadata.ambientLightB) ||
            !oneLine(metadata.sceneName) || !oneLine(metadata.author) || !oneLine(metadata.version) ||
            !oneLine(metadata.description))
            return false;
        for (const auto& node : written)
        {
            if (!IsRowType(node.type) || !oneLine(node.name) || !oneLine(node.modelPath) || !oneLine(node.materialPath))
                return false;
            for (const auto& [key, value] : node.properties)
            {
                if (!oneLine(key) || !oneLine(value))
                    return false;
            }
        }

        std::ostringstream serialized;
        serialized << "# SparkEngine Scene v1.0\n";
        serialized << "# name: " << metadata.sceneName << "\n";
        serialized << "# author: " << metadata.author << "\n";
        serialized << "# version: " << metadata.version << "\n";
        serialized << "# description: " << metadata.description << "\n";
        serialized << "# gravity: " << metadata.gravityX << " " << metadata.gravityY << " " << metadata.gravityZ
                   << "\n";
        serialized << "# ambient: " << metadata.ambientLightR << " " << metadata.ambientLightG << " "
                   << metadata.ambientLightB << "\n\n";

        for (const auto& node : written)
        {
            serialized << node.type << " " << std::quoted(node.name) << " " << std::fixed << std::setprecision(3)
                       << node.position.x << " " << node.position.y << " " << node.position.z << " " << node.rotation.x
                       << " " << node.rotation.y << " " << node.rotation.z << " " << node.scale.x << " " << node.scale.y
                       << " " << node.scale.z << " " << node.parentIndex << " " << std::quoted(node.modelPath) << " "
                       << std::quoted(node.materialPath) << " " << node.properties.size();
            std::vector<std::pair<std::string, std::string>> properties(node.properties.begin(), node.properties.end());
            std::sort(properties.begin(), properties.end());
            for (const auto& [key, value] : properties)
                serialized << " " << std::quoted(key) << " " << std::quoted(value);
            serialized << "\n";
        }
        text = serialized.str();
        return true;
    }
} // namespace Spark
