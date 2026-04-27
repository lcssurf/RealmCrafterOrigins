cmake_minimum_required(VERSION 3.20)
project(rco_renderer LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# Language config
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Idempotent include guard: the client, GUE, and terrain editor each pull this
# subdir in; CMake usually dedupes by path but being explicit avoids
# duplicate-target errors in edge cases.
if(TARGET rco_renderer)
    return()
endif()

# ---------------------------------------------------------------------------
# Dependencies
#
# GLAD with GL_ARB_bindless_texture is vendored in include/glad/ and src/glad.c
# (generated from https://glad.dav1d.de with gl=4.6 + GL_ARB_bindless_texture).
# Do NOT use vcpkg glad — it lacks bindless support.
# ---------------------------------------------------------------------------
find_package(glm    CONFIG REQUIRED)
find_package(assimp CONFIG REQUIRED)
find_package(Stb    REQUIRED)

# ---------------------------------------------------------------------------
# Library target
# ---------------------------------------------------------------------------
add_library(rco_renderer STATIC
    # Vendored GLAD with bindless support
    src/glad.c

    # Fase 1 — .ixx → .h/.cpp conversions
    src/light.cpp
    src/buffers.cpp
    src/texture.cpp
    src/material.cpp
    src/mesh.cpp
    src/shader.cpp
    src/helpers.cpp
    src/compile_shaders.cpp

    # Fase 2 — deferred PBR engine
    src/engine.cpp
    src/pipeline.cpp

    # Skinned PBR model (Assimp loader — RCO's runtime)
    src/model.cpp

    # Renderable skeletal instance (shared across client + GUE)
    src/actor.cpp

    # Model cache (weak_ptr-based, path-keyed)
    src/model_cache.cpp

    # STB single-translation-unit implementation
    src/stb_image_impl.cpp
)

target_include_directories(rco_renderer
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"   # rco/renderer/*.h + vendored glad/
    PRIVATE
        "${Stb_INCLUDE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(rco_renderer
    PUBLIC
        glm::glm         # mat4/vec3 appear in public API (engine.h, pipeline.h)
    PRIVATE
        assimp::assimp   # only used inside model.cpp
        opengl32         # Windows GL function loader target
)

# GLM experimental extensions (used by mesh.h's glm::gtx/hash.hpp).
# Target-scoped so it propagates only to consumers of rco_renderer.
target_compile_definitions(rco_renderer PUBLIC GLM_ENABLE_EXPERIMENTAL)

# ---------------------------------------------------------------------------
# Compile options — target-scoped (no global add_compile_options / definitions,
# which would leak into sibling targets in the same build tree).
# ---------------------------------------------------------------------------
if(MSVC)
    # /Zc:preprocessor — conforming __VA_ARGS__ expansion for shader.cpp's
    #                    GLSL include inliner.
    # /MP              — parallel compilation.
    # /permissive-     — standards-conforming C++ (rejects legacy MS extensions).
    target_compile_options(rco_renderer PRIVATE /Zc:preprocessor /MP /permissive-)
    # /wd4100 — silence "unreferenced formal parameter" warnings from Assimp headers.
    target_compile_options(rco_renderer PRIVATE /wd4100)
else()
    target_compile_options(rco_renderer PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

set_target_properties(rco_renderer PROPERTIES POSITION_INDEPENDENT_CODE ON)
