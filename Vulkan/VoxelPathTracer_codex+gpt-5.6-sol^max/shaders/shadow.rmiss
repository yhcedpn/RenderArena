#version 460
#extension GL_EXT_ray_tracing : require
#include "common.glsl"

layout(location = 1) rayPayloadInEXT uint visible;

void main() { visible = 1u; }
