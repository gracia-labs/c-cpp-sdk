# Gracia C/C++ SDK

The Gracia C/C++ SDK renders Gaussian-splat scenes and volumetric videos on your
own GPU device. On Windows, Android and Linux the SDK uses **Vulkan**. The public
surface is a small C ABI (`gracia/SDK.h`) with a header-only C++ wrapper
(`gracia/SDK.hpp`).

## Platforms

This SDK ships a prebuilt binary for **Windows x64**, **Android arm64-v8a**
and **Linux x86_64**. [CMakeLists.txt](CMakeLists.txt) selects the archive that
matches the target platform. The **examples run on Windows and Linux**: on
Android link `gracia::sdk` from your own build.

For other platforms use the matching Gracia SDK:

| Platform | SDK |
| --- | --- |
| macOS, iOS, visionOS | [gracia-labs/apple-sdk](https://github.com/gracia-labs/apple-sdk) |
| Python | [gracia-labs/python-sdk](https://github.com/gracia-labs/python-sdk) |
| Web | [gracia-labs/web-sdk](https://github.com/gracia-labs/web-sdk) |

## Contents

```
CMakeLists.txt                 Extracts the prebuilt SDK and exposes gracia::sdk
AGENTS.md                      Rules for a player: the device, the clock, the draw
artifacts/
  windows.zip                  Prebuilt SDK (include/, gracia_sdk.dll, gracia_sdk.lib)
  android.zip                  Prebuilt SDK (include/, libgracia_sdk.so, arm64-v8a)
  linux.zip                    Prebuilt SDK (include/, libgracia_sdk.so, x86_64)
cmake/CPM.cmake                CPM.cmake package manager (used by the examples)
deps/                          Vulkan headers, GLM, volk, GLFW, OpenXR, Dear ImGui,
                               portable-file-dialogs
docker/linux.Dockerfile        Toolchain image of the Linux build of the examples
examples/
  common/                      Shared viewer core (Vulkan setup, scenes, playback)
  desktop_viewer/              Vulkan + Dear ImGui desktop scene viewer
  xr_viewer/                   OpenXR + Vulkan stereo headset viewer
package.json                   The build:linux command
prebuilt/
  linux-x86_64.zip             Linux examples, ready to run (see Linux build)
scripts/build-linux.js         Builds prebuilt/linux-x86_64.zip in Docker
```

The archives ship prebuilt in this repository. Thus the examples build without
a source checkout of the SDK core. Every other component ships in `deps/` as well,
and [CPM](https://github.com/cpm-cmake/CPM.cmake) reads the archives from disk.

## Prerequisites

- Windows 10/11 or Linux x86_64 with a Vulkan 1.3 capable GPU and current drivers.
- CMake ≥ 3.21 and a C++20 compiler (MSVC or clang-cl on Windows, GCC or Clang
  on Linux).
- No network access: the build resolves every dependency from `deps/`.

## Vulkan device requirements

Your application creates the `VkInstance` and the `VkDevice`. `gvk::DeviceRequest`
in [examples/common/src/vk_common.cpp](examples/common/src/vk_common.cpp) builds
one that works.

- **Vulkan 1.3**, instance and device.
- **Extensions:** `VK_KHR_dynamic_rendering`, `VK_KHR_synchronization2`,
  `VK_KHR_push_descriptor`, and `VK_KHR_swapchain` for your own present. Vulkan 1.3 promotes everything else
  the SDK uses.
- **Features:** read `VkPhysicalDeviceVulkan11Features`, `Vulkan12Features` and
  `Vulkan13Features` with `vkGetPhysicalDeviceFeatures2` and pass the same chain
  to `vkCreateDevice`. That enables all the device supports, which is what the
  SDK needs.
- **Queues:** the three family indexes can be equal. Submit and present on the
  graphics family, index 0.

`gracia_context_create()` returns null when the device is short of any of this.

## Legacy pipeline

The SDK prepares and sorts the splats in compute shaders that use subgroup
operations. When the GPU does not support these operations, the renderer uses a
second set of shaders that has no subgroup operations. The renderer selects the
pipeline when you create it.

- The legacy pipeline gives the same image. The preparation of the splats is
  approximately two times slower.

## Build toolchain of the artifacts

| | Windows | Android | Linux |
| --- | --- | --- | --- |
| Target | x64 | arm64-v8a | x86_64 |
| Compiler | clang-cl (LLVM 22) | NDK **r27d** (27.3.13750724), Android clang 18 | GCC 13 (`gcc-toolset-13`), manylinux_2_28 |
| Minimum platform | Windows 10, Vulkan 1.3 | API level 32 (Android 12L) | glibc 2.28, Vulkan 1.3 |
| C++ runtime | C++20, static CRT (`/MT`) | C++20, static libc++ (`c++_static`) | C++20, system `libstdc++.so.6` (GCC 8 or later) |
| Build | Ninja, Release | Ninja, Release | Ninja, Release |

These are the versions of the build, not a requirement of your own toolchain. A
different compiler or a newer NDK links against the artifacts correctly: the
public surface is a C ABI, and the C++ wrapper is header only. Thus there is no
C++ ABI between your code and the library.

- The Android library holds libc++ inside it. There is no `libc++_shared.so` to
  package, and the library does not conflict with the STL of your application.
  It needs only `libc`, `libm`, `libdl` and `liblog`.
- The Android build uses `ANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON`. Its segments
  align to 16 KB, thus it loads on a device with 16 KB memory pages.
- The Windows DLL uses the static CRT. No Visual C++ redistributable is needed.
- The Linux library compiles in Docker on the `manylinux_2_28` image
  (AlmaLinux 8, glibc 2.28) with GCC 13 from `gcc-toolset-13`. A Linux binary
  runs on the glibc that it compiles against and on all later versions, but not
  on an earlier one. Thus the library needs glibc 2.28 or later: Debian 10,
  Ubuntu 20.04, RHEL 8, Fedora 29 or a later release. `ldd --version` shows the
  glibc version of a system.
- `gcc-toolset-13` links the parts of the C++ runtime that are newer than GCC 8
  into the library. Thus the library needs only the system `libstdc++.so.6` of
  GCC 8 or later. Each distribution with glibc 2.28 has it.
- The Linux library needs only `libc`, `libm`, `libdl`, `libpthread`,
  `libgcc_s` and `libstdc++`. It exports only the `gracia_*` functions.
- Neither library links the Vulkan loader. The SDK loads Vulkan at run time.

## Building the examples

```sh
cmake -S . -B build
cmake --build build --config Release
```

The `gracia::sdk` imported target (defined in [CMakeLists.txt](CMakeLists.txt))
carries the include directory and links `gracia_sdk.lib`. The example copies
`gracia_sdk.dll` next to its executable automatically.

To use the SDK in your own CMake project:

```cmake
add_subdirectory(path/to/c-cpp-sdk)   # defines gracia::sdk
target_link_libraries(my_app PRIVATE gracia::sdk)
```

```cpp
#include <gracia/SDK.hpp>
```

On Android, configure with the NDK toolchain and `ANDROID_ABI=arm64-v8a`. The
same `add_subdirectory` gives `gracia::sdk`, backed by `libgracia_sdk.so`. Put
that library into `jniLibs/arm64-v8a/` of your APK.

On Linux, the same `add_subdirectory` gives `gracia::sdk`, backed by
`libgracia_sdk.so`. CMake puts the directory of the library into the `RPATH` of
your executable. When you install your application, put `libgracia_sdk.so` next
to it or on the library path.

## Linux build

`build:linux` builds both examples in Docker and writes one archive:

```sh
bun run build:linux
```

- **Needs:** Docker and [Bun](https://bun.sh). No other toolchain.
- **Output:** `prebuilt/linux-x86_64.zip`, with `libgracia_sdk.so`,
  `desktop_viewer` and `xr_viewer`.
- The image is manylinux_2_28 with GCC 14. Thus the executables run on glibc
  2.28 and later. Each build is a clean build.

To run the examples, extract the archive and start an executable from that
directory. Each executable finds `libgracia_sdk.so` next to itself.

- **Desktop viewer:** an X11 or Wayland session and a Vulkan driver.
  **File ▸ Open** uses `zenity` or `kdialog`. Without them, drop files on the window.
- **Headset viewer:** an OpenXR runtime with `XR_KHR_vulkan_enable2`, for
  example [Monado](https://monado.freedesktop.org/).

## Examples

- [desktop_viewer](examples/desktop_viewer/) — a Vulkan desktop viewer that
  loads `.ply` / `.sog` / `.guf` splat scenes and renders them with an ImGui
  control panel. It is similar to the Mac viewer of the
  [Apple SDK](https://github.com/gracia-labs/apple-sdk).
- [xr_viewer](examples/xr_viewer/) — the same viewer in stereo on a headset.
  It keeps the Vulkan setup and replaces the window and the present loop with an
  OpenXR session. The OpenXR loader comes from CPM.

Read [AGENTS.md](AGENTS.md) before you write a player of your own. It holds the
playback rules: the client owns the clock, the draw runs also while a scene
buffers, and the frame lifetime. The same rules are in the
[desktop viewer README](examples/desktop_viewer/README.md#playback-rules).

## Demo content

Scenes to play with: **https://docs.gracia.ai/demo-data**

Three ways to play a scene:

- **Local file** — download it and open it: `File > Open` in the desktop viewer, or a path
  argument.
- **Direct URL** — a direct link to the file streams as it is, with no token.
  Paste it in the URL field of the desktop viewer, or pass `--stream <url>`.
- **Gracia stream** — streaming through Gracia infrastructure, with a token:
  `--stream <url> --token <t>`, or the URL and Token fields in the desktop viewer.

A direct link works from any static host, thus it is the quickest way to try a
stream. Gracia infrastructure is the more reliable path.

They are demo content: for demonstration and evaluation, not for redistribution
or publication. See [CONTENT-LICENSE](CONTENT-LICENSE). For your own 4DGS
content, write to support@gracia.ai.

## Contributing

Contributions are welcome — bug fixes, new examples, platform fixes,
documentation.

- **Everything under the MIT part of [LICENSE](LICENSE)** — issues and pull
  requests both. Fork it, patch it, send it back.
- **The Artifacts** — issues only. They ship prebuilt, thus there is nothing to
  patch here. Report a bug with steps to reproduce it and we fix it upstream and
  ship a new build.

By opening a pull request you agree that your contribution is licensed under the
MIT License (Part B of [LICENSE](LICENSE)).
