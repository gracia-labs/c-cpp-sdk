# Gracia C/C++ SDK

The Gracia C/C++ SDK renders Gaussian-splat scenes and volumetric videos on your
own GPU device. On Windows and on Android the SDK uses **Vulkan**. The public
surface is a small C ABI (`gracia/SDK.h`) with a header-only C++ wrapper
(`gracia/SDK.hpp`).

## Platforms

This SDK ships a prebuilt binary for **Windows x64** and for **Android
arm64-v8a**. [CMakeLists.txt](CMakeLists.txt) selects the archive that matches
the target platform. The **examples are Windows only**: on Android link
`gracia::sdk` from your own build.

For other platforms use the matching Gracia SDK:

| Platform | SDK |
| --- | --- |
| macOS, iOS, visionOS | [gracia-labs/apple-sdk](https://github.com/gracia-labs/apple-sdk) |
| Python | [gracia-labs/python-sdk](https://github.com/gracia-labs/python-sdk) |
| Web | [gracia-labs/web-sdk](https://github.com/gracia-labs/web-sdk) |

## Contents

```
CMakeLists.txt                 Extracts the prebuilt SDK and exposes gracia::sdk
AGENTS.md                      Rules for a player: the clock, the draw, frame lifetime
artifacts/
  windows.zip                  Prebuilt SDK (include/, gracia_sdk.dll, gracia_sdk.lib)
  android.zip                  Prebuilt SDK (include/, libgracia_sdk.so, arm64-v8a)
cmake/CPM.cmake                CPM.cmake package manager (used by the examples)
examples/
  common/                      Shared viewer core (Vulkan setup, scenes, playback)
  win-desktop-demo/            Vulkan + Dear ImGui desktop scene viewer
  win-openxr/                  OpenXR + Vulkan stereo headset viewer
```

The archives ship prebuilt in this repository. Thus the examples build without
a source checkout of the SDK core. The examples need other components: GLFW,
Dear ImGui, GLM, the Vulkan headers, volk, and VMA. CPM fetches these components
from GitHub on the first configure.

## Prerequisites

- Windows 10/11 with a Vulkan 1.3 capable GPU and current drivers.
- CMake ≥ 3.21 and a C++20 compiler (MSVC or clang-cl).
- Network access on the first configure. The examples fetch GLFW, Dear ImGui, GLM,
  the Vulkan headers, and volk with [CPM](https://github.com/cpm-cmake/CPM.cmake).

## Build toolchain of the artifacts

| | Windows | Android |
| --- | --- | --- |
| Target | x64 | arm64-v8a |
| Compiler | clang-cl (LLVM 22) | NDK **r27d** (27.3.13750724), Android clang 18 |
| Minimum platform | Windows 10, Vulkan 1.3 | API level 32 (Android 12L) |
| C++ runtime | C++20, static CRT (`/MT`) | C++20, static libc++ (`c++_static`) |
| Build | Ninja, Release | Ninja, Release |

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

## Examples

- [win-desktop-demo](examples/win-desktop-demo/) — a Vulkan desktop viewer that
  loads `.ply` / `.sog` / `.guf` splat scenes and renders them with an ImGui
  control panel. It is similar to the Mac viewer of the
  [Apple SDK](https://github.com/gracia-labs/apple-sdk).
- [win-openxr](examples/win-openxr/) — the same viewer in stereo on a headset.
  It keeps the Vulkan setup and replaces the window and the present loop with an
  OpenXR session. The OpenXR loader comes from CPM.

Read [AGENTS.md](AGENTS.md) before you write a player of your own. It holds the
playback rules: the client owns the clock, the draw runs also while a scene
buffers, and the frame lifetime. The same rules are in the
[demo README](examples/win-desktop-demo/README.md#playback-rules).

## Demo content

Scenes to play with: **https://docs.gracia.ai/demo-data**

Three ways to play a scene:

- **Local file** — download it and open it: `File > Open` in the demo, or a path
  argument.
- **Direct URL** — a direct link to the file streams as it is, with no token.
  Paste it in the demo's URL field, or pass `--stream <url>`.
- **Gracia stream** — streaming through Gracia infrastructure, with a token:
  `--stream <url> --token <t>`, or the URL and Token fields in the demo.

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
