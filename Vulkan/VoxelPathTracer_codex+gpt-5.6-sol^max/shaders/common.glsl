#ifndef VPT_COMMON_GLSL
#define VPT_COMMON_GLSL

#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_nonuniform_qualifier : require

#define PI 3.14159265358979323846
#define DOMAIN_COUNT 6u
#define DOMAIN_DIFFUSE 0u
#define DOMAIN_SPECULAR 1u
#define DOMAIN_REFLECT_DIFFUSE 2u
#define DOMAIN_REFLECT_SPECULAR 3u
#define DOMAIN_GLASS_REFLECTION 4u
#define DOMAIN_GLASS_TRANSMISSION 5u
#define DOMAIN_PENDING 0xffffffffu

#define PATH_INSIDE_GLASS (1u << 0u)
#define PATH_SPLIT_DONE (1u << 1u)
#define PATH_GLASS_BRANCH (1u << 2u)
#define PATH_TRANSMITTED (1u << 3u)
#define PATH_SUPPRESS_NEXT_EMITTER (1u << 4u)
#define PATH_LAST_DELTA (1u << 5u)

struct GlobalData {
    mat4 currentViewProjection;
    mat4 previousViewProjection;
    vec4 cameraPositionFrame;
    vec4 cameraForwardTanHalfFov;
    vec4 cameraRightAspect;
    vec4 cameraUpNear;
    vec4 jitterCurrentPrevious;
    uvec4 extentCounts;
    uvec4 flags;
    uvec4 sceneIds;
};

struct Vertex {
    vec4 positionU;
    vec4 normalV;
};

struct TriangleInfo {
    uvec4 indicesMaterial;
    uvec4 identity;
};

struct MaterialData {
    vec4 baseColor;
    vec4 emission;
    vec4 absorption;
    vec4 params;
};

struct LightData {
    vec4 p0Area;
    vec4 p1Cdf;
    vec4 p2SelectionPdf;
    vec4 normalTriangle;
};

struct PathState {
    vec4 originCone;
    vec4 directionSpread;
    vec4 throughput;
    vec4 bsdfMedium;
    vec4 virtualRow0;
    vec4 virtualRow1;
    vec4 virtualRow2;
    uvec4 ids;
    uvec4 optical;
};

struct HitRecord {
    vec4 positionT;
    vec4 normalHit;
    vec4 uvCone;
    uvec4 ids;
};

struct Guide {
    vec4 positionDepth;
    vec4 normalRoughness;
    vec4 virtualPositionDepth;
    vec4 virtualNormalDistance;
    vec4 albedo;
    uvec4 ids;
};

struct BranchOutput {
    vec4 radiance;
    vec4 analyticEmission;
    Guide guide;
    uvec4 meta;
};

struct ShadowTask {
    vec4 originTMax;
    vec4 direction;
    vec4 contribution;
    uvec4 meta;
};

struct ShadingPoint {
    vec4 position;
    vec4 normalRoughness;
    vec4 albedo;
    uvec4 ids;
};

struct Reservoir {
    uvec4 selected;
    vec4 weights;
};

struct GuidePixel {
    Guide guides[2];
};

struct SignalPixel {
    vec4 signal[6];
    vec4 emission;
};

struct TemporalPixel {
    vec4 meanVariance[6];
    vec4 momentsLength[6];
};

struct FilterPixel {
    vec4 valueVariance[6];
};

