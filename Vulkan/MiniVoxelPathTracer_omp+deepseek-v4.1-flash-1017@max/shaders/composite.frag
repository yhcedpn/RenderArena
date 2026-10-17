#version 460

#include "scene.glsl"

layout(location = 0) out vec4 outColor;

layout(std430, set = 0, binding = 10) readonly buffer SlotRadianceBuffer {
    vec4 items[];
} slotRadiance;
layout(std430, set = 0, binding = 11) readonly buffer SlotGuidesBuffer {
    Guide items[];
} slotGuides;
layout(std430, set = 0, binding = 12) readonly buffer PrimaryInfoBuffer {
    PrimaryInfo items[];
} primaryInfos;
layout(std430, set = 0, binding = 13) readonly buffer FilteredColorBuffer {
    vec4 items[];
} filteredColor;
layout(std430, set = 0, binding = 14) readonly buffer SlotTagsBuffer {
    uint items[];
} slotTags;
layout(std430, set = 0, binding = 15) readonly buffer EmissionBuffer {
    vec4 items[];
} emission;

vec3 guideAlbedo(Guide g) {
    return guideValid(g.meta.z) ? g.albedo.xyz : vec3(1.0);
}

void main() {
    uint W = pc.width;
    uint H = pc.height;
    uint N = W * H;
    ivec2 p = ivec2(gl_FragCoord.xy);
    uint pixel = uint(p.y) * W + uint(p.x);
    uint frameSlot = pc.slot;
    vec3 scene = emission.items[pixel].rgb;
    for (uint slot = 0u; slot < 3u; ++slot) {
        uint idx = slot * N + pixel;
        if (slotTags.items[idx] != pc.frameIndex) {
            continue;
        }
        Guide g = slotGuides.items[frameSlot * (3u * N) + idx];
        if (!guideValid(g.meta.z)) {
            // 没有有效 guide 的贡献仍计入本帧结果。
            scene += slotRadiance.items[idx].rgb;
            continue;
        }
        vec3 albedo = guideAlbedo(g);
        // demodulated 颜色在合成时乘回 albedo；零 albedo 通道在 demod 时已确定为零。
        scene += filteredColor.items[(slot * 2u) * N + pixel].rgb * albedo;
    }
    scene = max(scene, vec3(0.0));
    // 固定曝光 1，Reinhard c/(1+c)；写出线性 LDR，由 sRGB attachment 编码。
    vec3 ldr = scene / (1.0 + scene);
    outColor = vec4(ldr, 1.0);
}
