/**
 * @file ECSystems.h
 * @brief ECS system definitions that process component data each frame
 * @author Spark Engine Team
 * @date 2025
 *
 * Each system queries specific component combinations from the World
 * and updates them. Systems are the "logic" layer of the ECS pattern.
 */

#pragma once

#include "../Components.h"
#include <functional>
#include <vector>
#include <string>

// Forward declarations
class GraphicsEngine;
class PhysicsSystem;
class AudioEngine;

namespace Spark::ECS {

/**
 * @brief Base interface for all ECS systems
 */
class ISystem
{
public:
    virtual ~ISystem() = default;
    virtual void Update(World& world, float deltaTime) = 0;
    virtual const char* GetName() const = 0;
    virtual bool IsEnabled() const { return m_enabled; }
    virtual void SetEnabled(bool enabled) { m_enabled = enabled; }

protected:
    bool m_enabled = true;
};

/**
 * @brief Render system - submits MeshRenderer + Transform entities to the graphics engine
 */
class RenderSystem : public ISystem
{
public:
    RenderSystem(GraphicsEngine* graphics) : m_graphics(graphics) {}

    void Update(World& world, float deltaTime) override;
    const char* GetName() const override { return "RenderSystem"; }

    int GetRenderedEntityCount() const { return m_renderedCount; }

private:
    GraphicsEngine* m_graphics;
    int m_renderedCount = 0;
};

/**
 * @brief Physics sync system - syncs Transform with physics rigid bodies
 */
class PhysicsUpdateSystem : public ISystem
{
public:
    PhysicsUpdateSystem(PhysicsSystem* physics) : m_physics(physics) {}

    void Update(World& world, float deltaTime) override;
    const char* GetName() const override { return "PhysicsUpdateSystem"; }

private:
    PhysicsSystem* m_physics;
};

/**
 * @brief Audio update system - updates 3D audio source positions from Transform
 */
class AudioUpdateSystem : public ISystem
{
public:
    AudioUpdateSystem(AudioEngine* audio) : m_audio(audio) {}

    void Update(World& world, float deltaTime) override;
    const char* GetName() const override { return "AudioUpdateSystem"; }

private:
    AudioEngine* m_audio;
};

/**
 * @brief Lifecycle system - processes active/inactive entities, health, death
 */
class LifecycleSystem : public ISystem
{
public:
    using DeathCallback = std::function<void(EntityID)>;

    void Update(World& world, float deltaTime) override;
    const char* GetName() const override { return "LifecycleSystem"; }

    void SetDeathCallback(DeathCallback cb) { m_onDeath = cb; }

private:
    DeathCallback m_onDeath;
};

/**
 * @brief System manager - owns and updates all ECS systems in order
 */
class SystemManager
{
public:
    SystemManager() = default;
    ~SystemManager() = default;

    template<typename T, typename... Args>
    T* AddSystem(Args&&... args)
    {
        auto system = std::make_unique<T>(std::forward<Args>(args)...);
        T* ptr = system.get();
        m_systems.push_back(std::move(system));
        return ptr;
    }

    void UpdateAll(World& world, float deltaTime)
    {
        for (auto& system : m_systems)
        {
            if (system->IsEnabled())
                system->Update(world, deltaTime);
        }
    }

    ISystem* GetSystem(const std::string& name) const
    {
        for (const auto& system : m_systems)
        {
            if (system->GetName() == name)
                return system.get();
        }
        return nullptr;
    }

    size_t GetSystemCount() const { return m_systems.size(); }

    // Console integration
    std::string Console_ListSystems() const;

private:
    std::vector<std::unique_ptr<ISystem>> m_systems;
};

} // namespace Spark::ECS
