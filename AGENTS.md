# AGENTS.md

This file explains how to write a video player with the Gracia C/C++ SDK.
[`README.md`](README.md) is the reference for the build. There are two worked examples, and both obey
every rule below: [`examples/win-desktop-demo`](examples/win-desktop-demo/) on a window, and
[`examples/win-openxr`](examples/win-openxr/) on a headset.

> Write docs in Simplified Technical English (ASD-STE100): short active sentences, present tense, no
> gerund verbs, no contractions.

## Orientation

The SDK is a prebuilt binary in `artifacts/windows.zip` and `artifacts/android.zip`: a C ABI
(`gracia/SDK.h`) and a header-only C++ wrapper (`gracia/SDK.hpp`). On Windows and on Android the SDK
renders with Vulkan. The two examples are Windows only.

Correct a playback defect in the player, not in the SDK. The SDK core and the video loader are shared
with the Apple SDK and the Python SDK, and both are validated there. The Mac viewer
(`public/apple-sdk/Sources/GraciaView/SplatsSceneView.swift`) is the reference player. The Windows
demo is a port of it.

Both examples link [`examples/common`](examples/common/), which holds the Vulkan device setup, the
frame ring, the scenes and the playback clock. The clock rules below have exactly one implementation,
in `SplatsPlayer::advance`. Fix a clock defect there, not in a viewer.

## The client owns the clock

1. The player owns time, always. The SDK never moves the playhead.
2. `setTime()` is legal at any moment. A scene that buffers accepts a new time too.
3. Step the playhead with `dt` only. Wrap it at the duration.
4. Do not add `dt` while a scene buffers. The screen cannot show the new time yet, so the step has no
   value. It only makes the playhead run away from the picture.
   Derive that state from the decoder in `advance()`, with `pump()`, and never from the draw calls
   alone. A viewer skips the draw whenever it has nothing to show, and a stream has nothing to show
   until it decodes, so a buffering flag that only a completed frame can set deadlocks the stream: the
   clock runs, each step moves the target chunk, and the loader restarts its fetch before it ever
   completes one. Draw-call state is an addition to what `advance()` found, not the source of it.
5. A scrub takes the clock. While the user holds the scrub bar, the bar sets the playhead, and `dt`
   does not.
6. Hold the scrub state for the full drag, not only for the frames with a new value. Between two
   pointer moves the clock must stay parked. If playback steps the clock there, the next pointer move
   pulls it back, and the screen alternates between two moments of the video.
7. Apply a seek at once: call `setTime()` inside `seek()`. The UI runs after `advance()` and before
   `prepare()`, so a deferred seek draws the time of the last step, one frame away from the request.
8. Call `setTime()` and `pump()` one time in each frame, also when the player pauses. `pump()` is the
   only network check.
9. Decode state is status for the HUD. It never gates the clock, with the one exception in rule 4.

## The Vulkan device

