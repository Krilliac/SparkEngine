/**
 * @file InspectorComponentRenderers_ReflectedConditional.cpp
 * @brief Reflection-driven inspector renderers using conditional field visibility
 *
 * Split from InspectorComponentRenderers_Reflected.cpp. Contains the
 * MakeFieldVisibleWhen helper and the renderers whose fields use
 * visibleWhenField conditions: RigidBody2D, Collider2D, Projectile,
 * Interaction.
 */

#include "InspectorComponentRenderers_ReflectedInternal.h"

namespace SparkEditor
{

    // ============================================================================
    // Batch 3 — Migrated using conditional field visibility
    // ============================================================================

    // Helper: create a field with visibility condition
    static Spark::FieldInfo MakeFieldVisibleWhen(const char* name, const char* fieldName, Spark::FieldType type,
                                                 size_t offset, size_t size, const char* ctrlField, int ctrlValue)
    {
        Spark::FieldInfo f = MakeField(name, fieldName, type, offset, size);
        f.visibleWhenField = ctrlField;
        f.visibleWhenValue = ctrlValue;
        return f;
    }

    // ============================================================================
    // Rigid Body 2D (migrated — mass visible only when bodyType==Dynamic)
    // ============================================================================

    void InspectorPanel::RenderRigidBody2DComponent()
    {
        bool headerOpen = ImGui::CollapsingHeader(ICON_FA_GLOBE " Rigid Body 2D");
        if (ImGui::BeginPopupContextItem("##RigidBody2DCtx"))
        {
            if (ImGui::MenuItem(ICON_FA_TRASH " Remove Component"))
                RemoveComponent(ComponentType::RIGID_BODY_2D);
            ImGui::EndPopup();
        }
        if (headerOpen)
        {
            ImGui::Indent(4);
            Component* comp = FindComponent(m_scene, m_inspectedObjectID, ComponentType::RIGID_BODY_2D);
            auto* d = comp ? comp->GetData<RigidBody2DData>() : nullptr;
            if (d)
            {
                Spark::FieldInfo btField =
                    FIELD_ENUM(RigidBody2DData, bodyType, "Body Type", "Static", "Kinematic", "Dynamic");
                Spark::FieldInfo massField = FIELD_FLOAT(RigidBody2DData, mass, "Mass");
                massField.visibleWhenField = "bodyType";
                massField.visibleWhenValue = 2; // Dynamic

                RigidBody2DData oldSnapshot = *d;
                const std::vector<Spark::FieldInfo> fields = {
                    btField,
                    massField,
                    FIELD_FLOAT(RigidBody2DData, gravityScale, "Gravity Scale"),
                    FIELD_FLOAT(RigidBody2DData, linearDamping, "Linear Damping"),
                    FIELD_FLOAT(RigidBody2DData, angularDamping, "Angular Damping"),
                    FIELD_FLOAT(RigidBody2DData, friction, "Friction"),
                    FIELD_FLOAT(RigidBody2DData, restitution, "Restitution"),
                    FIELD_BOOL(RigidBody2DData, fixedRotation, "Fixed Rotation"),
                    FIELD_BOOL(RigidBody2DData, isBullet, "Bullet (CCD)"),
                };
                RenderReflectedFields(d, fields);
                if (std::memcmp(&oldSnapshot, d, sizeof(RigidBody2DData)) != 0)
                {
                    RigidBody2DData newSnapshot = *d;
                    SceneFile* cs = m_scene;
                    ObjectID ci = m_inspectedObjectID;
                    auto& history = Spark::Editor::CommandHistory::GetInstance();
                    history.Execute(std::make_unique<Spark::Editor::LambdaCommand>(
                        [cs, ci, newSnapshot]()
                        {
                            Component* c = FindComponent(cs, ci, ComponentType::RIGID_BODY_2D);
                            if (auto* p = c ? c->GetData<RigidBody2DData>() : nullptr)
                                *p = newSnapshot;
                        },
                        [cs, ci, oldSnapshot]()
                        {
                            Component* c = FindComponent(cs, ci, ComponentType::RIGID_BODY_2D);
                            if (auto* p = c ? c->GetData<RigidBody2DData>() : nullptr)
                                *p = oldSnapshot;
                        },
                        "Rigid Body 2D Change"));
                }
            }
            else
            {
                ImGui::TextDisabled("(Component data unavailable)");
            }
            ImGui::Unindent(4);
        }
    }

    // ============================================================================
    // Collider 2D (migrated — halfExtents for Box, height for Capsule, radius always)
    // ============================================================================

