#version 460
// SDNX §16.92: the depth of a ZF32_X24S8 image (view dsSource = depth), texel for texel, into an R32F target.
layout(std140, binding = 0) uniform Region { ivec4 offset; };
layout(binding = 0) uniform sampler2D source;
layout(location = 0) out vec4 color;
void main() {
  color = vec4(texelFetch(source, ivec2(gl_FragCoord.xy) + offset.xy, 0).r, 0.0, 0.0, 1.0);
}
