/**
 * @file CSGSystem.h
 * @brief Constructive Solid Geometry (CSG) system for level greyboxing
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides boolean mesh operations (union, subtract, intersect) on primitive
 * shapes for rapid level prototyping and greyboxing. Outputs vertex/index
 * buffers compatible with the engine's Mesh class.
 *
 * ## Usage
 * @code
 *   auto& csg = Spark::LevelDesign::CSGSystem::GetInstance();
 *   csg.Initialize();
 *
 *   // Create brushes
 *   auto room = csg.CreateBrush(BrushShape::Box, {10, 3, 10});
 *   auto doorway = csg.CreateBrush(BrushShape::Box, {1.2f, 2.5f, 0.5f});
 *   doorway.transform.position = {5, 0, 0};
 *
 *   // Boolean subtract to carve a doorway
 *   auto result = csg.Subtract(room, doorway);
 *   auto mesh = csg.GenerateMesh(result);
 * @endcode
 */

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <string>
#include <unordered_map>
#include <vector>

namespace Spark::LevelDesign
{

    /** @brief 3D vector for CSG operations */
    struct CSGVec3
    {
        float x = 0.0f, y = 0.0f, z = 0.0f;

        CSGVec3 operator+(const CSGVec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
        CSGVec3 operator-(const CSGVec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
        CSGVec3 operator*(float s) const { return {x * s, y * s, z * s}; }
        float Dot(const CSGVec3& o) const { return x * o.x + y * o.y + z * o.z; }
        CSGVec3 Cross(const CSGVec3& o) const { return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x}; }
        float Length() const { return std::sqrt(x * x + y * y + z * z); }
        CSGVec3 Normalized() const
        {
            float len = Length();
            return (len > 1e-6f) ? CSGVec3{x / len, y / len, z / len} : CSGVec3{0, 0, 0};
        }
    };

    /** @brief Simple transform for CSG brushes */
    struct CSGTransform
    {
        CSGVec3 position{0, 0, 0}; ///< World position
        CSGVec3 rotation{0, 0, 0}; ///< Euler angles in degrees
        CSGVec3 scale{1, 1, 1};    ///< Non-uniform scale
    };

    /** @brief Primitive shape types for CSG brushes */
    enum class BrushShape
    {
        Box,      ///< Axis-aligned box
        Cylinder, ///< Vertical cylinder with configurable segments
        Sphere,   ///< Tessellated sphere
        Wedge,    ///< Right-angle wedge (ramp)
        Cone      ///< Vertical cone
    };

    /** @brief Boolean operation types */
    enum class CSGOperation
    {
        Additive,    ///< Union — adds volume
        Subtractive, ///< Subtract — carves volume
        Intersect    ///< Intersection — keeps only overlap
    };

    /** @brief A single vertex in a CSG mesh */
    struct CSGVertex
    {
        CSGVec3 position; ///< World-space position
        CSGVec3 normal;   ///< Surface normal
        float u = 0.0f;   ///< Texture coordinate U
        float v = 0.0f;   ///< Texture coordinate V
    };

    /** @brief A polygon face in a CSG solid */
    struct CSGFace
    {
        std::vector<CSGVertex> vertices; ///< Vertices of this face (convex polygon)
        CSGVec3 normal;                  ///< Face normal
    };

    /** @brief A solid composed of convex faces, the core CSG data structure */
    struct CSGSolid
    {
        std::vector<CSGFace> faces;                      ///< All faces of the solid
        CSGTransform transform;                          ///< World transform
        BrushShape sourceShape = BrushShape::Box;        ///< Original primitive shape
        CSGOperation operation = CSGOperation::Additive; ///< Boolean operation
    };

    /** @brief Output mesh ready for rendering */
    struct CSGMesh
    {
        std::vector<CSGVertex> vertices; ///< Vertex buffer
        std::vector<uint32_t> indices;   ///< Index buffer (triangles)
        uint32_t triangleCount = 0;      ///< Number of triangles
    };

    /**
     * @brief BSP plane for spatial partitioning during boolean operations
     */
    struct CSGPlane
    {
        CSGVec3 normal;        ///< Plane normal
        float distance = 0.0f; ///< Distance from origin

        /** @brief Classify a point relative to this plane */
        float Classify(const CSGVec3& point) const { return normal.Dot(point) - distance; }
    };

