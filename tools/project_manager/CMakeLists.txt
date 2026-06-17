cmake_minimum_required(VERSION 3.20)
project(rco_project_manager LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# glad is vendored in shared/renderer. Guard prevents duplicate target when
# built alongside GUE under tools/CMakeLists.txt.
if (NOT TARGET rco_renderer)
    add_subdirectory(
        "${CMAKE_SOURCE_DIR}/../../shared/renderer"
        "${CMAKE_CURRENT_BINARY_DIR}/_rco_renderer"
    )
endif()

add_executable(rco_project_manager main.cpp)

find_package(glfw3  CONFIG REQUIRED)
find_package(imgui  CONFIG REQUIRED)

target_link_libraries(rco_project_manager PRIVATE
    rco_renderer
    glfw
    imgui::imgui
)

if (WIN32)
    # shell32: ShellExecuteW (open in Explorer), SHBrowseForFolderW, SHGetPathFromIDListW
    # ole32:   CoInitializeEx, CoUninitialize, CoTaskMemFree (used by SHBrowseForFolderW)
    target_link_libraries(rco_project_manager PRIVATE shell32 ole32)
endif()

set(RCO_TOOLS_DIST "${CMAKE_SOURCE_DIR}/../../dist/tools")

set_target_properties(rco_project_manager PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY           "${RCO_TOOLS_DIST}"
    RUNTIME_OUTPUT_DIRECTORY                "${RCO_TOOLS_DIST}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG          "${RCO_TOOLS_DIST}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE        "${RCO_TOOLS_DIST}"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${RCO_TOOLS_DIST}"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL     "${RCO_TOOLS_DIST}"
)
