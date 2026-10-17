#version 460

layout(push_constant) uniform PushConstants {
    uint pass;
    uint frame;
    uint width;
    uint height;
    uint domainIndex;
    uint iteration;
    uint queue;
    uint flags;
    uint pad0;
    uint pad1;
    uint pad2;
    uint pad3;
    uint pad4;
    uint pad5;
    uint pad6;
    uint pad7;
} pc;

layout(std430, set = 0, binding = 35) readonly buffer CompositeBuf { vec4 composite[]; };
layout(location = 0) out vec4 outColor;

void main() {
    uint pixel = uint(gl_FragCoord.y) * pc.width + uint(gl_FragCoord.x);
    vec3 hdr = composite[pixel].rgb;
    vec3 ldr = hdr / (1.0 + hdr);
    outColor = vec4(ldr, 1.0);
}
