/**
 * @file SceneGraph2D.h
 * @brief Retained 2D scene graph with granular update flags
 * @author Spark Engine Team
 * @date 2025
 *
 * Inspired by ThorVG's Paint/Scene hierarchy with RenderUpdateFlags.
 * Only reprocesses changed nodes each frame. Supports transform hierarchy,
 * color tinting, opacity, blend modes, and visibility culling.
 *
 * @see SpriteBatch.h, Physics2D.h
 */

#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Spark::Engine2D
{

    // =========================================================================
    // Update Flags
    // =========================================================================

    enum class UpdateFlag : uint32_t
    {
        None = 0,
        Transform = 1 << 0,
        Color = 1 << 1,
        Opacity = 1 << 2,
        Blend = 1 << 3,
        Visibility = 1 << 4,
        Image = 1 << 5,
        Path = 1 << 6,
        Children = 1 << 7,
        All = 0xFF
    };

    inline UpdateFlag operator|(UpdateFlag a, UpdateFlag b)
    {
        return static_cast<UpdateFlag>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
    }
    inline UpdateFlag& operator|=(UpdateFlag& a, UpdateFlag b)
    {
        a = a | b;
        return a;
    }
    inline bool HasFlag(UpdateFlag flags, UpdateFlag test)
    {
        return (static_cast<uint32_t>(flags) & static_cast<uint32_t>(test)) != 0;
    }

    // =========================================================================
    // Transform2D
    // =========================================================================

    struct Transform2D
    {
        float x = 0.0f, y = 0.0f;
        float scaleX = 1.0f, scaleY = 1.0f;
        float rotation = 0.0f; ///< Radians
        float pivotX = 0.0f, pivotY = 0.0f;

        // Cached world transform (computed from parent chain)
        float worldX = 0.0f, worldY = 0.0f;
        float worldScaleX = 1.0f, worldScaleY = 1.0f;
        float worldRotation = 0.0f;
    };

    // =========================================================================
    // Node2D
    // =========================================================================

    enum class BlendMode2D : uint8_t
    {
        Alpha,
        Additive,
        Multiply,
        PremultipliedAlpha
    };

    /**
     * @brief Base node in the 2D scene graph
     *
     * Supports transform hierarchy, color tinting, and granular dirty tracking.
     * Override OnRender() to implement custom drawing.
     */
    class Node2D
    {
      public:
        Node2D(const std::string& name = "") : m_name(name), m_dirty(UpdateFlag::All) {}
        virtual ~Node2D() = default;

        // --- Transform ---
        void SetPosition(float x, float y)
        {
            m_transform.x = x;
            m_transform.y = y;
            MarkDirty(UpdateFlag::Transform);
        }
        void SetScale(float sx, float sy)
        {
            m_transform.scaleX = sx;
            m_transform.scaleY = sy;
            MarkDirty(UpdateFlag::Transform);
        }
        void SetRotation(float radians)
        {
            m_transform.rotation = radians;
            MarkDirty(UpdateFlag::Transform);
        }

        // --- Appearance ---
        void SetColor(float r, float g, float b, float a = 1.0f)
        {
            m_colorR = r;
            m_colorG = g;
            m_colorB = b;
            m_colorA = a;
            MarkDirty(UpdateFlag::Color);
        }
        void SetOpacity(float opacity)
        {
            m_opacity = opacity;
            MarkDirty(UpdateFlag::Opacity);
        }
        void SetBlendMode(BlendMode2D mode)
        {
            m_blendMode = mode;
            MarkDirty(UpdateFlag::Blend);
        }
        void SetVisible(bool visible)
        {
            m_visible = visible;
            MarkDirty(UpdateFlag::Visibility);
        }
        void SetZOrder(int z)
        {
            m_zOrder = z;
            MarkDirty(UpdateFlag::Transform);
        }

        // --- Hierarchy ---
        Node2D* AddChild(std::unique_ptr<Node2D> child)
        {
            child->m_parent = this;
            Node2D* raw = child.get();
            m_children.push_back(std::move(child));
            MarkDirty(UpdateFlag::Children);
            return raw;
        }

        void RemoveChild(const std::string& name)
        {
            m_children.erase(
                std::remove_if(m_children.begin(), m_children.end(), [&](const auto& c) { return c->m_name == name; }),
                m_children.end());
            MarkDirty(UpdateFlag::Children);
        }

        Node2D* FindChild(const std::string& name) const
        {
            for (const auto& c : m_children)
            {
                if (c->m_name == name)
                    return c.get();
                Node2D* found = c->FindChild(name);
                if (found)
                    return found;
            }
            return nullptr;
        }

        // --- Update ---
        void Update()
        {
            if (m_dirty == UpdateFlag::None && !HasDirtyChildren())
                return;

            if (HasFlag(m_dirty, UpdateFlag::Transform))
            {
                UpdateWorldTransform();
            }

            OnUpdate();
            m_dirty = UpdateFlag::None;

            for (auto& child : m_children)
                child->Update();
        }

        // --- Queries ---
        const std::string& GetName() const { return m_name; }
        bool IsVisible() const { return m_visible; }
        bool IsDirty() const { return m_dirty != UpdateFlag::None; }
        UpdateFlag GetDirtyFlags() const { return m_dirty; }
        int GetZOrder() const { return m_zOrder; }
        const Transform2D& GetTransform() const { return m_transform; }
        const std::vector<std::unique_ptr<Node2D>>& GetChildren() const { return m_children; }

        /**
         * @brief Check if composition is needed (expensive render path)
         *
         * Composition is required when the node needs an intermediate render
         * target (e.g., for masking or non-trivial blending). Skip when the
         * node is simple: full opacity, alpha blend, no masks.
         */
        bool NeedsComposition() const { return m_opacity < 0.99f || m_blendMode != BlendMode2D::Alpha || m_hasMask; }

      protected:
        virtual void OnUpdate() {}

        void MarkDirty(UpdateFlag flags)
        {
            m_dirty |= flags;
            // Propagate transform changes to children
            if (HasFlag(flags, UpdateFlag::Transform))
            {
                for (auto& child : m_children)
                    child->MarkDirty(UpdateFlag::Transform);
            }
        }

      private:
        void UpdateWorldTransform()
        {
            if (m_parent)
            {
                const auto& pt = m_parent->m_transform;
                m_transform.worldX = pt.worldX + m_transform.x * pt.worldScaleX;
                m_transform.worldY = pt.worldY + m_transform.y * pt.worldScaleY;
                m_transform.worldScaleX = pt.worldScaleX * m_transform.scaleX;
                m_transform.worldScaleY = pt.worldScaleY * m_transform.scaleY;
                m_transform.worldRotation = pt.worldRotation + m_transform.rotation;
            }
            else
            {
                m_transform.worldX = m_transform.x;
                m_transform.worldY = m_transform.y;
                m_transform.worldScaleX = m_transform.scaleX;
                m_transform.worldScaleY = m_transform.scaleY;
                m_transform.worldRotation = m_transform.rotation;
            }
        }

        bool HasDirtyChildren() const
        {
            for (const auto& c : m_children)
            {
                if (c->IsDirty() || c->HasDirtyChildren())
                    return true;
            }
            return false;
        }

        std::string m_name;
        Transform2D m_transform;
        float m_colorR = 1.0f, m_colorG = 1.0f, m_colorB = 1.0f, m_colorA = 1.0f;
        float m_opacity = 1.0f;
        BlendMode2D m_blendMode = BlendMode2D::Alpha;
        bool m_visible = true;
        bool m_hasMask = false;
        int m_zOrder = 0;
        UpdateFlag m_dirty = UpdateFlag::All;
        Node2D* m_parent = nullptr;
        std::vector<std::unique_ptr<Node2D>> m_children;
    };

    // =========================================================================
    // Specialized Nodes
    // =========================================================================

    class SpriteNode : public Node2D
    {
      public:
        SpriteNode(const std::string& name = "") : Node2D(name) {}
        void SetTexture(uint32_t textureId)
        {
            m_textureId = textureId;
            MarkDirty(UpdateFlag::Image);
        }
        void SetSourceRect(float x, float y, float w, float h)
        {
            m_srcX = x;
            m_srcY = y;
            m_srcW = w;
            m_srcH = h;
            MarkDirty(UpdateFlag::Image);
        }
        uint32_t GetTextureId() const { return m_textureId; }

      private:
        uint32_t m_textureId = 0;
        float m_srcX = 0, m_srcY = 0, m_srcW = 1, m_srcH = 1;
    };

    class TextNode : public Node2D
    {
      public:
        TextNode(const std::string& name = "", const std::string& text = "") : Node2D(name), m_text(text) {}
        void SetText(const std::string& text)
        {
            if (m_text != text)
            {
                m_text = text;
                MarkDirty(UpdateFlag::Path);
            }
        }
        const std::string& GetText() const { return m_text; }
        void SetFontSize(float size)
        {
            m_fontSize = size;
            MarkDirty(UpdateFlag::Path);
        }

      private:
        std::string m_text;
        float m_fontSize = 16.0f;
    };

} // namespace Spark::Engine2D