    /**
     * @brief Constructive Solid Geometry system for level greyboxing
     *
     * Creates primitive brushes and performs boolean operations to build
     * level geometry. Outputs triangle meshes suitable for rendering and
     * collision.
     */
    class CSGSystem
    {
      public:
        static CSGSystem& GetInstance()
        {
            static CSGSystem instance;
            return instance;
        }

        /** @brief Initialize the CSG system */
        void Initialize()
        {
            m_brushes.clear();
            m_nextBrushId = 1;
            m_cylinderSegments = 16;
            m_sphereRings = 8;
            m_sphereSegments = 16;
            m_initialized = true;
        }

        /** @brief Shutdown and clear all brushes */
        void Shutdown()
        {
            m_brushes.clear();
            m_initialized = false;
        }

        /**
         * @brief Create a new brush from a primitive shape
         * @param shape Primitive shape type
         * @param size Dimensions (width, height, depth for box; radius, height, 0 for cylinder)
         * @return Brush ID for referencing
         */
        uint32_t CreateBrush(BrushShape shape, const CSGVec3& size)
        {
            if (!m_initialized)
                return 0;

            CSGSolid solid;
            solid.sourceShape = shape;

            switch (shape)
            {
            case BrushShape::Box:
                solid = GenerateBox(size);
                break;
            case BrushShape::Cylinder:
                solid = GenerateCylinder(size.x, size.y, m_cylinderSegments);
                break;
            case BrushShape::Sphere:
                solid = GenerateSphere(size.x, m_sphereRings, m_sphereSegments);
                break;
            case BrushShape::Wedge:
                solid = GenerateWedge(size);
                break;
            case BrushShape::Cone:
                solid = GenerateCone(size.x, size.y, m_cylinderSegments);
                break;
            }

            solid.sourceShape = shape;
            uint32_t id = m_nextBrushId++;
            m_brushes[id] = std::move(solid);
            return id;
        }

        /**
         * @brief Set the transform of a brush
         * @param brushId Brush to transform
         * @param transform New transform
         */
        void SetBrushTransform(uint32_t brushId, const CSGTransform& transform)
        {
            auto it = m_brushes.find(brushId);
            if (it != m_brushes.end())
                it->second.transform = transform;
        }

        /**
         * @brief Set the operation type for a brush
         * @param brushId Brush to modify
         * @param op Operation type
         */
        void SetBrushOperation(uint32_t brushId, CSGOperation op)
        {
            auto it = m_brushes.find(brushId);
            if (it != m_brushes.end())
                it->second.operation = op;
        }

        /**
         * @brief Perform boolean subtraction: result = A - B
         * @param solidA The base solid
         * @param solidB The solid to subtract
         * @return Resulting solid after subtraction
         */
        CSGSolid Subtract(const CSGSolid& solidA, const CSGSolid& solidB) const
        {
            // Clip A's faces to the inside of B's planes (inverted)
            CSGSolid result;
            result.faces = ClipFaces(solidA.faces, solidB.faces, true);
            // Add B's faces clipped to A (inverted normals)
            auto bClipped = ClipFaces(solidB.faces, solidA.faces, false);
            for (auto& face : bClipped)
            {
                face.normal = face.normal * -1.0f;
                for (auto& vert : face.vertices)
                    vert.normal = vert.normal * -1.0f;
            }
            result.faces.insert(result.faces.end(), bClipped.begin(), bClipped.end());
            return result;
        }

        /**
         * @brief Perform boolean union: result = A + B
         * @param solidA First solid
         * @param solidB Second solid
         * @return Combined solid
         */
        CSGSolid Union(const CSGSolid& solidA, const CSGSolid& solidB) const
        {
            CSGSolid result;
            // Keep A's faces outside B
            auto aOutside = ClipFaces(solidA.faces, solidB.faces, true);
            // Keep B's faces outside A
            auto bOutside = ClipFaces(solidB.faces, solidA.faces, true);
            result.faces = std::move(aOutside);
            result.faces.insert(result.faces.end(), bOutside.begin(), bOutside.end());
            return result;
        }

