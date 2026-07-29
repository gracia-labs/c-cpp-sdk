cmake_minimum_required(VERSION 3.21)
project(win_desktop_demo LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Allow building this example standalone (outside the parent CMakeLists) as long
# as the gracia::sdk target is provided some other way; normally it is defined by
# ../../CMakeLists.txt.
get_filename_component(SDK_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
include("${SDK_ROOT}/cmake/CPM.cmake")

# The shared viewer core: Vulkan device setup, frame ring, scenes, playback.
# It also fetches Vulkan-Headers, glm and volk, and defines the volk target.
if(NOT TARGET gracia_demo_common)
  add_subdirectory("${SDK_ROOT}/examples/common" "${CMAKE_CURRENT_BINARY_DIR}/common")
endif()

# --- Third-party dependencies for the desktop shell only ----------------------
CPMAddPackage(
  NAME glfw
  GITHUB_REPOSITORY glfw/glfw
  GIT_TAG 3.4
  OPTIONS
    "GLFW_BUILD_DOCS OFF"
    "GLFW_BUILD_TESTS OFF"
    "GLFW_BUILD_EXAMPLES OFF"
    "GLFW_INSTALL OFF"
)

CPMAddPackage(
  NAME imgui
  GITHUB_REPOSITORY ocornut/imgui
  GIT_TAG v1.90.9
  DOWNLOAD_ONLY YES
)

# --- Dear ImGui (core + GLFW/Vulkan backends) ---------------------------------
add_library(imgui STATIC
  "${imgui_SOURCE_DIR}/imgui.cpp"
  "${imgui_SOURCE_DIR}/imgui_draw.cpp"
  "${imgui_SOURCE_DIR}/imgui_tables.cpp"
  "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
  "${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp"
  "${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp"
)
target_include_directories(imgui PUBLIC
  "${imgui_SOURCE_DIR}" "${imgui_SOURCE_DIR}/backends")
target_link_libraries(imgui PUBLIC glfw volk Vulkan::Headers)
# The Vulkan backend resolves entry points through volk (no static prototypes).
target_compile_definitions(imgui PUBLIC
  VK_NO_PROTOTYPES IMGUI_IMPL_VULKAN_NO_PROTOTYPES)

# --- The demo executable ------------------------------------------------------
add_executable(win_desktop_demo
  src/main.cpp
  src/vk_context.cpp
  src/splat_viewer.cpp
  src/camera.cpp
)

target_link_libraries(win_desktop_demo PRIVATE
  gracia_demo_common
  imgui
  glfw
  comdlg32
)

target_compile_definitions(win_desktop_demo PRIVATE
  IMGUI_IMPL_VULKAN_NO_PROTOTYPES
  GLFW_INCLUDE_NONE
)

set_target_properties(win_desktop_demo PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# Put gracia_sdk.dll next to the executable so it loads at runtime.
add_custom_command(TARGET win_desktop_demo POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "$<TARGET_FILE:gracia::sdk>" "$<TARGET_FILE_DIR:win_desktop_demo>"
  COMMENT "Copying gracia_sdk.dll next to the demo executable")
