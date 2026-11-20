#version 460
// SDNX §16.106: the reverse of the depth extraction. The frontend's packed depth (4 bytes per pixel, float) is read
// through the upload buffer's R32_UINT buffer texture and written as the fragment depth of a full-viewport triangle
// (depth test always, depth writes on, no colour target). at = (destination x, y, row length in pixels, byte offset
// of the data in the buffer).
layout(std140, binding = 0) uniform Restore { ivec4 at; uvec4 mode; };
layout(binding = 0) uniform usamplerBuffer words;
void main() {
  ivec2 p = ivec2(gl_FragCoord.xy) - at.xy;
  gl_FragDepth = uintBitsToFloat(texelFetch(words, (at.w >> 2) + p.y * at.z + p.x).x);
}