        /**
         * @brief Perform boolean intersection: result = A ∩ B
         * @param solidA First solid
         * @param solidB Second solid
         * @return Intersection of both solids
         */
        CSGSolid Intersect(const CSGSolid& solidA, const CSGSolid& solidB) const
        {
            CSGSolid result;
            // Keep A's faces inside B
            auto aInside = ClipFaces(solidA.faces, solidB.faces, false);
            // Keep B's faces inside A
            auto bInside = ClipFaces(solidB.faces, solidA.faces, false);
            result.faces = std::move(aInside);
            result.faces.insert(result.faces.end(), bInside.begin(), bInside.end());
            return result;
        }

        /**
         * @brief Generate a renderable mesh from a CSG solid
         * @param solid The solid to triangulate
         * @return Triangle mesh with vertices and indices
         */
        CSGMesh GenerateMesh(const CSGSolid& solid) const
        {
            CSGMesh mesh;
            uint32_t baseVertex = 0;

            for (const auto& face : solid.faces)
            {
                if (face.vertices.size() < 3)
                    continue;

                // Fan triangulation for convex polygons
                for (size_t i = 1; i + 1 < face.vertices.size(); ++i)
                {
                    mesh.vertices.push_back(face.vertices[0]);
                    mesh.vertices.push_back(face.vertices[i]);
                    mesh.vertices.push_back(face.vertices[i + 1]);

                    mesh.indices.push_back(baseVertex);
                    mesh.indices.push_back(baseVertex + 1);
                    mesh.indices.push_back(baseVertex + 2);
                    baseVertex += 3;
                }
            }

            mesh.triangleCount = static_cast<uint32_t>(mesh.indices.size() / 3);
            return mesh;
        }

        /**
         * @brief Build the combined mesh from all registered brushes
         * @return Final combined mesh after all boolean operations
         */
        CSGMesh BuildAll() const
        {
            if (m_brushes.empty())
                return {};

            // Find the first additive brush as the base
            const CSGSolid* base = nullptr;
            for (const auto& [id, solid] : m_brushes)
            {
                if (solid.operation == CSGOperation::Additive)
                {
                    base = &solid;
                    break;
                }
            }

            if (!base)
                return {};

            CSGSolid result = *base;

            for (const auto& [id, solid] : m_brushes)
            {
                if (&solid == base)
                    continue;

                switch (solid.operation)
                {
                case CSGOperation::Additive:
                    result = Union(result, solid);
                    break;
                case CSGOperation::Subtractive:
                    result = Subtract(result, solid);
                    break;
                case CSGOperation::Intersect:
                    result = Intersect(result, solid);
                    break;
                }
            }

            return GenerateMesh(result);
        }

        /** @brief Remove a brush */
        void RemoveBrush(uint32_t brushId) { m_brushes.erase(brushId); }

        /** @brief Clear all brushes */
        void ClearAll() { m_brushes.clear(); }

        /** @brief Get the number of registered brushes */
        size_t GetBrushCount() const { return m_brushes.size(); }

        /** @brief Get a brush solid by ID */
        const CSGSolid* GetBrush(uint32_t id) const
        {
            auto it = m_brushes.find(id);
            return (it != m_brushes.end()) ? &it->second : nullptr;
        }

        /** @brief Set cylinder tessellation segments */
        void SetCylinderSegments(uint32_t segments) { m_cylinderSegments = std::max(3u, segments); }

        /** @brief Set sphere tessellation detail */
        void SetSphereTessellation(uint32_t rings, uint32_t segments)
        {
            m_sphereRings = std::max(2u, rings);
            m_sphereSegments = std::max(3u, segments);
        }

        /** @brief Get console-friendly status string */
        std::string Console_GetStatus() const
        {
            if (!m_initialized)
                return "[CSG] Not initialized";
            return "[CSG] Brushes: " + std::to_string(m_brushes.size()) +
                   " | CylinderSegs: " + std::to_string(m_cylinderSegments) +
                   " | SphereRings: " + std::to_string(m_sphereRings);
        }

      private:
        CSGSystem() = default;

        static constexpr float PI = 3.14159265358979323846f;

