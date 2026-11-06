#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_GOOGLE_include_directive : require
#include "shared.glsl"
layout(location=0) rayPayloadInEXT Hit result;
void main(){result.ids=uvec4(0);}
