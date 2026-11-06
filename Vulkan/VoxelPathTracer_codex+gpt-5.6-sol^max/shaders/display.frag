#version 460
#extension GL_GOOGLE_include_directive : require
#define DISPLAY_READ_ONLY
#include "common.glsl"

layout(location = 0) out vec4 outputColor;

void main() {
    ivec2 coordinate = ivec2(gl_FragCoord.xy);
    if (!inFramebuffer(coordinate)) {
        outputColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    uint pixel = linearPixel(coordinate);
    vec3 color = rawSignal(currentBranchBase(), pixel, DOMAIN_ANALYTIC);
    for (uint domainIndex = 0u; domainIndex < SVGF_DOMAIN_COUNT; ++domainIndex) {
        uint domain = signalDomainFromIndex(domainIndex);
        vec3 filtered = atrousA[atrousIndex(domainIndex, pixel)].xyz;
        BranchRecord branch = branchForDomain(currentBranchBase(), pixel, domain);
        if (branch.guide.albedoValidity.w >= 0.5 && branch.guide.identity.y < pc.extra.y &&
            materials[branch.guide.identity.y].metadata.x == MATERIAL_DIFFUSE) {
            filtered *= branch.guide.albedoValidity.xyz;
        }
        color += filtered;
    }
    // Fixed exposure-one Reinhard mapping. The sRGB swapchain attachment
    // performs the only transfer-function encoding.
    vec3 mapped = color / (vec3(1.0) + max(color, vec3(0.0)));
    outputColor = vec4(mapped, 1.0);
}
