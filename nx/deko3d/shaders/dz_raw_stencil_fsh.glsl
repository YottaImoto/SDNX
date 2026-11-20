#version 460
// SDNX §16.113: stencil of a ZF32_X24S8 region from the raw 8-byte texels in the scratch buffer (low byte of word 1),
// into an R8 target, as dz_stencil_fsh did through the texture unit. info.x = row length in texels.
layout(std140, binding = 0) uniform Raw { ivec4 info; };
layout(binding = 0) uniform usamplerBuffer words;
layout(location = 0) out vec4 color;
void main() {
  ivec2 p = ivec2(gl_FragCoord.xy);
  int texel = p.y * info.x + p.x;
  uint s = texelFetch(words, info.y + texel * 2 + 1).x & 0xFFu;
  color = vec4(float(s) / 255.0, 0.0, 0.0, 1.0);
}
