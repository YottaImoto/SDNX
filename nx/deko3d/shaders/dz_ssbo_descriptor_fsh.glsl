#version 460
// Diagnostic only. build.sh changes this UBO's LDC bank from 17 to driver bank 0.
// Fragment storage slot 3 is at 0x760. Eight pixels encode its four words as
// exact 16-bit numeric floats, avoiding denormal flushing and NaN conversion.
// §16.101: components are picked with constant indices. uam compiles a dynamic
// component index into a UBO vec4 as (offset & ~15), always reading component 0.
layout(binding = 15, std140) uniform Driver { uvec4 words[119]; } driver;
layout(location = 0) out vec4 color;
void main() {
    uint x = uint(gl_FragCoord.x);
    uvec4 d = driver.words[118];
    uint v = x < 2u ? d.x : (x < 4u ? d.y : (x < 6u ? d.z : d.w));
    color = vec4(float((v >> ((x % 2u) * 16u)) & 65535u), 0, 0, 1);
}
