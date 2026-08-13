# win-openxr

A stereo headset viewer for Gracia scenes. It uses **OpenXR** and **Vulkan**. It
is the same SDK orchestration as [win-desktop-demo](../win-desktop-demo/), with
the window, the swapchain and the present loop replaced by an OpenXR session.
Put the headset on, and the scene stands in front of you.

## What it does

- **Static and video** — `.ply` / `.sog` / `.guf` static splats and `.mint`
  volumetric video, plus HTTP(S) MINT streams (`--stream`).
- **Stereo** — one renderer, one `render()` call for each frame, and **one** draw
  call for both eyes (multiview). One swapchain of two array layers, one pass.
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
cmake --build build --config Release --target win_openxr_demo
```

The first configure downloads the OpenXR loader with CPM, together with GLM, the
Vulkan headers and volk. `KhronosGroup/OpenXR-SDK` is the release repository: the
generated sources are committed, so the build needs no Python and no Jinja2. The
loader is static, so there is no second DLL to copy. The build puts
`gracia_sdk.dll` next to the executable.

## Run

```sh
win_openxr_demo.exe <scene.ply|.sog|.guf|.mint> [more scenes ...] [options]
```

| Option | Effect |
|---|---|
| `--stream <url>` / `--token <t>` | Open an HTTP(S) MINT stream. |
| `--seek <s>` | Start the video at this time. |
| `--scale <m>` | Target scene radius in metres (default 0.75). |
| `--distance <m>` | Metres in front of the origin (default 2.0). |
| `--height <m>` | Metres above the origin (default 0.0). |
| `--fp16` | `RGBA16F` target instead of 8-bit sRGB. See Colors. |

A runtime must be installed. A headset is not needed: the Meta XR Simulator
reports a Meta Quest 3 and renders the Vulkan path. Without a runtime or a
headset the viewer waits 10 seconds, prints a message and exits. It does not
crash.

## Stereo

The viewer uses multiview. The swapchain holds two array layers, the image views
are `2D_ARRAY` over both, and the render pass carries a view mask of `0b11`
(`VkRenderPassMultiviewCreateInfo`). One pass then writes both eyes: every draw
runs once per layer. `render()` is given `GRACIA_STEREO_MODE_MULTIVIEW` to match
the pass, and returns **one** draw call, not one per eye. Both composition-layer
views name the one swapchain and differ only by `imageArrayIndex`.

A framebuffer for a multiview pass declares `layers = 1`. The view mask says how
many layers the pass writes, not the framebuffer.

## Colors

Splats are trained in sRGB, so the SDK writes values that are already encoded.

By default the swapchain is declared `*_SRGB` and rendered through the `*_UNORM`
twin of the same compatibility class. The compositor then decodes on read, and
our write encodes nothing, so the values survive the trip unchanged. Declaring
the swapchain `*_UNORM` instead would leave the compositor reading encoded values
as linear and wash the picture out. If a runtime offers no `*_SRGB` format the
viewer falls back to `*_UNORM` and says so.

`--fp16` asks for `R16G16B16A16_SFLOAT`, so the blend of many overlapping splats
accumulates in float instead of quantizing to 256 levels at every step. A float
format has no sRGB twin to alias and a runtime reads it as linear, so the encoded
values have to be decoded before the runtime sees them. Vulkan will not do it for
free: an `*_SRGB` attachment *encodes* on write, and there is no decode-on-write.

So the viewer decodes itself, in a second subpass of the same render pass. The
splats blend into a float intermediate in subpass 0, and subpass 1 reads that as
a **subpass input** and writes the decoded result into the swapchain. The two
shaders in [`shaders/`](shaders/) are the only ones in the examples; CMake
compiles them with `glslc -mfmt=c` and `#include`s the result, so there is no
`.spv` file to find at runtime.

The decode belongs after the blend and not in the splat shader. Decoding is not
linear, so `sum(w·decode(c))` is not `decode(sum(w·c))`, and a shader sees one
splat at a time — it cannot defer past a blend it does not own. Decoding per
splat would move compositing into linear space, away from the space the splats
were trained in. Decoding afterwards reproduces exactly what the compositor does
for the 8-bit path, which is why `--fp16` gains precision without changing the
look.

Replay the splat draw calls in the first subpass of the pass. A later subpass
does not accept them.

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
