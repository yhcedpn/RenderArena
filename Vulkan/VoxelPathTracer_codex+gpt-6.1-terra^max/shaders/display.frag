#version 460
#extension GL_GOOGLE_include_directive : require
#define DISPLAY_SHADER
#include "common.glsl"
layout(location=0) out vec4 color;
void main() {
    uvec2 pixel=uvec2(gl_FragCoord.xy); uint i=pixel.y*pc.width+pixel.x, n=pc.width*pc.height,phase=pc.frame&1u;
    Guide guide=guides[phase*n+i]; bool ordinary=guide.type<=1u; vec3 hdr=vec3(0);
    if (ordinary) {
        hdr=filters[n+i].color;
        if (guide.type==0u) hdr*=guide.albedo;
    }
    if (!ordinary || signalFlags[i]!=0u) {
        for (uint branch=0u;branch<2u;++branch) {
            OpticalMeta meta=opticalMetadata[(phase*2u+branch)*n+i];
            vec3 signal=radiance[2u*i+branch].rgb;
            if (meta.valid!=0u) {
                Guide terminal=branch==0u ? opticalGuides0[phase*n+i] : opticalGuides1[phase*n+i];
                signal=opticalFilters[(2u+branch)*n+i].color;
                if (terminal.type==0u) signal*=terminal.albedo;
            }
            hdr+=prefixOptical[2u*i+branch].rgb+signal;
        }
    }
    hdr=max(hdr,vec3(0)); color=vec4(hdr/(1.0+hdr),1.0);
}
