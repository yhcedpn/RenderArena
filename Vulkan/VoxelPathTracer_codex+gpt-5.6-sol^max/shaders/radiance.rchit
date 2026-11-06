#version 460
#extension GL_EXT_ray_tracing : require
#include "common.glsl"

layout(location = 0) rayPayloadInEXT uint payloadIndex;
hitAttributeEXT vec2 barycentrics;

void main() {
    uint triangleIndex = gl_InstanceCustomIndexEXT + gl_PrimitiveID;
    TriangleInfo triangle = triangles[triangleIndex];
    Vertex a = vertices[triangle.indicesMaterial.x];
    Vertex b = vertices[triangle.indicesMaterial.y];
    Vertex c = vertices[triangle.indicesMaterial.z];
    vec3 weights = vec3(1.0 - barycentrics.x - barycentrics.y,
                        barycentrics.x, barycentrics.y);
    vec3 normal = safeNormalize(a.normalV.xyz * weights.x + b.normalV.xyz * weights.y +
                                c.normalV.xyz * weights.z);
    vec2 uv = vec2(a.positionU.w, a.normalV.w) * weights.x +
              vec2(b.positionU.w, b.normalV.w) * weights.y +
              vec2(c.positionU.w, c.normalV.w) * weights.z;
    bool frontFace = dot(gl_WorldRayDirectionEXT, normal) < 0.0;
    HitRecord hit;
    hit.positionT = vec4(gl_WorldRayOriginEXT + gl_HitTEXT * gl_WorldRayDirectionEXT,
                         gl_HitTEXT);
    hit.normalHit = vec4(normal, 1.0);
    hit.uvCone = vec4(uv, 0.0, 0.0);
    hit.ids = uvec4(triangle.indicesMaterial.w, triangle.identity.x, triangleIndex,
                    frontFace ? 1u : 0u);
    hits[payloadIndex] = hit;
}