    void InspectorPanel::RenderCollider2DComponent()
    {
        bool headerOpen = ImGui::CollapsingHeader(ICON_FA_VECTOR_SQUARE " Collider 2D");
        if (ImGui::BeginPopupContextItem("##Collider2DCtx"))
        {
            if (ImGui::MenuItem(ICON_FA_TRASH " Remove Component"))
                RemoveComponent(ComponentType::COLLIDER_2D);
            ImGui::EndPopup();
        }
        if (headerOpen)
        {
            ImGui::Indent(4);
            Component* comp = FindComponent(m_scene, m_inspectedObjectID, ComponentType::COLLIDER_2D);
            auto* d = comp ? comp->GetData<Collider2DData>() : nullptr;
            if (d)
            {
                Spark::FieldInfo heField = FIELD_VEC2(Collider2DData, halfExtents, "Half Extents");
                heField.visibleWhenField = "shape";
                heField.visibleWhenValue = 0; // Box
                Spark::FieldInfo htField = FIELD_FLOAT(Collider2DData, height, "Height");
                htField.visibleWhenField = "shape";
                htField.visibleWhenValue = 2; // Capsule

                Collider2DData oldSnapshot = *d;
                const std::vector<Spark::FieldInfo> fields = {
                    FIELD_ENUM(Collider2DData, shape, "Shape", "Box", "Circle", "Capsule", "Polygon"),
                    FIELD_VEC2(Collider2DData, offset, "Offset"),
                    heField,
                    FIELD_FLOAT(Collider2DData, radius, "Radius"),
                    htField,
                    FIELD_BOOL(Collider2DData, isTrigger, "Is Trigger"),
                };
                RenderReflectedFields(d, fields);
                if (std::memcmp(&oldSnapshot, d, sizeof(Collider2DData)) != 0)
                {
                    Collider2DData newSnapshot = *d;
                    SceneFile* cs = m_scene;
                    ObjectID ci = m_inspectedObjectID;
                    auto& history = Spark::Editor::CommandHistory::GetInstance();
                    history.Execute(std::make_unique<Spark::Editor::LambdaCommand>(
                        [cs, ci, newSnapshot]()
                        {
                            Component* c = FindComponent(cs, ci, ComponentType::COLLIDER_2D);
                            if (auto* p = c ? c->GetData<Collider2DData>() : nullptr)
                                *p = newSnapshot;
                        },
                        [cs, ci, oldSnapshot]()
                        {
                            Component* c = FindComponent(cs, ci, ComponentType::COLLIDER_2D);
                            if (auto* p = c ? c->GetData<Collider2DData>() : nullptr)
                                *p = oldSnapshot;
                        },
                        "Collider 2D Change"));
                }
            }
            else
            {
                ImGui::TextDisabled("(Component data unavailable)");
            }
            ImGui::Unindent(4);
        }
    }

    // ============================================================================
    // Projectile (migrated — bounces/pierces conditional on impactBehavior)
    // ============================================================================

    void InspectorPanel::RenderProjectileComponent()
    {
        bool headerOpen = ImGui::CollapsingHeader(ICON_FA_CROSSHAIRS " Projectile");
        if (ImGui::BeginPopupContextItem("##ProjectileCtx"))
        {
            if (ImGui::MenuItem(ICON_FA_TRASH " Remove Component"))
                RemoveComponent(ComponentType::PROJECTILE);
            ImGui::EndPopup();
        }
        if (headerOpen)
        {
            ImGui::Indent(4);
            Component* comp = FindComponent(m_scene, m_inspectedObjectID, ComponentType::PROJECTILE);
            auto* d = comp ? comp->GetData<ProjectileData>() : nullptr;
            if (d)
            {
                Spark::FieldInfo bouncesField = FIELD_INT(ProjectileData, bouncesRemaining, "Bounces");
                bouncesField.visibleWhenField = "impactBehavior";
                bouncesField.visibleWhenValue = 1; // Bounce
                Spark::FieldInfo piercesField = FIELD_INT(ProjectileData, piercesRemaining, "Pierces");
                piercesField.visibleWhenField = "impactBehavior";
                piercesField.visibleWhenValue = 2; // Pierce

                ProjectileData oldSnapshot = *d;
                const std::vector<Spark::FieldInfo> fields = {
                    FIELD_ENUM(ProjectileData, movementType, "Movement", "Hitscan", "Ballistic"),
                    FIELD_ENUM(ProjectileData, impactBehavior, "On Impact", "Destroy", "Bounce", "Pierce", "Stick"),
                    FIELD_FLOAT(ProjectileData, speed, "Speed"),
                    FIELD_FLOAT(ProjectileData, damage, "Damage"),
                    FIELD_FLOAT(ProjectileData, gravityScale, "Gravity Scale"),
                    FIELD_FLOAT(ProjectileData, explosionRadius, "Explosion Radius"),
                    FIELD_FLOAT(ProjectileData, maxRange, "Max Range"),
                    FIELD_FLOAT(ProjectileData, maxLifetime, "Max Lifetime"),
                    bouncesField,
                    piercesField,
                };
                RenderReflectedFields(d, fields);
                if (std::memcmp(&oldSnapshot, d, sizeof(ProjectileData)) != 0)
                {
                    ProjectileData newSnapshot = *d;
                    SceneFile* cs = m_scene;
                    ObjectID ci = m_inspectedObjectID;
                    auto& history = Spark::Editor::CommandHistory::GetInstance();
                    history.Execute(std::make_unique<Spark::Editor::LambdaCommand>(
                        [cs, ci, newSnapshot]()
                        {
                            Component* c = FindComponent(cs, ci, ComponentType::PROJECTILE);
                            if (auto* p = c ? c->GetData<ProjectileData>() : nullptr)
                                *p = newSnapshot;
                        },
                        [cs, ci, oldSnapshot]()
                        {
                            Component* c = FindComponent(cs, ci, ComponentType::PROJECTILE);
                            if (auto* p = c ? c->GetData<ProjectileData>() : nullptr)
                                *p = oldSnapshot;
                        },
                        "Projectile Change"));
                }
            }
            else
            {
                ImGui::TextDisabled("(Component data unavailable)");
            }
            ImGui::Unindent(4);
        }
    }

