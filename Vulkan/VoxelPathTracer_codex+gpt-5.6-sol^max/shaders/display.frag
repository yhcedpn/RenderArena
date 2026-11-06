#version 460
#define VPT_FINAL_READ_ONLY
#include "common.glsl"

layout(location = 0) out vec4 outColor;

void main() {
    uvec2 coordinate = uvec2(gl_FragCoord.xy);
    uint pixel = coordinate.y * globals.extentCounts.x + coordinate.x;
    vec3 hdr = finalPixels[pixel].rgb;
    vec3 linearLdr = hdr / (vec3(1.0) + hdr);
    // The swapchain attachment is sRGB; it performs the sole transfer encoding.
    outColor = vec4(linearLdr, 1.0);
}
