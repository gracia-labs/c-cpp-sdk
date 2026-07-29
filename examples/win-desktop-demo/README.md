# win-desktop-demo

A Windows desktop viewer for Gracia scenes. The viewer uses **Vulkan** and **Dear
ImGui**. It is the C/C++ counterpart to the Mac viewer of the Apple SDK. Open one
or more scenes, orbit or fly around them, play volumetric video, and adjust the
look of each scene.

## Features

- **Multiple scenes at once** — open several files together. `mix` composites
  them into one image. `split` shows one scene in each column that you can drag
  (an A/B wipe that shares the camera). Press **M** to toggle.
- **Static and video** — `.ply` / `.sog` / `.guf` static splats and `.mint`
  volumetric video, plus HTTP(S) MINT streams (**File ▸ Open Stream…**).
- **Transport** — for video: play/pause, a scrub bar and playback speed. The
  play button becomes a spinner while the video cannot show the current time.
- **Appearance for each scene** — Visibility and HSV (Hue / Saturation / Value)
  grade sliders. Adjust the sliders for each scene, with Reset.
- **Scene flags** — **File ▸ Set Flag…** sets an SDK flag (key + on/off) on the
  loaded scene(s).
- **Full camera** — trackball orbit, pan, dolly, and a WASD fly mode with
  inertia. The camera frames the scene bounds automatically.

The viewer recreates the renderer to match the splat budget when the scenes
change: on load, on a mix or split switch, and when the real splat count of a
video settles.

## Playback rules

These rules make the difference between a smooth player and one that flickers.
[`../../AGENTS.md`](../../AGENTS.md) holds the same list with the symptom of each
mistake.

**The client owns the clock.** The player owns time, always. The SDK never moves
the playhead. `setTime()` is legal at any moment, and a scene that buffers accepts
a new time too. The playhead steps with `dt` alone. Decode state is status for the
HUD, and it never gates the clock, with one exception: while a scene buffers, the
player does not add `dt`. The screen cannot show the new time yet, so the step
only makes the playhead run away from the picture.

**A scrub takes the clock.** While the user holds the scrub bar, the bar sets the
playhead, and `dt` does not. The player holds that state for the full drag, not
only for the frames with a new value. Between two pointer moves the clock stays
parked. `seek()` calls `setTime()` at once, because the UI runs after `advance()`
and before `prepare()`. A deferred seek draws the time of the last step instead of
the time of the request. Both mistakes make the screen alternate between two
moments of the video.

**Draw each frame, also while a scene buffers.** The SDK keeps the last decoded
frame on its display slot, so the draw calls hold the picture still. A skipped
draw gives a black frame, because the render pass clears the swapchain image. A
rewind sets and clears `buffering` many times, so a skipped draw flickers.
`buffering` drives the HUD only. The draw depends on the color view alone.

**Hold the draw calls until the GPU is done.** The viewer keeps one `FrameDraws`
for each in-flight slot, and it clears a slot only after the fence of that slot
signals. It uses three frames at a time, which matches the per-frame ring depth of
the SDK, and it submits and presents on the queue that the SDK tracks.

**Expect late metadata.** A stream reports `duration == 0`, an empty bounding box,
and a placeholder splat budget until the metadata lands. The viewer pumps the
scene, watches `isSettled()`, then rebuilds the renderers on the real budget, sets
the playback range, and frames the camera again.

**Keep the scrub bar continuous.** Dear ImGui rounds the value of a slider to the
precision of the display format, so `"%.1fs"` gives steps of 100 ms. The transport
sets `ImGuiSliderFlags_NoRoundToFormat` and reads the drag state with
`ImGui::IsItemActive()`.

## Build

From the SDK root (`public/c-cpp-sdk`):

```sh
cmake -S . -B build
cmake --build build --config Release
```

The executable goes into `build/bin/`. With a multi-config generator, the
executable goes into a subfolder for the configuration, for example
`build/bin/Release/`. The build copies `gracia_sdk.dll` next to it.

> The first configure downloads GLFW, Dear ImGui, GLM, the Vulkan headers, and
> volk with CPM. Thus it needs network access.

## Run

```sh
win_desktop_demo.exe [scene ...] [options]
```

Pass one or more scene files. Or start with none, and **drag and drop** files
onto the window (two or more files open in split mode). The supported formats are
`.ply`, `.sog`, `.guf`, and `.mint`.

Options: `--split`, `--stream <url>`, `--token <t>`, `--zoom <f>`, `--seek <s>`.

### Controls

| Input                 | Action                              |
|-----------------------|-------------------------------------|
| Left drag             | Orbit                               |
| Right / Alt drag      | Pan                                 |
| Scroll                | Zoom (fly-speed while flying)       |
| W A S D / R F / Q E   | Fly: move / up-down / roll          |
| Shift                 | Sprint                              |
| Space                 | Play / pause video                  |
| 0                     | Reset framing                       |
| M                     | Toggle mix / split                  |

## Layout

```
src/
  main.cpp          Window, ImGui (HUD/transport/appearance/menus), input, loop
  vk_context.*      Surface, swapchain and present
  splat_viewer.*    Layout, renderers, camera and appearance
  camera.*          SceneCamera + CameraInput (trackball + fly + inertia)
  split_layout.hpp  Column ratios, divider drag, scissor rects
```

The Vulkan device setup, the frame ring, the scenes and the playback clock live
in [`examples/common`](../common/), shared with the headset viewer.
