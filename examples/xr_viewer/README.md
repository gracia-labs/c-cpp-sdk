# xr_viewer

A stereo headset viewer for Gracia scenes. It uses **OpenXR** and **Vulkan**. It
is the same SDK orchestration as [desktop_viewer](../desktop_viewer/), with
the window, the swapchain and the present loop replaced by an OpenXR session.
Put the headset on, and the scene stands in front of you.

## What it does

- **Static and video** — `.ply` / `.sog` / `.guf` static splats and `.mint`
  volumetric video, plus HTTP(S) MINT streams (`--stream`).
- **Stereo** — one renderer, one `render()` call for each frame, and one draw
  call for each eye (multipass stereo).
- **Head tracking** — the runtime supplies the pose and the projection of each
  eye. The scene is placed in front of the origin of the reference space.
- **Video playback** — the same clock as the desktop viewer. The video loops.

## Shared code

The Vulkan device setup, the frame ring, the scenes and the playback clock come
from [`examples/common`](../common/) as `gracia_demo_common`. What stays here is
only what OpenXR makes different: the session and swapchains, the stereo pairing,
and the pose and projection maths.

## Build

From the SDK root (`public/c-cpp-sdk`):

```sh
cmake -S . -B build
cmake --build build --config Release --target xr_viewer
```

The first configure downloads the OpenXR loader with CPM, together with GLM, the
Vulkan headers and volk. `KhronosGroup/OpenXR-SDK` is the release repository: the
generated sources are committed, so the build needs no Python and no Jinja2. The
loader is static, so there is no second DLL to copy. The build puts
`gracia_sdk.dll` next to the executable.

## Run

```sh
xr_viewer.exe <scene.ply|.sog|.guf|.mint> [more scenes ...] [options]
```

| Option | Effect |
|---|---|
| `--stream <url>` / `--token <t>` | Open an HTTP(S) MINT stream. |
| `--seek <s>` | Start the video at this time. |
| `--scale <m>` | Target scene radius in metres (default 0.75). |
| `--distance <m>` | Metres in front of the origin (default 2.0). |
| `--height <m>` | Metres above the origin (default 0.0). |

A runtime must be installed. A headset is not needed: the Meta XR Simulator
reports a Meta Quest 3 and renders the Vulkan path. Without a runtime or a
headset the viewer waits 10 seconds, prints a message and exits. It does not
crash.

## Colors

Splats are trained in sRGB, so the SDK writes values that are already encoded.
The viewer asks for a `*_UNORM` swapchain, which hands those values to the
compositor untouched. An `*_SRGB` target would encode them a second time and
wash the picture out. The swapchain format, the render pass attachment and the
format given to the SDK all come from one variable, so they cannot disagree.

If a runtime offers no `*_UNORM` format the viewer takes sRGB and says so.

## Limits

No overlay, no mirror window, no controller input, no camera controls (the head
pose is the camera), no split layout and no depth-as-color. Scenes come from the
command line. The rules the viewer obeys are in
[../../AGENTS.md](../../AGENTS.md).

Validation layers report a `WRITE_AFTER_WRITE` hazard inside the runtime's own
`xrEndFrame`. It comes from the runtime, not from this code.

## Layout

```
src/
  main.cpp        Arg parsing, session loop, per-eye record
  xr_context.*    OpenXR instance/session/swapchains + the Vulkan device it picks
  xr_viewer.*     The stereo renderer and the scene placement
  xr_math.hpp     XrFovf projection, XrPosef pose, scene placement
```
