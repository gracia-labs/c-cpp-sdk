cmake_minimum_required(VERSION 3.21)
project(GraciaCppSDK VERSION 1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")

option(GRACIA_BUILD_EXAMPLES "Build the Gracia C/C++ SDK examples" ON)

# --- Prebuilt SDK archive ------------------------------------------------------
# The archive that matches the target platform:
#   artifacts/windows.zip          artifacts/android.zip (arm64-v8a)
#     include/gracia/SDK.h           include/gracia/SDK.h
#     include/gracia/SDK.hpp         include/gracia/SDK.hpp
#     gracia_sdk.dll                 libgracia_sdk.so
#     gracia_sdk.lib
if(ANDROID OR CMAKE_SYSTEM_NAME STREQUAL "Android")
  set(GRACIA_SDK_PLATFORM android)
  set(GRACIA_SDK_LIBRARY libgracia_sdk.so)
  set(GRACIA_SDK_IMPLIB "")
elseif(WIN32)
  set(GRACIA_SDK_PLATFORM windows)
  set(GRACIA_SDK_LIBRARY gracia_sdk.dll)
  set(GRACIA_SDK_IMPLIB gracia_sdk.lib)
else()
  message(WARNING
    "Gracia C/C++ SDK: this package ships prebuilt binaries for Windows "
    "(windows.zip) and Android arm64-v8a (android.zip). Configuring for another "
    "platform only exposes the sources.")
  return()
endif()

# Not a cache entry: the default follows the target platform. -DGRACIA_SDK_ZIP=<path>
# still overrides it.
if(NOT GRACIA_SDK_ZIP)
  set(GRACIA_SDK_ZIP "${CMAKE_CURRENT_LIST_DIR}/artifacts/${GRACIA_SDK_PLATFORM}.zip")
endif()
set(GRACIA_SDK_DIR "${CMAKE_BINARY_DIR}/gracia-sdk/${GRACIA_SDK_PLATFORM}")

if(NOT EXISTS "${GRACIA_SDK_ZIP}")
  message(FATAL_ERROR
    "Gracia SDK archive not found:\n  ${GRACIA_SDK_ZIP}\n"
    "The archive ships with this repository; restore it, or point "
    "GRACIA_SDK_ZIP at a copy. A build directory that was configured before the "
    "archives moved into artifacts/ holds the old path in its CMake cache: "
    "delete the build directory and configure it again.")
endif()

if(NOT EXISTS "${GRACIA_SDK_DIR}/${GRACIA_SDK_LIBRARY}")
  message(STATUS "Gracia SDK: extracting ${GRACIA_SDK_ZIP}")
  file(MAKE_DIRECTORY "${GRACIA_SDK_DIR}")
  file(ARCHIVE_EXTRACT INPUT "${GRACIA_SDK_ZIP}" DESTINATION "${GRACIA_SDK_DIR}")
endif()

# --- gracia::sdk imported target ----------------------------------------------
add_library(gracia::sdk SHARED IMPORTED GLOBAL)
set_target_properties(gracia::sdk PROPERTIES
  IMPORTED_LOCATION             "${GRACIA_SDK_DIR}/${GRACIA_SDK_LIBRARY}"
  INTERFACE_INCLUDE_DIRECTORIES "${GRACIA_SDK_DIR}/include")

if(GRACIA_SDK_IMPLIB)
  set_target_properties(gracia::sdk PROPERTIES
    IMPORTED_IMPLIB "${GRACIA_SDK_DIR}/${GRACIA_SDK_IMPLIB}")
endif()

# The examples are Windows only: they use a Win32 surface, a desktop OpenXR
# runtime and GLFW. On Android link gracia::sdk from your own build.
if(GRACIA_BUILD_EXAMPLES AND WIN32)
  add_subdirectory(examples/common)   # shared viewer core, used by both examples
  add_subdirectory(examples/win-desktop-demo)
  add_subdirectory(examples/win-openxr)
elseif(GRACIA_BUILD_EXAMPLES)
  message(STATUS "Gracia SDK: the examples are Windows only; skipped.")
endif()
