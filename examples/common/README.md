# common

The shared core of the example viewers. It is platform agnostic on purpose: no
window system, no OpenXR, no UI toolkit. Both
[desktop_viewer](../desktop_viewer/) and [xr_viewer](../xr_viewer/) link it
as `gracia_demo_common`.

The split is by dependency, not by convenience. Everything here needs only the
Gracia SDK, Vulkan and GLM. Anything that needs a window, a headset or an
interface stays in the viewer that owns it.

```
include/gracia_demo/
  vk_common.hpp     Queue families, device create info, render pass, barriers
  frame_ring.hpp    Command buffers and fences for the frames in flight
  player.hpp        SDK context, scenes and the playback clock
  scene.hpp         Scene + SceneGroup (static or MINT, late-settling metadata)
  math3d.hpp        Bounding box helpers
```

## What each part is for

**`vk_common`** builds the Vulkan device the same way for both viewers. The
desktop viewer hands `DeviceRequest::info()` to `vkCreateDevice`, and the headset
viewer hands the same structure to `xrCreateVulkanDeviceKHR`, which is why the
extension list and the feature chain belong to an object with a lifetime rather
than to a function. `createColorRenderPass` takes the final layout, which is the
only difference between a window pass and an OpenXR pass.

The compute queue is deliberately unused: the family index stays `~0u`, and the
SDK leaves that queue handle null instead of creating one.

**`frame_ring`** holds the command buffers and the fences. Stay at or below the
SDK's per-frame ring depth of 3, and submit every frame on the one queue the SDK
tracks.

**`player`** owns the SDK context, the scenes and the playback clock. The clock
rules are in [../../AGENTS.md](../../AGENTS.md), and this is the only copy of
them: the client owns the clock, it steps on `dt` alone, and it parks only for a
scrub or while a scene buffers. A viewer feeds it `setBuffering()` from the draw
calls of the frame and reads the transport state back.

## Adding to it

Move something here when both viewers need it **and** it needs nothing but the
SDK, Vulkan and GLM. A helper that reaches for GLFW, ImGui or OpenXR belongs to
its viewer, however similar the two copies look.