        /** @brief Generate a box solid centered at origin */
        CSGSolid GenerateBox(const CSGVec3& size) const
        {
            float hx = size.x * 0.5f, hy = size.y * 0.5f, hz = size.z * 0.5f;
            CSGSolid solid;

            // 6 faces: +X, -X, +Y, -Y, +Z, -Z
            struct FaceDef
            {
                CSGVec3 normal;
                std::array<CSGVec3, 4> corners;
            };
            FaceDef faces[] = {
                {{0, 0, 1}, {{{-hx, -hy, hz}, {hx, -hy, hz}, {hx, hy, hz}, {-hx, hy, hz}}}},
                {{0, 0, -1}, {{{hx, -hy, -hz}, {-hx, -hy, -hz}, {-hx, hy, -hz}, {hx, hy, -hz}}}},
                {{0, 1, 0}, {{{-hx, hy, hz}, {hx, hy, hz}, {hx, hy, -hz}, {-hx, hy, -hz}}}},
                {{0, -1, 0}, {{{-hx, -hy, -hz}, {hx, -hy, -hz}, {hx, -hy, hz}, {-hx, -hy, hz}}}},
                {{1, 0, 0}, {{{hx, -hy, hz}, {hx, -hy, -hz}, {hx, hy, -hz}, {hx, hy, hz}}}},
                {{-1, 0, 0}, {{{-hx, -hy, -hz}, {-hx, -hy, hz}, {-hx, hy, hz}, {-hx, hy, -hz}}}},
            };

            for (const auto& fd : faces)
            {
                CSGFace face;
                face.normal = fd.normal;
                for (const auto& corner : fd.corners)
                {
                    CSGVertex vert;
                    vert.position = corner;
                    vert.normal = fd.normal;
                    face.vertices.push_back(vert);
                }
                solid.faces.push_back(std::move(face));
            }
            return solid;
        }

        /** @brief Generate a cylinder solid centered at origin */
        CSGSolid GenerateCylinder(float radius, float height, uint32_t segments) const
        {
            CSGSolid solid;
            float hy = height * 0.5f;
            float step = 2.0f * PI / static_cast<float>(segments);

            // Side faces (quads)
            for (uint32_t i = 0; i < segments; ++i)
            {
                float a0 = step * static_cast<float>(i);
                float a1 = step * static_cast<float>(i + 1);
                float c0 = std::cos(a0), s0 = std::sin(a0);
                float c1 = std::cos(a1), s1 = std::sin(a1);

                CSGVec3 n = CSGVec3{c0 + c1, 0, s0 + s1}.Normalized();
                CSGFace face;
                face.normal = n;
                face.vertices.push_back({{radius * c0, -hy, radius * s0}, n, 0, 0});
                face.vertices.push_back({{radius * c1, -hy, radius * s1}, n, 0, 0});
                face.vertices.push_back({{radius * c1, hy, radius * s1}, n, 0, 0});
                face.vertices.push_back({{radius * c0, hy, radius * s0}, n, 0, 0});
                solid.faces.push_back(std::move(face));
            }

            // Top and bottom caps
            CSGFace top, bottom;
            top.normal = {0, 1, 0};
            bottom.normal = {0, -1, 0};
            for (uint32_t i = 0; i < segments; ++i)
            {
                float a = step * static_cast<float>(i);
                float c = std::cos(a), s = std::sin(a);
                top.vertices.push_back({{radius * c, hy, radius * s}, {0, 1, 0}, 0, 0});
                // Bottom in reverse winding
                float aRev = step * static_cast<float>(segments - 1 - i);
                float cR = std::cos(aRev), sR = std::sin(aRev);
                bottom.vertices.push_back({{radius * cR, -hy, radius * sR}, {0, -1, 0}, 0, 0});
            }
            solid.faces.push_back(std::move(top));
            solid.faces.push_back(std::move(bottom));

            return solid;
        }

