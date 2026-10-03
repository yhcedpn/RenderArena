#version 460
#extension GL_EXT_scalar_block_layout : require
layout(set=0,binding=6,scalar) readonly buffer Frame {
 vec4 cam[8];uvec4 extent;vec4 jitter;uvec4 settings;
} f;
layout(set=0,binding=23,scalar) readonly buffer OutputData {vec4 v[];} outputBuffer;
layout(location=0) out vec4 color;
void main(){uvec2 p=uvec2(gl_FragCoord.xy);vec3 c=outputBuffer.v[p.y*f.extent.x+p.x].rgb;color=vec4(c/(1+c),1);}
