#version 460
#extension GL_EXT_ray_tracing : require
struct Hit {float t;uint tri;};
layout(location=0) rayPayloadInEXT Hit payload;
hitAttributeEXT vec2 bary;
void main(){payload.t=gl_HitTEXT;payload.tri=gl_InstanceCustomIndexEXT+gl_PrimitiveID;}
