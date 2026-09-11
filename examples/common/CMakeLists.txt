cmake_minimum_required(VERSION 3.21)
project(gracia_demo_common LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(SDK_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
include("${SDK_ROOT}/cmake/CPM.cmake")

# --- Dependencies every viewer needs -----------------------------------------
# Vulkan-Headers and glm ship as local archives in deps/; they are the same
# versions gracia_sdk.dll itself was built against.
CPMAddPackage(
  NAME Vulkan-Headers
  URL "file://${SDK_ROOT}/deps/Vulkan-Headers-1.4.357.zip"
)

# Headers only, so the archive carries no CMakeLists and defines no target.
CPMAddPackage(
  NAME glm
  URL "file://${SDK_ROOT}/deps/glm-1.0.1-light.zip"
)
add_library(glm_headers INTERFACE)
add_library(glm::glm ALIAS glm_headers)
target_include_directories(glm_headers INTERFACE "${glm_SOURCE_DIR}")

CPMAddPackage(
  NAME volk
  URL "file://${SDK_ROOT}/deps/volk-vulkan-sdk-1.4.304.0.zip"
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
