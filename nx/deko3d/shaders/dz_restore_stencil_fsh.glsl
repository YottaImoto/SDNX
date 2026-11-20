#version 460
// SDNX §16.106: the packed stencil (1 byte per pixel) back into a depth/stencil target without stencil export: one
// pass clears the bits (stencil replace with reference 0, no discard: mode.x = 8), then one pass per bit b keeps only
// the pixels whose byte has bit b (mode.x = b) and replaces with reference 0xFF under write mask 1 << b.
// at = (destination x, y, row length in pixels, byte offset of the data in the buffer).
layout(std140, binding = 0) uniform Restore { ivec4 at; uvec4 mode; };
layout(binding = 0) uniform usamplerBuffer words;
void main() {
  if (mode.x >= 8u) return;
  ivec2 p = ivec2(gl_FragCoord.xy) - at.xy;
  int byte = at.w + p.y * at.z + p.x;
  uint v = (texelFetch(words, byte >> 2).x >> uint((byte & 3) * 8)) & 0xFFu;
  if (((v >> mode.x) & 1u) == 0u) discard;
}
