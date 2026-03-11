# =============================================================================
# SparkComponentLibraries.cmake — R9.1 Architecture Analysis
# =============================================================================
#
# Defines per-subsystem INTERFACE/OBJECT library targets that partition the
# monolithic SparkEngineLib into fine-grained component libraries.
#
# Usage (from root CMakeLists.txt):
#   include(cmake/SparkComponentLibraries.cmake)
#   spark_define_component_libraries()
#
# After calling, the following targets are available:
#   SparkCore         — Platform, EngineContext, Assert, Logger, FrameAllocator
#   SparkECS          — Components, Systems, World, ReactiveSystem
#   SparkGraphics     — GraphicsEngine, RHI, RenderGraph, Shader, PostProcessing
#   SparkPhysics      — PhysicsSystem, Bullet integration
#   SparkAudio        — AudioEngine, XAudio2 integration
#   SparkAI           — AISystem, BehaviorTree, NavMesh, Steering, Perception
#   SparkAnimation    — AnimationSystem, Skeleton, Blender, IK, StateMachine
#   SparkNetworking   — NetworkManager, Transport, Security
#   SparkUtils        — Console, Profiler, JobSystem, Octree, CrashHandler
#   SparkEditor       — Editor panels, plugin system (optional)
#
# Each component target declares its own include directories and public
# dependencies, enabling consumers to link only what they need.
#
# ## Migration guide
# To migrate from SparkEngineLib to component libraries:
#
# 1. Replace:
#      target_link_libraries(MyTarget PRIVATE SparkEngineLib)
#    With:
#      target_link_libraries(MyTarget PRIVATE SparkECS SparkGraphics SparkPhysics)
#
# 2. SparkEngineLib remains as an "umbrella" target that links all components:
#      target_link_libraries(SparkEngineLib PUBLIC
#          SparkCore SparkECS SparkGraphics SparkPhysics SparkAudio
#          SparkAI SparkAnimation SparkNetworking SparkUtils)
#

