#extension GL_EXT_ray_tracing : require
#include "common.glsl"

layout(set = 0, binding = 6) uniform accelerationStructureEXT sceneTlas;

struct RadiancePayload {
    uint pathIndex;
    uint hit;
};

layout(location = 0) rayPayloadEXT RadiancePayload radiancePayload;
