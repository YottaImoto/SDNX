#version 460
// SDNX §16.92: one triangle covering the viewport (no vertex inputs), for the depth/stencil extraction pass.
void main() {
  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
