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

void main() {
    payload.distance = 0.0;
    payload.barycentric = vec2(0.0);
    payload.triangle = 0xffffffffu;
    payload.hit = 0u;
    payload.frontFace = 0u;
}