layout(set = 0, binding = 0, scalar) readonly buffer GlobalsBuffer { GlobalData globals; };
layout(set = 0, binding = 1, scalar) readonly buffer VerticesBuffer { Vertex vertices[]; };
layout(set = 0, binding = 2, scalar) readonly buffer TrianglesBuffer { TriangleInfo triangles[]; };
layout(set = 0, binding = 3, scalar) readonly buffer MaterialsBuffer { MaterialData materials[]; };
layout(set = 0, binding = 4, scalar) readonly buffer LightsBuffer { LightData lights[]; };
layout(set = 0, binding = 5, scalar) buffer PathsABuffer { PathState pathsA[]; };
layout(set = 0, binding = 6, scalar) buffer PathsBBuffer { PathState pathsB[]; };
layout(set = 0, binding = 7, scalar) buffer HitsBuffer { HitRecord hits[]; };
layout(set = 0, binding = 8, scalar) buffer BranchOutputsBuffer { BranchOutput branchOutputs[]; };
layout(set = 0, binding = 9, scalar) buffer CountersBuffer {
    uint queueCount[2];
    uint shadowCount;
    uint gpuError;
} counters;
layout(set = 0, binding = 10, scalar) buffer ShadowTasksBuffer { ShadowTask shadowTasks[]; };
layout(set = 0, binding = 11, scalar) buffer ShadowResultsBuffer { uint shadowResults[]; };
layout(set = 0, binding = 12, scalar) buffer ShadingPointsBuffer { ShadingPoint shadingPoints[]; };
layout(set = 0, binding = 13, scalar) buffer ReservoirInitialBuffer { Reservoir reservoirInitial[]; };
layout(set = 0, binding = 14, scalar) buffer ReservoirTemporalBuffer { Reservoir reservoirTemporal[]; };
layout(set = 0, binding = 15, scalar) buffer ReservoirSpatialBuffer { Reservoir reservoirSpatial[]; };
layout(set = 0, binding = 16, scalar) buffer ReservoirHistory0Buffer { Reservoir reservoirHistory0[]; };
layout(set = 0, binding = 17, scalar) buffer ReservoirHistory1Buffer { Reservoir reservoirHistory1[]; };
layout(set = 0, binding = 18, scalar) buffer GuidesCurrentBuffer { GuidePixel guidesCurrent[]; };
layout(set = 0, binding = 19, scalar) buffer GuidesHistory0Buffer { GuidePixel guidesHistory0[]; };
layout(set = 0, binding = 20, scalar) buffer GuidesHistory1Buffer { GuidePixel guidesHistory1[]; };
layout(set = 0, binding = 21, scalar) buffer SignalsCurrentBuffer { SignalPixel signalsCurrent[]; };
layout(set = 0, binding = 22, scalar) buffer Temporal0Buffer { TemporalPixel temporal0[]; };
layout(set = 0, binding = 23, scalar) buffer Temporal1Buffer { TemporalPixel temporal1[]; };
layout(set = 0, binding = 24, scalar) buffer FilterPingBuffer { FilterPixel filterPing[]; };
layout(set = 0, binding = 25, scalar) buffer FilterPongBuffer { FilterPixel filterPong[]; };
#ifdef VPT_FINAL_READ_ONLY
layout(set = 0, binding = 26, scalar) readonly buffer FinalPixelsBuffer { vec4 finalPixels[]; };
#else
layout(set = 0, binding = 26, scalar) buffer FinalPixelsBuffer { vec4 finalPixels[]; };
#endif
layout(set = 0, binding = 27, scalar) buffer IndirectBuffer { uvec4 indirectCommands[]; };
layout(set = 0, binding = 28) uniform sampler2DArray baseColorTextures;
layout(set = 0, binding = 29) uniform sampler2DArray roughnessTextures;

layout(push_constant, scalar) uniform PushConstants {
    uvec4 u0;
    uvec4 u1;
    vec4 f0;
    vec4 f1;
} pushData;

uint pixelCount() { return globals.extentCounts.x * globals.extentCounts.y; }
uint branchIndex(uint pixel, uint branch) { return branch * pixelCount() + pixel; }

