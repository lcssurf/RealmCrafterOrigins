cmake_minimum_required(VERSION 3.20)
project(rco_physics LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Language config
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Idempotent include guard — same reasoning as shared/renderer/CMakeLists.txt:
# any consumer (client today; GUE and others later, once this actually does
# something) pulling this subdir in more than once shouldn't redefine the
# target.
if(TARGET rco_physics)
    return()
endif()

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
find_package(glm CONFIG REQUIRED)

# rco_renderer's collision_data.h (ColBox/ColSphere/ColTri/ColData — pure
# data, no OpenGL) is a real dependency now: CollisionWorld and the capsule
# sweep dereference these types directly (see collision_world.h's doc
# comment for why Terrain itself is deliberately NOT included the same way).
# Self-sufficient: adds shared/renderer itself if no consumer higher up the
# tree already did, so rco_physics doesn't depend on include order.
if(NOT TARGET rco_renderer)
    add_subdirectory(
        "${CMAKE_CURRENT_SOURCE_DIR}/../renderer"
        "${CMAKE_BINARY_DIR}/_rco_renderer"
    )
endif()

# ---------------------------------------------------------------------------
# Library target
# ---------------------------------------------------------------------------
add_library(rco_physics STATIC
    src/collision_world.cpp
    src/capsule_sweep.cpp
    src/character_physics.cpp
)

target_include_directories(rco_physics
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"   # rco/physics/*.h
)

target_link_libraries(rco_physics
    PUBLIC
        glm::glm       # vec3/mat3 in the public API (CapsuleMoveInput/Result etc)
        rco_renderer   # collision_data.h's ColBox/ColSphere/ColTri/ColData
)

# ---------------------------------------------------------------------------
# Compile options — target-scoped, same convention as rco_renderer.
# ---------------------------------------------------------------------------
if(MSVC)
    target_compile_options(rco_physics PRIVATE /Zc:preprocessor /MP /permissive-)
else()
    target_compile_options(rco_physics PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

set_target_properties(rco_physics PROPERTIES POSITION_INDEPENDENT_CODE ON)
