#version 460
#extension GL_GOOGLE_include_directive : require
#define RW readonly
#include "shared.glsl"
layout(location=0) out vec4 outputColor;
void main(){uvec2 xy=uvec2(gl_FragCoord.xy);uint p=xy.y*frame.extent.x+xy.x;vec3 c=vec3(0);for(uint b=0u;b<3u;++b){uint s=p*3u+b;Guide g=guides[s];vec3 f=filterB[s].xyz;if(g.albedo.w==1.0)f*=g.albedo.xyz;c+=f+signals[s*2u+1u].xyz;}outputColor=vec4(c/(vec3(1)+c),1);}
