cmake_minimum_required(VERSION 3.21)
project(win_openxr_demo LANGUAGES C CXX)

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
  GITHUB_REPOSITORY KhronosGroup/OpenXR-SDK
  GIT_TAG release-1.1.61
  OPTIONS
    "DYNAMIC_LOADER OFF"
    "BUILD_TESTING OFF"
    "BUILD_WITH_SYSTEM_JSONCPP OFF"
    "BUILD_API_LAYERS OFF"
    "BUILD_TESTS OFF"
    "BUILD_CONFORMANCE_TESTS OFF"
)

# --- Shaders ------------------------------------------------------------------
# The only shaders in the examples: the sRGB resolve for the --fp16 target.
# glslc emits them as C array initializers (-mfmt=c), so they are #included into
# the binary. No .spv files to find at runtime, and no asset path to get wrong.
find_program(GLSLC_EXE NAMES glslc
  HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin")
if(NOT GLSLC_EXE)
  message(FATAL_ERROR
    "glslc not found. Install the Vulkan SDK, or set VULKAN_SDK.")
endif()

set(SHADER_OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY "${SHADER_OUT_DIR}")
set(SHADER_HEADERS)
foreach(shader srgb_resolve.vert srgb_resolve.frag)
  set(src "${CMAKE_CURRENT_LIST_DIR}/shaders/${shader}")
  set(dst "${SHADER_OUT_DIR}/${shader}.inc")
  add_custom_command(
    OUTPUT "${dst}"
    COMMAND "${GLSLC_EXE}" -O -mfmt=c -o "${dst}" "${src}"
    DEPENDS "${src}"
    COMMENT "glslc ${shader}"
    VERBATIM)
  list(APPEND SHADER_HEADERS "${dst}")
endforeach()

# --- The demo executable ------------------------------------------------------
add_executable(win_openxr_demo
  src/main.cpp
  src/xr_context.cpp
  src/xr_viewer.cpp
  ${SHADER_HEADERS}
)

target_include_directories(win_openxr_demo PRIVATE "${SHADER_OUT_DIR}")

target_link_libraries(win_openxr_demo PRIVATE
  gracia_demo_common
  openxr_loader
)

target_compile_definitions(win_openxr_demo PRIVATE
  XR_USE_GRAPHICS_API_VULKAN
  XR_USE_PLATFORM_WIN32
)

set_target_properties(win_openxr_demo PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# Put gracia_sdk.dll next to the executable so it loads at runtime.
add_custom_command(TARGET win_openxr_demo POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "$<TARGET_FILE:gracia::sdk>" "$<TARGET_FILE_DIR:win_openxr_demo>"
  COMMENT "Copying gracia_sdk.dll next to the demo executable")
