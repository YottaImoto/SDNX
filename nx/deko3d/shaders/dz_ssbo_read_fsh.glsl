#version 460
// SDNX §16.98 self-test: a pixel shader reads storage buffer 3 (word 64 = byte 0x100) itself.
layout(binding = 3, std430) readonly buffer Probe { uint words[]; } probe;
layout(location = 0) out vec4 color;
void main() { color = vec4(uintBitsToFloat(probe.words[64]), 0.0, 0.0, 1.0); }
