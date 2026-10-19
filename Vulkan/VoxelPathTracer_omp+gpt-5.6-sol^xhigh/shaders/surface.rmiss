#version 460
#define ENABLE_RAY_TRACING_STAGE
#include "common.glsl"

layout(location = 0) rayPayloadInEXT HitRecord payload;

void main() {
    payload.baryT = vec4(0.0, 0.0, -1.0, 0.0);
    payload.identity = uvec4(INVALID_ID);
}
