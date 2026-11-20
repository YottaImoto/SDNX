#version 460
// SDNX §16.98 self-test: a vertex shader reads storage buffer 3 (word 128 = byte 0x200) and passes it on.
layout(binding = 3, std430) readonly buffer Probe { uint words[]; } probe;
layout(location = 0) flat out uint value;
void main() {
  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
  value = probe.words[128];
}