Your application creates the device; the SDK never does.
[`README.md`](README.md#vulkan-device-requirements) lists what it must carry, and
`gvk::DeviceRequest` in [`examples/common`](examples/common/src/vk_common.cpp) builds it.

- Enable `VK_KHR_dynamic_rendering` and `VK_KHR_synchronization2` as extensions. The SDK calls
  the `KHR` entry points, and the promoted core 1.3 ones do not fill those slots.
- Read the supported feature chain and pass the same chain to `vkCreateDevice`.
- `gracia_context_create()` returns null on a device that is short. It is not a crash and not an
  exception, so check the return value.

## Dynamic rendering

The examples record with `vkCmdBeginRenderingKHR`, so they build no `VkRenderPass` and no
`VkFramebuffer`. `gvk::beginColorRendering` and `gvk::endColorRendering` in
[`examples/common`](examples/common/) hold the two image barriers that a render pass used to do
through `initialLayout`, `finalLayout` and a subpass dependency.

One rule follows from it: leave `GraciaRenderPass::renderPass` **null**. The SDK then builds its
pipelines for dynamic rendering. Give it a real `VkRenderPass` only when your own frame still uses
one; the SDK serves both, and it keys its pipeline cache on that handle. Either way the device needs
`VK_KHR_dynamic_rendering`, because the SDK draws splats into its own targets.

## The draw

10. Execute the draw calls in each frame, also when `DrawCalls::buffering` is true. The SDK keeps the
    last decoded frame on its display slot, so the picture holds still.
11. Do not skip the draw for a scene that buffers. The rendering scope clears the swapchain image, so
    a skipped draw gives a black frame. A rewind sets and clears `buffering` many times, and the screen
    flickers between the frame that the SDK holds and the new frame.
12. `buffering` is status for the HUD. It is not a draw gate. Gate on the color view alone:
    `!views.empty() && views[0].has_value()`.

## Frames and lifetime

13. Keep the draw calls of a frame until the GPU completes that frame. The demo holds one `FrameDraws`
    for each in-flight slot, and it clears a slot only after the fence of that slot signals.
14. Use a maximum of three frames at a time. It matches the per-frame ring depth of the SDK.
15. Submit and present on the queue that the SDK tracks: the graphics family, index 0. Another queue
    corrupts the buffer accounting of the SDK, and the splats flicker.
16. Wait for an idle device before you replace a scene or a renderer.

## Late metadata

17. A stream reports `duration == 0`, an empty bounding box, and a placeholder splat budget until the
    metadata lands. Do not gate on `duration > 0`. Pump the scene, watch `isSettled()`, then rebuild
    the renderers on the real budget, set the playback range, and frame the camera again.

## Stereo and OpenXR

18. Set `vr` to true in **both** places: `gracia_splats_renderer_create()` and `render()`. They are
    independent, and neither reports an error. With one of them false you get one view instead of
    two, both eyes show the left image, and the picture looks correct until you close one eye.
19. Call `render()` **one time for each frame**, not one time for each eye. One call returns one draw
    call for each eye. Two calls consume two of the three ring slots of the SDK, which wraps the ring
    inside two frames and gives the flicker of rule 15.
20. Build a projection for each eye from `XrView::fov`. The field of view of a headset is asymmetric
    and differs between the eyes. A symmetric projection from the aspect ratio looks correct on a
    monitor and is wrong in a headset.
21. Give the SDK the camera-to-world transform of each eye, straight from `XrPosef`. Do not invert
    it: the SDK inverts it. OpenXR and the demo camera share a convention, so no axis changes.
22. Let OpenXR pick the GPU with `xrGetVulkanGraphicsDevice2KHR`, and create the Vulkan instance and
    the device through `xrCreateVulkanInstanceKHR` and `xrCreateVulkanDeviceKHR`. Check the
    `XrResult` **and** the `vulkanResult` output: a success with a failed `vulkanResult` gives a null
    handle. Name the graphics family and queue index 0 in the graphics binding, which is the queue of
    rule 15.
23. Leave the eye image in `COLOR_ATTACHMENT_OPTIMAL`, not `PRESENT_SRC_KHR`. Transition it from
    `UNDEFINED`: the runtime promises only a compatible layout, so you cannot transition from a known
    one. `gvk::endColorRendering` takes the final layout for this reason.
24. Release a swapchain image **after** the queue submit, never before. The runtime accepts an image
    whose command buffer still runs, but not one that was only recorded.
25. Call `xrBeginFrame` and `xrEndFrame` for every frame, also when `shouldRender` is false, and keep
    `setTime()` and `pump()` running on those frames (rule 8).

## The transport UI

26. The scrub bar must be continuous. Dear ImGui rounds the value of a slider to the precision of the
    display format, so `"%.1fs"` gives steps of 100 ms. Set `ImGuiSliderFlags_NoRoundToFormat`.
27. Read the drag state with `ImGui::IsItemActive()` directly after the slider. The return value of
    the slider is true only for the frames with a new value, and that is not the full drag.

## Looks broken, but is not

- **The screen flips between two moments of the video in a rewind** → rules 4, 6, 7, 10 and 11.
- **The picture goes black in short flashes** → rule 11.
- **The scrub bar moves in steps** → rule 26.
- **Nothing decodes** → rule 8.
- **The video plays, and the playhead runs ahead of the picture** → rule 4.
- **The splats flicker in a static scene** → rule 15, and in a headset also rule 19.
- **A `.mint` is blank at the start** → it buffers. Read `DrawCalls::buffering`.
- **A stream shows an empty scene and never frames the camera** → rule 17.
- **A stream loads, reports its duration and splat budget, and then never draws, while the playhead
  runs and a scrub responds instantly** → rule 4. Nothing is decoding, and the clock has not noticed.
- **`visibleSplatsCount` is 0** → set `renderer.setFlag("splats_count_readback", true)`.
- **The headset shows the same image in both eyes** → rule 18.
- **The headset shows a black or garbled image** → rule 23.
- **The scene sits inside the user** → place it in front of the origin of the reference space. The
  bounding box of a stream is empty until the metadata lands, so place it again after rule 17.

## Verify a change

There is no automated test for the player. Build the demo, then run it against a `.mint`:

```sh
cmake --build build --config Release --target win_desktop_demo
build/bin/Release/win_desktop_demo.exe scene.mint
```

Test a rewind with the scrub bar, a loop at the end of the clip, and both layouts
(`a.mint b.sog`, and `a.mint b.mint --split`).

The headset viewer needs a runtime. The Meta XR Simulator is enough: it reports a Meta Quest 3, and
it renders the Vulkan path correctly, so no hardware is needed to test a change. Without a runtime or
a headset the viewer waits 10 seconds and exits with a message.

```sh
cmake --build build --config Release --target win_openxr_demo
build/bin/Release/win_openxr_demo.exe scene.mint
```

Look through the headset with one eye closed at a time: a stereo fault (rule 18) is invisible with
both eyes open. Compare the brightness against the desktop viewer, and try `--unorm` if it differs.
