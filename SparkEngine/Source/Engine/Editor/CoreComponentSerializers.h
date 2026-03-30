/**
 * @file CoreComponentSerializers.h
 * @brief Registers snapshot serializers for all core ECS components
 *
 * Each core component (Transform, NameComponent, ActiveComponent, LightComponent,
 * Camera, MeshRenderer, RigidBodyComponent, ColliderComponent, AudioSourceComponent,
 * TagComponent) gets a ComponentSerializerEntry that can round-trip its data through
 * the SceneSnapshotSerializer binary format.
 *
 * Call RegisterCoreComponentSerializers() once at engine startup (e.g. from
 * InitGameplaySystems) before any play-mode snapshot is taken.
 */

#pragma once

namespace Spark::Editor
{

    /// @brief Register snapshot serializers for all core ECS component types.
    /// Safe to call multiple times — subsequent calls are no-ops.
    void RegisterCoreComponentSerializers();

} // namespace Spark::Editor
