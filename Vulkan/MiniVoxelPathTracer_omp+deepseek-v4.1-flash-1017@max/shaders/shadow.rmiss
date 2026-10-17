#version 460
#extension GL_EXT_ray_tracing : require

#include "common.glsl"

layout(location = 0) rayPayloadInEXT HitPayload payload;

void main() {
    payload.pos = vec4(1.0, 0.0, 0.0, 0.0);
}
