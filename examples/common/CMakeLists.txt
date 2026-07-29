cmake_minimum_required(VERSION 3.21)
project(gracia_demo_common LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(SDK_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
include("${SDK_ROOT}/cmake/CPM.cmake")

# --- Dependencies every viewer needs (fetched from GitHub with CPM) -----------
# Vulkan-Headers is pinned to the SDK version volk targets.
CPMAddPackage(
  NAME Vulkan-Headers
  GITHUB_REPOSITORY KhronosGroup/Vulkan-Headers
  GIT_TAG v1.4.304
)

CPMAddPackage(
  NAME glm
  GITHUB_REPOSITORY g-truc/glm
  GIT_TAG 1.0.1
)

CPMAddPackage(
  NAME volk
  GITHUB_REPOSITORY zeux/volk
  GIT_TAG vulkan-sdk-1.4.304.0
  DOWNLOAD_ONLY YES
)

# --- volk (meta-loader; built from the fetched source) ------------------------
add_library(volk STATIC "${volk_SOURCE_DIR}/volk.c")
target_include_directories(volk PUBLIC "${volk_SOURCE_DIR}")
target_link_libraries(volk PUBLIC Vulkan::Headers)
target_compile_definitions(volk PUBLIC VK_NO_PROTOTYPES VK_USE_PLATFORM_WIN32_KHR)

# --- The shared viewer core ---------------------------------------------------
# Platform agnostic on purpose: no window system, no OpenXR, no UI toolkit. It
# holds what a Gracia viewer needs whatever it draws into — the Vulkan device
# setup, the frame ring, the scenes, and the playback clock.
add_library(gracia_demo_common STATIC
  src/scene.cpp
  src/player.cpp
  src/vk_common.cpp
  src/frame_ring.cpp
)

target_include_directories(gracia_demo_common PUBLIC
  "${CMAKE_CURRENT_LIST_DIR}/include")

target_link_libraries(gracia_demo_common PUBLIC
  gracia::sdk
  volk
  glm::glm
  Vulkan::Headers
)

target_compile_definitions(gracia_demo_common PUBLIC
  VK_NO_PROTOTYPES
  VK_USE_PLATFORM_WIN32_KHR
  NOMINMAX
  WIN32_LEAN_AND_MEAN
  GLM_FORCE_DEPTH_ZERO_TO_ONE
  GLM_ENABLE_EXPERIMENTAL
)
