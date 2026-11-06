#version 460
#define FINAL_COLOR_READ_ONLY
#include "common.glsl"

layout(location = 0) out vec4 outputColor;

void main()
{
    FrameData frame = frameData();
    uvec2 pixel = uvec2(gl_FragCoord.xy);
    uint index = pixelIndex(pixel, frame.extentFrameFlags.xy);
    vec3 hdr = finalColors.values[index].xyz;
    vec3 mapped = hdr / (vec3(1.0) + hdr);
    outputColor = vec4(mapped, 1.0);
}