uint pcgHash(uint value) {
    uint state = value * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float randomFloat(uint pixel, uint frame, uint dimension, uint branch) {
    uint seed = pixel * 0x9e3779b9u ^ frame * 0x85ebca6bu ^
                dimension * 0xc2b2ae35u ^ branch * 0x27d4eb2fu;
    return float(pcgHash(seed) >> 8u) * (1.0 / 16777216.0);
}

vec3 safeNormalize(vec3 value) {
    float l2 = dot(value, value);
    return l2 > 1.0e-20 ? value * inversesqrt(l2) : vec3(0.0, 1.0, 0.0);
}

float luminance(vec3 value) { return dot(value, vec3(0.2126, 0.7152, 0.0722)); }

vec3 transformPoint(PathState path, vec3 p) {
    return vec3(dot(path.virtualRow0.xyz, p) + path.virtualRow0.w,
                dot(path.virtualRow1.xyz, p) + path.virtualRow1.w,
                dot(path.virtualRow2.xyz, p) + path.virtualRow2.w);
}

vec3 transformDirection(PathState path, vec3 v) {
    return safeNormalize(vec3(dot(path.virtualRow0.xyz, v), dot(path.virtualRow1.xyz, v),
                              dot(path.virtualRow2.xyz, v)));
}

mat3 reflectionMatrix(vec3 n) {
    return mat3(1.0) - 2.0 * outerProduct(n, n);
}

void appendReflectionTransform(inout PathState path, vec3 planePoint, vec3 normal) {
    mat3 oldLinear = mat3(path.virtualRow0.xyz, path.virtualRow1.xyz,
                          path.virtualRow2.xyz);
    // The rows above form a transposed GLSL mat3; use explicit row products.
    mat3 reflectM = reflectionMatrix(normal);
    vec3 reflectionTranslation = 2.0 * normal * dot(normal, planePoint);
    vec3 r0 = vec3(dot(path.virtualRow0.xyz, reflectM[0]),
                   dot(path.virtualRow0.xyz, reflectM[1]),
                   dot(path.virtualRow0.xyz, reflectM[2]));
    vec3 r1 = vec3(dot(path.virtualRow1.xyz, reflectM[0]),
                   dot(path.virtualRow1.xyz, reflectM[1]),
                   dot(path.virtualRow1.xyz, reflectM[2]));
    vec3 r2 = vec3(dot(path.virtualRow2.xyz, reflectM[0]),
                   dot(path.virtualRow2.xyz, reflectM[1]),
                   dot(path.virtualRow2.xyz, reflectM[2]));
    path.virtualRow0 = vec4(r0, path.virtualRow0.w + dot(path.virtualRow0.xyz, reflectionTranslation));
    path.virtualRow1 = vec4(r1, path.virtualRow1.w + dot(path.virtualRow1.xyz, reflectionTranslation));
    path.virtualRow2 = vec4(r2, path.virtualRow2.w + dot(path.virtualRow2.xyz, reflectionTranslation));
}

vec3 offsetRay(vec3 position, vec3 geometricNormal, vec3 direction) {
    // 1e-3 world units is small relative to the unit voxel while remaining
    // well above float ulp at the scene's maximum coordinate.
    return position + geometricNormal * (dot(direction, geometricNormal) >= 0.0 ? 1.0e-3 : -1.0e-3);
}

Guide emptyGuide() {
    Guide g;
    g.positionDepth = vec4(0.0);
    g.normalRoughness = vec4(0.0);
    g.virtualPositionDepth = vec4(0.0);
    g.virtualNormalDistance = vec4(0.0);
    g.albedo = vec4(0.0);
    g.ids = uvec4(0u);
    return g;
}

uint packedGuideMeta(uint domain, uint chainLength, uint terminalType, uint transmission) {
    return (domain & 255u) | ((chainLength & 255u) << 8u) |
           ((terminalType & 255u) << 16u) | ((transmission & 1u) << 24u);
}

uint guideDomain(Guide guide) { return guide.ids.w & 255u; }
uint guideChainLength(Guide guide) { return (guide.ids.w >> 8u) & 255u; }
uint guideTerminalType(Guide guide) { return (guide.ids.w >> 16u) & 255u; }

Guide findGuide(GuidePixel pixel, uint domain) {
    if (pixel.guides[0].ids.x != 0u && guideDomain(pixel.guides[0]) == domain)
        return pixel.guides[0];
    if (pixel.guides[1].ids.x != 0u && guideDomain(pixel.guides[1]) == domain)
        return pixel.guides[1];
    return emptyGuide();
}

bool guideCompatible(Guide a, Guide b, uint domain) {
    if (a.ids.x == 0u || b.ids.x == 0u || guideDomain(a) != domain || guideDomain(b) != domain)
        return false;
    if (a.ids.x != b.ids.x || a.ids.y != b.ids.y || a.ids.z != b.ids.z || a.ids.w != b.ids.w)
        return false;
    float positionTolerance = 0.035 + 0.01 * max(a.positionDepth.w, b.positionDepth.w);
    if (distance(a.positionDepth.xyz, b.positionDepth.xyz) > positionTolerance) return false;
    if (dot(a.normalRoughness.xyz, b.normalRoughness.xyz) < 0.92) return false;
    return abs(a.normalRoughness.w - b.normalRoughness.w) <= 0.08;
}

#endif