function(spark_define_component_libraries)
    set(ENGINE_SRC "${CMAKE_SOURCE_DIR}/SparkEngine/Source")

    # =========================================================================
    # SparkCore — Platform abstractions, service locator, core utilities
    # =========================================================================
    file(GLOB SPARK_CORE_SOURCES CONFIGURE_DEPENDS
        "${ENGINE_SRC}/Core/*.cpp"
        "${ENGINE_SRC}/Core/*.h"
    )
    add_library(SparkCore OBJECT ${SPARK_CORE_SOURCES})
    target_include_directories(SparkCore PUBLIC "${ENGINE_SRC}")
    target_compile_features(SparkCore PUBLIC cxx_std_20)
    set_target_properties(SparkCore PROPERTIES FOLDER "Engine/Core")

    # =========================================================================
    # SparkUtils — Console, Logger, Profiler, JobSystem, Octree, FrameAllocator
    # =========================================================================
    file(GLOB SPARK_UTILS_SOURCES CONFIGURE_DEPENDS
        "${ENGINE_SRC}/Utils/*.cpp"
        "${ENGINE_SRC}/Utils/*.h"
    )
    add_library(SparkUtils OBJECT ${SPARK_UTILS_SOURCES})
    target_include_directories(SparkUtils PUBLIC "${ENGINE_SRC}")
    target_link_libraries(SparkUtils PUBLIC SparkCore)
    target_compile_features(SparkUtils PUBLIC cxx_std_20)
    set_target_properties(SparkUtils PROPERTIES FOLDER "Engine/Utils")

    # =========================================================================
    # SparkECS — Components, Systems, World, ReactiveSystem, PhaseSystemManager
    # =========================================================================
    file(GLOB_RECURSE SPARK_ECS_SOURCES CONFIGURE_DEPENDS
        "${ENGINE_SRC}/Engine/ECS/*.cpp"
        "${ENGINE_SRC}/Engine/ECS/*.h"
    )
    add_library(SparkECS OBJECT ${SPARK_ECS_SOURCES})
    target_include_directories(SparkECS PUBLIC "${ENGINE_SRC}")
    target_link_libraries(SparkECS PUBLIC SparkCore)
    target_compile_features(SparkECS PUBLIC cxx_std_20)
    set_target_properties(SparkECS PROPERTIES FOLDER "Engine/ECS")

    # =========================================================================
    # SparkGraphics — GraphicsEngine, RHI, RenderGraph, Shader, PostProcessing
    # =========================================================================
    file(GLOB_RECURSE SPARK_GRAPHICS_SOURCES CONFIGURE_DEPENDS
        "${ENGINE_SRC}/Graphics/*.cpp"
        "${ENGINE_SRC}/Graphics/*.h"
    )
    add_library(SparkGraphics OBJECT ${SPARK_GRAPHICS_SOURCES})
    target_include_directories(SparkGraphics PUBLIC "${ENGINE_SRC}")
    target_link_libraries(SparkGraphics PUBLIC SparkCore SparkUtils)
    target_compile_features(SparkGraphics PUBLIC cxx_std_20)
    set_target_properties(SparkGraphics PROPERTIES FOLDER "Engine/Graphics")

    # =========================================================================
    # SparkPhysics — PhysicsSystem, Bullet Physics integration
    # =========================================================================
    file(GLOB_RECURSE SPARK_PHYSICS_SOURCES CONFIGURE_DEPENDS
        "${ENGINE_SRC}/Engine/Physics/*.cpp"
        "${ENGINE_SRC}/Engine/Physics/*.h"
    )
    add_library(SparkPhysics OBJECT ${SPARK_PHYSICS_SOURCES})
    target_include_directories(SparkPhysics PUBLIC "${ENGINE_SRC}")
    target_link_libraries(SparkPhysics PUBLIC SparkCore SparkECS)
    target_compile_features(SparkPhysics PUBLIC cxx_std_20)
    set_target_properties(SparkPhysics PROPERTIES FOLDER "Engine/Physics")

    # =========================================================================
    # SparkAudio — AudioEngine, XAudio2
    # =========================================================================
    file(GLOB_RECURSE SPARK_AUDIO_SOURCES CONFIGURE_DEPENDS
        "${ENGINE_SRC}/Engine/Audio/*.cpp"
        "${ENGINE_SRC}/Engine/Audio/*.h"
    )
    if(SPARK_AUDIO_SOURCES)
        add_library(SparkAudio OBJECT ${SPARK_AUDIO_SOURCES})
        target_include_directories(SparkAudio PUBLIC "${ENGINE_SRC}")
        target_link_libraries(SparkAudio PUBLIC SparkCore SparkECS)
        target_compile_features(SparkAudio PUBLIC cxx_std_20)
        set_target_properties(SparkAudio PROPERTIES FOLDER "Engine/Audio")
    endif()

    # =========================================================================
    # SparkAI — AISystem, BehaviorTree, NavMesh, Steering, Perception
    # =========================================================================
    file(GLOB_RECURSE SPARK_AI_SOURCES CONFIGURE_DEPENDS
        "${ENGINE_SRC}/Engine/AI/*.cpp"
        "${ENGINE_SRC}/Engine/AI/*.h"
    )
    if(SPARK_AI_SOURCES)
        add_library(SparkAI OBJECT ${SPARK_AI_SOURCES})
        target_include_directories(SparkAI PUBLIC "${ENGINE_SRC}")
        target_link_libraries(SparkAI PUBLIC SparkCore SparkECS SparkUtils)
        target_compile_features(SparkAI PUBLIC cxx_std_20)
        set_target_properties(SparkAI PROPERTIES FOLDER "Engine/AI")
    endif()

    # =========================================================================
    # SparkAnimation — Skeleton, Clips, Blending, IK, StateMachine
    # =========================================================================
    file(GLOB_RECURSE SPARK_ANIMATION_SOURCES CONFIGURE_DEPENDS
        "${ENGINE_SRC}/Engine/Animation/*.cpp"
        "${ENGINE_SRC}/Engine/Animation/*.h"
    )
    if(SPARK_ANIMATION_SOURCES)
        add_library(SparkAnimation OBJECT ${SPARK_ANIMATION_SOURCES})
        target_include_directories(SparkAnimation PUBLIC "${ENGINE_SRC}")
        target_link_libraries(SparkAnimation PUBLIC SparkCore SparkECS)
        target_compile_features(SparkAnimation PUBLIC cxx_std_20)
        set_target_properties(SparkAnimation PROPERTIES FOLDER "Engine/Animation")
    endif()

    # =========================================================================
    # SparkNetworking — NetworkManager, Transport, Security
    # =========================================================================
    file(GLOB_RECURSE SPARK_NET_SOURCES CONFIGURE_DEPENDS
        "${ENGINE_SRC}/Engine/Networking/*.cpp"
        "${ENGINE_SRC}/Engine/Networking/*.h"
    )
    if(SPARK_NET_SOURCES)
        add_library(SparkNetworking OBJECT ${SPARK_NET_SOURCES})
        target_include_directories(SparkNetworking PUBLIC "${ENGINE_SRC}")
        target_link_libraries(SparkNetworking PUBLIC SparkCore)
        target_compile_features(SparkNetworking PUBLIC cxx_std_20)
        set_target_properties(SparkNetworking PROPERTIES FOLDER "Engine/Networking")
    endif()

    # =========================================================================
    # Log the component targets
    # =========================================================================
    message(STATUS "[R9.1] Component libraries defined:")
    message(STATUS "  SparkCore, SparkUtils, SparkECS, SparkGraphics, SparkPhysics")
    if(TARGET SparkAudio)
        message(STATUS "  SparkAudio")
    endif()
    if(TARGET SparkAI)
        message(STATUS "  SparkAI")
    endif()
    if(TARGET SparkAnimation)
        message(STATUS "  SparkAnimation")
    endif()
    if(TARGET SparkNetworking)
        message(STATUS "  SparkNetworking")
    endif()

endfunction()
