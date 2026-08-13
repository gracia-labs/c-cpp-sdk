#version 450

// Decodes the composited splats from sRGB to linear, for a float swapchain.
//
// The decode belongs here and not in the splat shader. Splats composite with
// premultiplied alpha, and decoding is not linear, so decoding each splat before
// the blend is not the same image as decoding the blend:
//
//   sum(w[i] * decode(c[i]))  !=  decode(sum(w[i] * c[i]))
//
// The splats were trained against sRGB images, so the blend belongs in that
// encoded space. Decoding afterwards keeps it there, and reproduces exactly what
// the 8-bit path gets from the compositor. The gain is that the blend itself
// accumulated in 16-bit float instead of quantizing at every step.
//
// A subpass input, not a sampler: the splats pass is subpass 0 of this same
// render pass, so its result is read at the current fragment with no round trip
// to memory. Under multiview the read comes from the layer of the current view,
// which is what keeps one pass covering both eyes.
layout(input_attachment_index = 0, set = 0, binding = 0) uniform subpassInput uComposited;

layout(location = 0) out vec4 oColor;

vec3 srgbToLinear(vec3 c) {
  return mix(c / 12.92,
             pow((c + 0.055) / 1.055, vec3(2.4)),
             step(vec3(0.04045), c));
}

void main() {
  vec4 c = subpassLoad(uComposited);
  oColor = vec4(srgbToLinear(c.rgb), c.a);
}
