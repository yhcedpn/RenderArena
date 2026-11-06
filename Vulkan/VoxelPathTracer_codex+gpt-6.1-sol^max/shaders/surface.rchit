#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_GOOGLE_include_directive : require
#include "shared.glsl"
layout(location=0) rayPayloadInEXT Hit result;
hitAttributeEXT vec2 bary;
void main(){uint tri=uint(gl_InstanceCustomIndexEXT)+uint(gl_PrimitiveID);Triangle t=triangles[tri];float w=1.0-bary.x-bary.y;
    result.position=vec4(gl_WorldRayOriginEXT+gl_HitTEXT*gl_WorldRayDirectionEXT,gl_HitTEXT);result.normal=t.normal;
    result.ids=uvec4(1,tri,uint(t.uv2.z),uint(t.normal.w));result.uv=vec4(t.uv01.xy*w+t.uv01.zw*bary.x+t.uv2.xy*bary.y,0,0);}
