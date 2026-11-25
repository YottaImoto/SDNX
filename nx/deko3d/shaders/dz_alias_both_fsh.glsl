#version 460
// SDNX 16.242: depth (R32F, target 0) and stencil (R8, target 1) of a ZF32_X24S8 region in one pass, read straight
// from the depth image through a colour view of it (RG32_UINT, the same 8-byte texels the copy engine copied for
// dz_raw_both_fsh: float depth in word 0, stencil in the low byte of word 1), so the raw copy to a buffer goes away.
// info.xy: the region's origin in the source image (the targets are written from 0,0).
layout(std140, binding = 0) uniform Alias { ivec4 info; };
layout(binding = 0) uniform usampler2D texels;
layout(location = 0) out vec4 depth_out;
layout(location = 1) out vec4 stencil_out;
void main() {
  uvec2 t = texelFetch(texels, ivec2(gl_FragCoord.xy) + info.xy, 0).xy;
  depth_out = vec4(uintBitsToFloat(t.x), 0.0, 0.0, 1.0);
  stencil_out = vec4(float(t.y & 0xFFu) / 255.0, 0.0, 0.0, 1.0);
}
