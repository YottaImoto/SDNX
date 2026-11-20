#version 460
// SDNX §16.98 self-test: writes the value the vertex shader read (its bits) into an R32F target.
layout(location = 0) flat in uint value;
layout(location = 0) out vec4 color;
void main() { color = vec4(uintBitsToFloat(value), 0.0, 0.0, 1.0); }
