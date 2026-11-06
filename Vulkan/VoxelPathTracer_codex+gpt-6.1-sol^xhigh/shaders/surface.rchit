#version 460
#extension GL_EXT_ray_tracing : require
struct Hit {vec4 data;uvec4 id;};
layout(location=0) rayPayloadInEXT Hit payload;
hitAttributeEXT vec2 bary;
void main(){payload.data=vec4(gl_HitTEXT,bary,0);payload.id=uvec4(gl_InstanceCustomIndexEXT+gl_PrimitiveID,0,0,0);}

