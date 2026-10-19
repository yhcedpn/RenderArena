#version 460
#extension GL_EXT_scalar_block_layout : require

const uint DOMAIN_COUNT = 4u;

struct Guide {
    vec4 worldValid;
    vec4 virtualDepth;
    vec4 normalRoughness;
    uvec4 identity;
};

struct BranchOutput {
    vec4 signal0;
    vec4 signal1;
    vec4 signal2;
    vec4 signal3;
    vec4 emission;
    Guide guide;
    uvec4 guideMeta;
};

layout(set = 0, binding = 9, std430) readonly buffer Counters {
    uint currentCount;
    uint nextCount;
    uint shadowCount;
    uint overflowed;
};
layout(set = 0, binding = 13, std430) readonly buffer BranchOutputs { BranchOutput branchOutputs[]; };
layout(set = 0, binding = 17, std430) readonly buffer AtrousA { vec4 atrousA[]; };

layout(push_constant, scalar) uniform PushConstants {
    uint width;
    uint height;
    uint bounce;
    uint queueParity;
    uint frameSlot;
    uint frameParity;
    uint phase;
    uint iteration;
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outputColor;

uint branchIndex(uint pixel, uint branch) { return pixel * 2u + branch; }
uint domainIndex(uint pixel, uint domain) { return pixel * DOMAIN_COUNT + domain; }

uvec4 metaFor(uint pixel, uint domain) {
    BranchOutput a = branchOutputs[branchIndex(pixel, 0u)];
    BranchOutput b = branchOutputs[branchIndex(pixel, 1u)];
    if (a.guide.worldValid.w != 0.0 && (a.guideMeta.w & 0xffu) == domain) return a.guideMeta;
    if (b.guide.worldValid.w != 0.0 && (b.guideMeta.w & 0xffu) == domain) return b.guideMeta;
    return uvec4(0u, 0u, 0u, 0xffffffffu);
}

void main() {
    ivec2 coordinate = clamp(ivec2(gl_FragCoord.xy), ivec2(0), ivec2(int(pc.width) - 1, int(pc.height) - 1));
    uint pixel = uint(coordinate.y) * pc.width + uint(coordinate.x);
    vec3 hdr = vec3(0.0);
    for (uint domain = 0u; domain < DOMAIN_COUNT; ++domain) {
        vec3 signal = atrousA[domainIndex(pixel, domain)].rgb;
        uvec4 meta = metaFor(pixel, domain);
        if ((meta.w >> 8u) == 0u) signal *= vec3(uintBitsToFloat(meta.x), uintBitsToFloat(meta.y), uintBitsToFloat(meta.z));
        hdr += signal;
    }
    hdr += branchOutputs[branchIndex(pixel, 0u)].emission.rgb + branchOutputs[branchIndex(pixel, 1u)].emission.rgb;
    if (overflowed != 0u) {
        outputColor = vec4(1.0, 0.0, 1.0, 1.0);
        return;
    }
    vec3 mapped = hdr / (vec3(1.0) + hdr);
    outputColor = vec4(mapped, 1.0);
}