        /** @brief Generate a sphere solid centered at origin */
        CSGSolid GenerateSphere(float radius, uint32_t rings, uint32_t segments) const
        {
            CSGSolid solid;
            float ringStep = PI / static_cast<float>(rings);
            float segStep = 2.0f * PI / static_cast<float>(segments);

            for (uint32_t r = 0; r < rings; ++r)
            {
                float phi0 = ringStep * static_cast<float>(r);
                float phi1 = ringStep * static_cast<float>(r + 1);

                for (uint32_t s = 0; s < segments; ++s)
                {
                    float theta0 = segStep * static_cast<float>(s);
                    float theta1 = segStep * static_cast<float>(s + 1);

                    auto spherePoint = [radius](float phi, float theta) -> CSGVec3
                    {
                        return {radius * std::sin(phi) * std::cos(theta), radius * std::cos(phi),
                                radius * std::sin(phi) * std::sin(theta)};
                    };

                    CSGFace face;
                    CSGVec3 p0 = spherePoint(phi0, theta0);
                    CSGVec3 p1 = spherePoint(phi0, theta1);
                    CSGVec3 p2 = spherePoint(phi1, theta1);
                    CSGVec3 p3 = spherePoint(phi1, theta0);

                    face.vertices.push_back({p0, p0.Normalized(), 0, 0});
                    if (r > 0)
                        face.vertices.push_back({p1, p1.Normalized(), 0, 0});
                    face.vertices.push_back({p2, p2.Normalized(), 0, 0});
                    if (r + 1 < rings)
                        face.vertices.push_back({p3, p3.Normalized(), 0, 0});

                    face.normal = face.vertices[0].normal;
                    if (face.vertices.size() >= 3)
                        solid.faces.push_back(std::move(face));
                }
            }
            return solid;
        }

        /** @brief Generate a wedge (ramp) solid */
        CSGSolid GenerateWedge(const CSGVec3& size) const
        {
            float hx = size.x * 0.5f, hy = size.y * 0.5f, hz = size.z * 0.5f;
            CSGSolid solid;

            // Bottom face
            CSGFace bottom;
            bottom.normal = {0, -1, 0};
            bottom.vertices.push_back({{-hx, -hy, -hz}, {0, -1, 0}, 0, 0});
            bottom.vertices.push_back({{hx, -hy, -hz}, {0, -1, 0}, 0, 0});
            bottom.vertices.push_back({{hx, -hy, hz}, {0, -1, 0}, 0, 0});
            bottom.vertices.push_back({{-hx, -hy, hz}, {0, -1, 0}, 0, 0});
            solid.faces.push_back(std::move(bottom));

            // Back face (vertical)
            CSGFace back;
            back.normal = {0, 0, -1};
            back.vertices.push_back({{hx, -hy, -hz}, {0, 0, -1}, 0, 0});
            back.vertices.push_back({{-hx, -hy, -hz}, {0, 0, -1}, 0, 0});
            back.vertices.push_back({{-hx, hy, -hz}, {0, 0, -1}, 0, 0});
            back.vertices.push_back({{hx, hy, -hz}, {0, 0, -1}, 0, 0});
            solid.faces.push_back(std::move(back));

            // Slope face
            CSGVec3 slopeNormal = CSGVec3{0, hz, hy}.Normalized();
            CSGFace slope;
            slope.normal = slopeNormal;
            slope.vertices.push_back({{-hx, -hy, hz}, slopeNormal, 0, 0});
            slope.vertices.push_back({{hx, -hy, hz}, slopeNormal, 0, 0});
            slope.vertices.push_back({{hx, hy, -hz}, slopeNormal, 0, 0});
            slope.vertices.push_back({{-hx, hy, -hz}, slopeNormal, 0, 0});
            solid.faces.push_back(std::move(slope));

            // Left triangle
            CSGFace left;
            left.normal = {-1, 0, 0};
            left.vertices.push_back({{-hx, -hy, hz}, {-1, 0, 0}, 0, 0});
            left.vertices.push_back({{-hx, hy, -hz}, {-1, 0, 0}, 0, 0});
            left.vertices.push_back({{-hx, -hy, -hz}, {-1, 0, 0}, 0, 0});
            solid.faces.push_back(std::move(left));

            // Right triangle
            CSGFace right;
            right.normal = {1, 0, 0};
            right.vertices.push_back({{hx, -hy, -hz}, {1, 0, 0}, 0, 0});
            right.vertices.push_back({{hx, hy, -hz}, {1, 0, 0}, 0, 0});
            right.vertices.push_back({{hx, -hy, hz}, {1, 0, 0}, 0, 0});
            solid.faces.push_back(std::move(right));

            return solid;
        }

