cmake_minimum_required(VERSION 3.21)
project(xr_viewer LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(SDK_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
include("${SDK_ROOT}/cmake/CPM.cmake")

# The shared viewer core: Vulkan device setup, frame ring, scenes, playback.
# It also fetches Vulkan-Headers, glm and volk, and defines the volk target.
if(NOT TARGET gracia_demo_common)
  add_subdirectory("${SDK_ROOT}/examples/common" "${CMAKE_CURRENT_BINARY_DIR}/common")
endif()

# OpenXR-SDK is the release repo: the generated sources are committed, so no
# Python or Jinja2 is needed, and jsoncpp is vendored. DYNAMIC_LOADER OFF gives a
# static openxr_loader.lib, so there is no second DLL to copy.
CPMAddPackage(
  NAME OpenXR
  URL "file://${SDK_ROOT}/deps/OpenXR-SDK-release-1.1.61.zip"
  OPTIONS
    "DYNAMIC_LOADER OFF"
    "BUILD_TESTING OFF"
    "BUILD_WITH_SYSTEM_JSONCPP OFF"
    "BUILD_API_LAYERS OFF"
    "BUILD_TESTS OFF"
    "BUILD_CONFORMANCE_TESTS OFF"
)

# --- The demo executable ------------------------------------------------------
add_executable(xr_viewer
  src/main.cpp
  src/xr_context.cpp
  src/xr_viewer.cpp
)

target_link_libraries(xr_viewer PRIVATE
  gracia_demo_common
  openxr_loader
)

target_compile_definitions(xr_viewer PRIVATE
  XR_USE_GRAPHICS_API_VULKAN
  TARGET_NAME="$<TARGET_PROPERTY:NAME>"
)

# On Linux the executable finds libgracia_sdk.so next to itself, like the DLL.
set_target_properties(xr_viewer PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
  BUILD_WITH_INSTALL_RPATH ON
  INSTALL_RPATH "$ORIGIN")

# Put the SDK library next to the executable so it loads at runtime.
add_custom_command(TARGET xr_viewer POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "$<TARGET_FILE:gracia::sdk>" "$<TARGET_FILE_DIR:xr_viewer>"
  COMMENT "Copying the SDK library next to the demo executable")
