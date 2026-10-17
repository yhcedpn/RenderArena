#version 460
#extension GL_EXT_ray_tracing : require
#include "common.glsl"

layout(std430, set = 0, binding = 9) buffer HitsBuf { Hit hits[]; };
layout(std430, set = 0, binding = 18) readonly buffer ShadowQueueBuf { ShadowTask shadowQueue[]; };
layout(std430, set = 0, binding = 19) buffer ShadowResultsBuf { uvec4 shadowResults[]; };

struct Payload {
    uint slot;
    uint shadow;
};
layout(location = 0) rayPayloadInEXT Payload payload;

void main() {
    if (payload.shadow != 0u) {
        ShadowTask task = shadowQueue[payload.slot];
        bool match = gl_InstanceCustomIndexEXT == task.ids.x &&
                     uint(gl_PrimitiveID) == task.ids.y;
        bool frontSide = false;
        if (match) {
            InstanceDesc inst = instances[gl_InstanceCustomIndexEXT];
            Triangle tri = triangles[inst.triangleOffset + uint(gl_PrimitiveID)];
            frontSide = dot(normalize(tri.n.xyz), -gl_WorldRayDirectionEXT) > 0.0;
        }
        shadowResults[payload.slot] = uvec4((match && frontSide) ? 1u : 0u, 0u, 0u, 0u);
        return;
    }

    Hit h;
    h.t = gl_HitTEXT;
    h.instance = gl_InstanceCustomIndexEXT;
    h.primitive = uint(gl_PrimitiveID);
    // 着色阶段用 ray + t 重建位置，无需重心坐标。
    h.baryX = 0.0;
    h.baryY = 0.0;
    h.pad0 = 0.0;
    h.pad1 = 0.0;
    h.pad2 = 0.0;
    hits[payload.slot] = h;
}