    // ============================================================================
    // Interaction (migrated — holdDuration conditional on interactionType==Hold)
    // ============================================================================

    void InspectorPanel::RenderInteractionComponent()
    {
        bool headerOpen = ImGui::CollapsingHeader(ICON_FA_HAND_POINTER " Interaction");
        if (ImGui::BeginPopupContextItem("##InteractionCtx"))
        {
            if (ImGui::MenuItem(ICON_FA_TRASH " Remove Component"))
                RemoveComponent(ComponentType::INTERACTION);
            ImGui::EndPopup();
        }
        if (headerOpen)
        {
            ImGui::Indent(4);
            Component* comp = FindComponent(m_scene, m_inspectedObjectID, ComponentType::INTERACTION);
            auto* d = comp ? comp->GetData<InteractionData>() : nullptr;
            if (d)
            {
                Spark::FieldInfo holdField = FIELD_FLOAT(InteractionData, holdDuration, "Hold Duration");
                holdField.visibleWhenField = "interactionType";
                holdField.visibleWhenValue = 2; // Hold

                InteractionData oldSnapshot = *d;
                const std::vector<Spark::FieldInfo> fields = {
                    FIELD_ENUM(InteractionData, interactionType, "Type", "Use", "Pickup", "Hold", "Toggle"),
                    FIELD_STRING(InteractionData, displayName, "Display Name"),
                    FIELD_STRING(InteractionData, actionVerb, "Action Verb"),
                    FIELD_FLOAT(InteractionData, interactionRadius, "Radius"),
                    holdField,
                    FIELD_FLOAT(InteractionData, cooldownDuration, "Cooldown"),
                    FIELD_INT(InteractionData, usesRemaining, "Uses (-1=unlimited)"),
                    FIELD_BOOL(InteractionData, showHighlight, "Show Highlight"),
                };
                RenderReflectedFields(d, fields);
                if (std::memcmp(&oldSnapshot, d, sizeof(InteractionData)) != 0)
                {
                    InteractionData newSnapshot = *d;
                    SceneFile* cs = m_scene;
                    ObjectID ci = m_inspectedObjectID;
                    auto& history = Spark::Editor::CommandHistory::GetInstance();
                    history.Execute(std::make_unique<Spark::Editor::LambdaCommand>(
                        [cs, ci, newSnapshot]()
                        {
                            Component* c = FindComponent(cs, ci, ComponentType::INTERACTION);
                            if (auto* p = c ? c->GetData<InteractionData>() : nullptr)
                                *p = newSnapshot;
                        },
                        [cs, ci, oldSnapshot]()
                        {
                            Component* c = FindComponent(cs, ci, ComponentType::INTERACTION);
                            if (auto* p = c ? c->GetData<InteractionData>() : nullptr)
                                *p = oldSnapshot;
                        },
                        "Interaction Change"));
                }
            }
            else
            {
                ImGui::TextDisabled("(Component data unavailable)");
            }
            ImGui::Unindent(4);
        }
    }

} // namespace SparkEditor
