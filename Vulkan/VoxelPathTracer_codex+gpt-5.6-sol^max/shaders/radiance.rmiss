#version 460
#extension GL_EXT_ray_tracing : require
#include "common.glsl"

layout(location = 0) rayPayloadInEXT uint payloadIndex;

void main() {
    HitRecord hit;
    hit.positionT = vec4(0.0, 0.0, 0.0, 1.0e4);
    hit.normalHit = vec4(0.0);
    hit.uvCone = vec4(0.0);
    hit.ids = uvec4(0u);
    hits[payloadIndex] = hit;
}