        /** @brief Generate a cone solid */
        CSGSolid GenerateCone(float radius, float height, uint32_t segments) const
        {
            CSGSolid solid;
            float hy = height * 0.5f;
            float step = 2.0f * PI / static_cast<float>(segments);
            CSGVec3 apex = {0, hy, 0};

            // Side triangles
            for (uint32_t i = 0; i < segments; ++i)
            {
                float a0 = step * static_cast<float>(i);
                float a1 = step * static_cast<float>(i + 1);
                CSGVec3 b0 = {radius * std::cos(a0), -hy, radius * std::sin(a0)};
                CSGVec3 b1 = {radius * std::cos(a1), -hy, radius * std::sin(a1)};

                CSGVec3 edge1 = b0 - apex;
                CSGVec3 edge2 = b1 - apex;
                CSGVec3 n = edge2.Cross(edge1).Normalized();

                CSGFace face;
                face.normal = n;
                face.vertices.push_back({apex, n, 0, 0});
                face.vertices.push_back({b0, n, 0, 0});
                face.vertices.push_back({b1, n, 0, 0});
                solid.faces.push_back(std::move(face));
            }

            // Bottom cap
            CSGFace bottom;
            bottom.normal = {0, -1, 0};
            for (uint32_t i = segments; i > 0; --i)
            {
                float a = step * static_cast<float>(i - 1);
                bottom.vertices.push_back({{radius * std::cos(a), -hy, radius * std::sin(a)}, {0, -1, 0}, 0, 0});
            }
            solid.faces.push_back(std::move(bottom));

            return solid;
        }

        /**
         * @brief Clip faces of solid A against faces of solid B
         * @param facesA Faces to clip
         * @param facesB Faces defining the clipping volume
         * @param keepOutside If true, keep faces outside B; if false, keep inside
         * @return Clipped faces
         */
        std::vector<CSGFace> ClipFaces(const std::vector<CSGFace>& facesA, const std::vector<CSGFace>& facesB,
                                       bool keepOutside) const
        {
            std::vector<CSGFace> result = facesA;

            for (const auto& clipFace : facesB)
            {
                if (clipFace.vertices.size() < 3)
                    continue;

                CSGPlane plane;
                plane.normal = clipFace.normal;
                plane.distance = clipFace.normal.Dot(clipFace.vertices[0].position);

                std::vector<CSGFace> clipped;
                for (const auto& face : result)
                {
                    auto [front, back] = SplitFace(face, plane);
                    if (keepOutside)
                    {
                        if (front.vertices.size() >= 3)
                            clipped.push_back(std::move(front));
                    }
                    else
                    {
                        if (back.vertices.size() >= 3)
                            clipped.push_back(std::move(back));
                    }
                }
                result = std::move(clipped);
            }

            return result;
        }

        /**
         * @brief Split a face by a plane into front and back portions
         */
        std::pair<CSGFace, CSGFace> SplitFace(const CSGFace& face, const CSGPlane& plane) const
        {
            constexpr float EPSILON = 1e-5f;
            CSGFace front, back;
            front.normal = face.normal;
            back.normal = face.normal;

            if (face.vertices.empty())
                return {front, back};

            for (size_t i = 0; i < face.vertices.size(); ++i)
            {
                const auto& current = face.vertices[i];
                const auto& next = face.vertices[(i + 1) % face.vertices.size()];
                float dc = plane.Classify(current.position);
                float dn = plane.Classify(next.position);

                if (dc >= -EPSILON)
                    front.vertices.push_back(current);
                if (dc <= EPSILON)
                    back.vertices.push_back(current);

                // If the edge crosses the plane, compute intersection
                if ((dc > EPSILON && dn < -EPSILON) || (dc < -EPSILON && dn > EPSILON))
                {
                    float t = dc / (dc - dn);
                    CSGVertex interp;
                    interp.position = current.position + (next.position - current.position) * t;
                    interp.normal = current.normal; // Simplified
                    interp.u = current.u + (next.u - current.u) * t;
                    interp.v = current.v + (next.v - current.v) * t;
                    front.vertices.push_back(interp);
                    back.vertices.push_back(interp);
                }
            }

            return {front, back};
        }

        bool m_initialized = false;
        uint32_t m_nextBrushId = 1;
        uint32_t m_cylinderSegments = 16;
        uint32_t m_sphereRings = 8;
        uint32_t m_sphereSegments = 16;
        std::unordered_map<uint32_t, CSGSolid> m_brushes;
    };

} // namespace Spark::LevelDesign
