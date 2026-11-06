#version 460
#extension GL_EXT_ray_tracing : require
struct Payload { float distance; uint triangle; vec2 bary; };
layout(location=0) rayPayloadInEXT Payload payload;
void main() { payload.triangle = 0xffffffffu; }
