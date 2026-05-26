cmake_minimum_required(VERSION 3.20)
project(rco_gue LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Shared renderer (Model + Shader) — same code as the client uses.
add_subdirectory(
    "${CMAKE_SOURCE_DIR}/../../shared/renderer"
    "${CMAKE_BINARY_DIR}/_rco_renderer"
)

file(GLOB_RECURSE GUE_SOURCES "${CMAKE_SOURCE_DIR}/src/*.cpp")
list(APPEND GUE_SOURCES "${CMAKE_SOURCE_DIR}/src/tabs/weapon_kits.cpp")
list(APPEND GUE_SOURCES "${CMAKE_SOURCE_DIR}/src/tabs/equipment_slots.cpp")
list(REMOVE_DUPLICATES GUE_SOURCES)
add_executable(rco_gue ${GUE_SOURCES})

find_package(glfw3                CONFIG REQUIRED)
# glad is vendored in shared/renderer (bindless version) — do NOT find_package(glad)
find_package(imgui                CONFIG REQUIRED)
find_package(glm                  CONFIG REQUIRED)
find_package(unofficial-sqlite3   CONFIG REQUIRED)
find_package(Stb                  REQUIRED)

target_include_directories(rco_gue PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party"
    ${Stb_INCLUDE_DIR}
)

target_link_libraries(rco_gue PRIVATE
    rco_renderer
    glfw
    glm::glm
    imgui::imgui
    unofficial::sqlite3::sqlite3
)

if (WIN32)
    target_link_libraries(rco_gue PRIVATE ws2_32 comdlg32)
endif()

set(RCO_TOOLS_DIST "${CMAKE_SOURCE_DIR}/../../dist/tools")

set_target_properties(rco_gue PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY       "${RCO_TOOLS_DIST}"
    RUNTIME_OUTPUT_DIRECTORY            "${RCO_TOOLS_DIST}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG      "${RCO_TOOLS_DIST}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE    "${RCO_TOOLS_DIST}"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${RCO_TOOLS_DIST}"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${RCO_TOOLS_DIST}"
)
