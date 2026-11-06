#version 460
#extension GL_EXT_ray_tracing : require
#include "common.glsl"

struct RadiancePayload {
    uint pathIndex;
    uint hit;
};
layout(location = 0) rayPayloadInEXT RadiancePayload radiancePayload;
hitAttributeEXT vec2 barycentrics;

void main()
{
    uint triangleIndex = gl_InstanceCustomIndexEXT + gl_PrimitiveID;
    uint i0 = indices.values[triangleIndex * 3u];
    uint i1 = indices.values[triangleIndex * 3u + 1u];
    uint i2 = indices.values[triangleIndex * 3u + 2u];
    float b0 = 1.0 - barycentrics.x - barycentrics.y;
    Vertex v0 = vertices.values[i0];
    Vertex v1 = vertices.values[i1];
    Vertex v2 = vertices.values[i2];
    vec3 outward = normalize(b0 * v0.normal.xyz + barycentrics.x * v1.normal.xyz +
                             barycentrics.y * v2.normal.xyz);
    vec2 uv = b0 * v0.uv.xy + barycentrics.x * v1.uv.xy + barycentrics.y * v2.uv.xy;
    bool front = dot(gl_WorldRayDirectionEXT, outward) < 0.0;
    HitRecord record;
    record.positionT = vec4(gl_WorldRayOriginEXT + gl_HitTEXT * gl_WorldRayDirectionEXT, gl_HitTEXT);
    record.normalFront = vec4(outward, front ? 1.0 : 0.0);
    record.uvCone = vec4(uv, 0.0, 0.0);
    record.ids = uvec4(triangles.values[triangleIndex].data.x,
                       triangles.values[triangleIndex].data.y,
                       triangleIndex, triangles.values[triangleIndex].data.z);
    hits.values[radiancePayload.pathIndex] = record;
    radiancePayload.hit = 1u;
}
