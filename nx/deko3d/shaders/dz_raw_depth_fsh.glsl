#version 460
// SDNX §16.113: depth of a ZF32_X24S8 region without the texture unit. The copy engine copied the region's raw
// 8-byte texels (float depth in word 0, stencil in the low byte of word 1) to a scratch buffer; this pass reads it
// through an R32_UINT buffer texture. Sampling the depth through a dsSource = depth view gave wrong values
// (vertical bands) on the console while the stencil read was right. info.x = row length in texels.
layout(std140, binding = 0) uniform Raw { ivec4 info; };
layout(binding = 0) uniform usamplerBuffer words;
layout(location = 0) out vec4 color;
void main() {
  ivec2 p = ivec2(gl_FragCoord.xy);
  int texel = p.y * info.x + p.x;
  color = vec4(uintBitsToFloat(texelFetch(words, info.y + texel * 2).x), 0.0, 0.0, 1.0);
}
