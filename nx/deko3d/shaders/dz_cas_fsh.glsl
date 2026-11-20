#version 460
// SDNX §16.148: the front buffer to the screen with contrast-adaptive sharpening (after AMD FidelityFX CAS, MIT).
// The internal render scale makes the scene softer; this restores edge contrast without halos on flat areas.
// p.xy = UV of the source rectangle's far corner (source size / image size), p.zw = one source texel in UV,
// q.x = sharpness 0..1, q.yz = 1 / destination size.
layout(std140, binding = 0) uniform Cas { vec4 p; vec4 q; };
layout(binding = 0) uniform sampler2D src;
layout(location = 0) out vec4 color;

vec3 at(vec2 uv, float dx, float dy) { return textureLod(src, uv + vec2(dx, dy) * p.zw, 0.0).rgb; }

void main() {
  vec2 uv = gl_FragCoord.xy * q.yz * p.xy;
  vec3 a = at(uv, -1.0, -1.0), b = at(uv, 0.0, -1.0), c = at(uv, 1.0, -1.0);
  vec3 d = at(uv, -1.0, 0.0), e = at(uv, 0.0, 0.0), f = at(uv, 1.0, 0.0);
  vec3 g = at(uv, -1.0, 1.0), h = at(uv, 0.0, 1.0), i = at(uv, 1.0, 1.0);
  // Soft min and max of the 3x3 neighbourhood (cross plus corners).
  vec3 mn = min(min(min(d, e), min(f, b)), h);
  mn += min(mn, min(min(a, c), min(g, i)));
  vec3 mx = max(max(max(d, e), max(f, b)), h);
  mx += max(mx, max(max(a, c), max(g, i)));
  // Amount: high where there is headroom to sharpen without clipping.
  vec3 amp = clamp(min(mn, 2.0 - mx) / max(mx, vec3(1e-4)), 0.0, 1.0);
  amp = sqrt(amp);
  float peak = -1.0 / mix(8.0, 5.0, clamp(q.x, 0.0, 1.0));
  vec3 w = amp * peak;
  vec3 rgb = (b * w + d * w + f * w + h * w + e) / (1.0 + 4.0 * w);
  color = vec4(clamp(rgb, 0.0, 1.0), 1.0);
}
