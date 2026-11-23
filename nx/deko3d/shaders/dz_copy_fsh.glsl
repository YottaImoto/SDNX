#version 460
// SDNX §16.201: a colour resolve / blit drawn by the 3D engine (one full-viewport triangle) instead of the 2D engine.
// Every 2D-engine blit made the GPU finish all 3D work first and idle (~60 us each, ~90 a frame: 5.7 ms of 43, most
// of them tiny: luminance and bloom chains, 18x11 .. 1200x704). The viewport is the destination rectangle.
// p.xy = source origin (texels of the source view, level 0), p.zw = source texels per destination pixel;
// q.xy = destination origin (pixels), q.zw = 1 / source view size; r.x = 1: linear filter, 0: nearest (texelFetch).
layout(std140, binding = 0) uniform Copy { vec4 p; vec4 q; vec4 r; };
layout(binding = 0) uniform sampler2D src;
layout(location = 0) out vec4 color;

void main() {
  vec2 s = p.xy + (gl_FragCoord.xy - q.xy) * p.zw;  // source position of this pixel's centre, in texels
  if (r.x > 0.5) {
    color = textureLod(src, s * q.zw, 0.0);
  } else {
    color = texelFetch(src, ivec2(floor(s)), 0);  // nearest, exact for a same-size copy
  }
}
