#version 460
#extension GL_EXT_ray_tracing : require
#include "common.glsl"

layout(std430, set = 0, binding = 9) buffer HitsBuf { Hit hits[]; };
layout(std430, set = 0, binding = 19) buffer ShadowResultsBuf { uvec4 shadowResults[]; };

struct Payload {
    uint slot;
    uint shadow;
};
layout(location = 0) rayPayloadInEXT Payload payload;

void main() {
    if (payload.shadow != 0u) {
        shadowResults[payload.slot] = uvec4(0u, 0u, 0u, 0u);
        return;
    }
    Hit h;
    h.t = -1.0;
    h.instance = 0u;
    h.primitive = 0u;
    h.baryX = 0.0;
    h.baryY = 0.0;
    h.pad0 = 0.0;
    h.pad1 = 0.0;
    h.pad2 = 0.0;
    hits[payload.slot] = h;
}
