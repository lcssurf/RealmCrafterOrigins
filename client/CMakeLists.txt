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

find_path(MINIAUDIO_INCLUDE_DIRS "miniaudio.h" REQUIRED)

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
    "${MINIAUDIO_INCLUDE_DIRS}"
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
    target_link_libraries(rco_client PRIVATE ws2_32 secur32 ole32)
endif()

# Linux extras
if (UNIX AND NOT APPLE)
    target_link_libraries(rco_client PRIVATE pthread)
endif()

# --------------------------------------------------------------------------
# Output directory — exe lives directly in dist/client/ alongside its assets.
# No copy steps needed; shaders/assets/data are the source of truth in dist/.
# --------------------------------------------------------------------------
set(RCO_CLIENT_DIST "${CMAKE_SOURCE_DIR}/../dist/client")

set_target_properties(rco_client PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY       "${RCO_CLIENT_DIST}"
    RUNTIME_OUTPUT_DIRECTORY            "${RCO_CLIENT_DIST}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG      "${RCO_CLIENT_DIST}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE    "${RCO_CLIENT_DIST}"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${RCO_CLIENT_DIST}"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${RCO_CLIENT_DIST}"
)
