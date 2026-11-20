#version 460
// SDNX §16.120: depth (R32F, target 0) and stencil (R8, target 1) of a ZF32_X24S8 region in one pass, from the raw
// 8-byte texels the copy engine put in the scratch buffer (float depth in word 0, stencil in the low byte of word 1).
// The frontend asks for the depth and then the stencil of the same region: the second request takes the R8 target.
layout(std140, binding = 0) uniform Raw { ivec4 info; };
layout(binding = 0) uniform usamplerBuffer words;
layout(location = 0) out vec4 depth_out;
layout(location = 1) out vec4 stencil_out;
void main() {
  ivec2 p = ivec2(gl_FragCoord.xy);
  int word = info.y + (p.y * info.x + p.x) * 2;
  depth_out = vec4(uintBitsToFloat(texelFetch(words, word).x), 0.0, 0.0, 1.0);
  uint s = texelFetch(words, word + 1).x & 0xFFu;
  stencil_out = vec4(float(s) / 255.0, 0.0, 0.0, 1.0);
}
