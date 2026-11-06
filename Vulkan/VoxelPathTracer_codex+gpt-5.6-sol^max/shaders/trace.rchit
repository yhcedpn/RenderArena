#version 460
#extension GL_EXT_ray_tracing : require

struct HitPayload {
    float distance;
    vec2 barycentric;
    uint triangle;
    uint hit;
    uint frontFace;
};
layout(location = 0) rayPayloadInEXT HitPayload payload;
hitAttributeEXT vec2 attributes;

void main() {
    payload.distance = gl_HitTEXT;
    payload.barycentric = attributes;
    payload.triangle = gl_InstanceCustomIndexEXT + gl_PrimitiveID;
    payload.hit = 1u;
    payload.frontFace = gl_HitKindEXT == gl_HitKindFrontFacingTriangleEXT ? 1u : 0u;
}
