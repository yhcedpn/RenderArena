#version 460
#extension GL_GOOGLE_include_directive : require
#define DISPLAY_READONLY
#include "common.glsl"
layout(location=0) out vec4 outputColor;
void main(){uvec2 p=uvec2(gl_FragCoord.xy);uint pixel=p.y*params.size.x+p.x;vec3 c=vec3(0);for(uint d=0;d<3u;++d){uint slot=d*pixels()+pixel;Guide g=guides[currentGuide(slot)];vec3 v=filtered[signals()+slot].rgb;if((g.id.w&1u)!=0u)v*=g.albedo.rgb;c+=v;}c=max(c,vec3(0));outputColor=vec4(c/(1+c),1);}

