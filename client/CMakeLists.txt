cmake_minimum_required(VERSION 3.20)
project(rco_client LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --------------------------------------------------------------------------
# Source files
# --------------------------------------------------------------------------
file(GLOB_RECURSE RCO_SOURCES
    "${CMAKE_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_SOURCE_DIR}/src/**/*.cpp"
)

add_executable(rco_client ${RCO_SOURCES})

# --------------------------------------------------------------------------
# vcpkg / find_package
# --------------------------------------------------------------------------
find_package(glfw3   CONFIG REQUIRED)
find_package(glad    CONFIG REQUIRED)
find_package(glm     CONFIG REQUIRED)
find_package(imgui   CONFIG REQUIRED)
find_package(assimp  CONFIG REQUIRED)
find_package(Stb     REQUIRED)

# msquic: CONFIG package first; if no target resolves, fall back to find_library.
find_package(msquic CONFIG QUIET)
if (TARGET msquic::msquic)
    set(MSQUIC_TARGET msquic::msquic)
elseif (TARGET msquic)
    set(MSQUIC_TARGET msquic)
else()
    find_library(MSQUIC_LIB NAMES msquic
        HINTS
            "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib"
            "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/lib"
        REQUIRED)
    find_path(MSQUIC_INCLUDE_DIR NAMES msquic.h
        HINTS "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/include"
        REQUIRED)
    add_library(_msquic UNKNOWN IMPORTED)
    set_target_properties(_msquic PROPERTIES IMPORTED_LOCATION "${MSQUIC_LIB}")
    target_include_directories(rco_client PRIVATE "${MSQUIC_INCLUDE_DIR}")
    set(MSQUIC_TARGET _msquic)
    message(STATUS "msquic: using raw library ${MSQUIC_LIB}")
endif()
message(STATUS "msquic target: ${MSQUIC_TARGET}")

# --------------------------------------------------------------------------
# Include dirs
# --------------------------------------------------------------------------
target_include_directories(rco_client PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    "${Stb_INCLUDE_DIR}"
)

# --------------------------------------------------------------------------
# Link libraries
# --------------------------------------------------------------------------
target_link_libraries(rco_client PRIVATE
    glfw
    glad::glad
    glm::glm
    imgui::imgui
    assimp::assimp
    ${MSQUIC_TARGET}
)

# Windows extras
if (WIN32)
    target_link_libraries(rco_client PRIVATE ws2_32 secur32)
endif()

# Linux extras
if (UNIX AND NOT APPLE)
    target_link_libraries(rco_client PRIVATE pthread)
endif()

# --------------------------------------------------------------------------
# Working directory and post-build
# --------------------------------------------------------------------------
set_target_properties(rco_client PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

# Copy config.toml to build dir after build (ignore if it doesn't exist yet)
if (EXISTS "${CMAKE_SOURCE_DIR}/config.toml")
    add_custom_command(TARGET rco_client POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/config.toml"
            "$<TARGET_FILE_DIR:rco_client>/config.toml"
        COMMENT "Copying config.toml"
    )
endif()

# Copy shaders directory to build output so the exe can find them at runtime.
add_custom_command(TARGET rco_client POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/shaders"
        "$<TARGET_FILE_DIR:rco_client>/shaders"
    COMMENT "Copying shaders"
)

# Copy assets directory to build output (models, textures, etc.)
if (EXISTS "${CMAKE_SOURCE_DIR}/assets")
    add_custom_command(TARGET rco_client POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/assets"
            "$<TARGET_FILE_DIR:rco_client>/assets"
        COMMENT "Copying assets"
    )
endif()
