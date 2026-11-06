#version 460
#extension GL_EXT_ray_tracing : require
struct Payload { float distance; uint triangle; vec2 bary; };
layout(location=0) rayPayloadInEXT Payload payload;
hitAttributeEXT vec2 barycentric;
void main() {
    payload.distance = gl_HitTEXT;
    payload.triangle = gl_InstanceCustomIndexEXT + gl_PrimitiveID;
    payload.bary = barycentric;
}
