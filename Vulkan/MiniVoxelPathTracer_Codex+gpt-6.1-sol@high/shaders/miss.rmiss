#version 460
#extension GL_EXT_ray_tracing : require
struct Hit {float t;uint tri;};
layout(location=0) rayPayloadInEXT Hit payload;
void main(){payload.t=0;payload.tri=0xffffffffu;}
