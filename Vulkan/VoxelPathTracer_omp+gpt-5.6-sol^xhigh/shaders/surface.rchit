#version 460
#define ENABLE_RAY_TRACING_STAGE
#include "common.glsl"

hitAttributeEXT vec2 attributes;
layout(location = 0) rayPayloadInEXT HitRecord payload;

void main() {
    payload.baryT = vec4(attributes, gl_HitTEXT, 1.0);
    payload.identity = uvec4(gl_PrimitiveID, gl_InstanceCustomIndexEXT, gl_HitKindEXT, 0u);
}
