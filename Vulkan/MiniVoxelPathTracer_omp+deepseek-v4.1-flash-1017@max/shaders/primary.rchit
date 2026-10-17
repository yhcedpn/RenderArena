#version 460
#extension GL_EXT_ray_tracing : require

#include "common.glsl"

layout(location = 0) rayPayloadInEXT HitPayload payload;

void main() {
    float t = gl_HitTEXT;
    vec3 pos = gl_WorldRayOriginEXT + gl_WorldRayDirectionEXT * t;
    payload.pos = vec4(pos, t);
    payload.normal = vec4(0.0, 0.0, 0.0, 1.0);
    payload.geom = uvec4(uint(gl_InstanceCustomIndexEXT), uint(gl_PrimitiveID), 0u, 0u);
}
