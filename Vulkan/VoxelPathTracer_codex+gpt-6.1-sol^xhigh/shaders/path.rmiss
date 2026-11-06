#version 460
#extension GL_EXT_ray_tracing : require
struct Hit {vec4 data;uvec4 id;};
layout(location=0) rayPayloadInEXT Hit payload;
void main(){payload.id=uvec4(0xffffffffu);payload.data=vec4(0);}

