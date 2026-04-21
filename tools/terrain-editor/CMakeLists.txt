cmake_minimum_required(VERSION 3.20)
project(rco_terrain LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

file(GLOB_RECURSE TE_SOURCES "${CMAKE_SOURCE_DIR}/src/*.cpp")
add_executable(rco_terrain ${TE_SOURCES})

find_package(glfw3  CONFIG REQUIRED)
find_package(glad   CONFIG REQUIRED)
find_package(imgui  CONFIG REQUIRED)
find_package(glm    CONFIG REQUIRED)
find_package(Stb    REQUIRED)

target_include_directories(rco_terrain PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    ${Stb_INCLUDE_DIR}
)

target_link_libraries(rco_terrain PRIVATE
    glfw
    glad::glad
    imgui::imgui
    glm::glm
)

if(WIN32)
    target_link_libraries(rco_terrain PRIVATE ws2_32)
endif()

set(RCO_TOOLS_DIST "${CMAKE_SOURCE_DIR}/../../dist/tools")

set_target_properties(rco_terrain PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY       "${RCO_TOOLS_DIST}"
    RUNTIME_OUTPUT_DIRECTORY            "${RCO_TOOLS_DIST}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG      "${RCO_TOOLS_DIST}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE    "${RCO_TOOLS_DIST}"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${RCO_TOOLS_DIST}"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${RCO_TOOLS_DIST}"
)
