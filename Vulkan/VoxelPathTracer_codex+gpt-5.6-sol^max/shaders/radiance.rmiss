#version 460
#include "rt_common.glsl"

void main()
{
    HitRecord record;
    record.positionT = vec4(0.0, 0.0, 0.0, -1.0);
    record.normalFront = vec4(0.0);
    record.uvCone = vec4(0.0);
    record.ids = uvec4(0xffffffffu);
    hits.values[radiancePayload.pathIndex] = record;
    radiancePayload.hit = 0u;
}
