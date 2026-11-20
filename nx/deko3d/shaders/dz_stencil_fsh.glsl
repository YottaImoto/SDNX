#version 460
// SDNX §16.92: the stencil of a depth/stencil image (view dsSource = stencil), texel for texel, into an R8 target.
layout(std140, binding = 0) uniform Region { ivec4 offset; };
layout(binding = 0) uniform usampler2D source;
layout(location = 0) out vec4 color;
void main() {
  uint s = texelFetch(source, ivec2(gl_FragCoord.xy) + offset.xy, 0).r & 0xFFu;
  color = vec4(float(s) / 255.0, 0.0, 0.0, 1.0);
}
