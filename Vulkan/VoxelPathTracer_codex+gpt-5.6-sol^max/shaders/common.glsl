#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_nonuniform_qualifier : require

const uint INVALID_INDEX = 0xffffffffu;
const uint FLAG_INSIDE_MEDIUM = 1u << 0;
const uint FLAG_GLASS_SPLIT = 1u << 1;
const uint FLAG_HAS_TRANSMISSION = 1u << 2;
const uint FLAG_LAST_DELTA = 1u << 3;
const uint FLAG_SUPPRESS_EMITTER = 1u << 4;
const uint FLAG_OPTICAL_MIRROR = 1u << 5;
const uint FLAG_OPTICAL_GLASS_REFLECT = 1u << 6;
const uint FLAG_OPTICAL_GLASS_TRANSMIT = 1u << 7;
const uint FLAG_PRIMARY_SEGMENT = 1u << 8;

const uint DOMAIN_UNCLASSIFIED = 0u;
const uint DOMAIN_ANALYTIC = 1u;
const uint DOMAIN_ORDINARY_DIFFUSE = 2u;
const uint DOMAIN_ORDINARY_SPECULAR = 3u;
const uint DOMAIN_REFLECTION_DIFFUSE = 4u;
const uint DOMAIN_REFLECTION_SPECULAR = 5u;
const uint DOMAIN_GLASS_REFLECTION = 6u;
const uint DOMAIN_GLASS_TRANSMISSION = 7u;
const uint SVGF_DOMAIN_COUNT = 6u;

const uint MATERIAL_DIFFUSE = 0u;
const uint MATERIAL_ROUGH_METAL = 1u;
const uint MATERIAL_MIRROR = 2u;
const uint MATERIAL_DIELECTRIC = 3u;
const uint MATERIAL_EMISSIVE = 4u;

struct Vertex {
    vec4 positionU;
    vec4 normalV;
    uvec4 metadata;
};

struct Triangle {
    uvec4 metadata;
    vec4 geometry;
};

struct Material {
    vec4 baseColorMetallic;
    vec4 emissionRoughness;
    vec4 absorptionIor;
    uvec4 metadata;
};

struct Light {
    vec4 v0Area;
    vec4 edge1SelectionPdf;
    vec4 edge2AreaPdf;
    vec4 normalCdf;
    uvec4 metadata;
};

struct PathState {
    vec4 originConeWidth;
    vec4 directionConeSpread;
    vec4 throughputLastPdf;
    uvec4 identity;
    uvec4 optical;
    vec4 reflectionRow0;
    vec4 reflectionRow1;
    vec4 reflectionRow2;
};

struct HitState {
    vec4 distanceBarycentric;
    uvec4 identity;
};

struct Guide {
    vec4 positionDistance;
    vec4 normalRoughness;
    vec4 albedoValidity;
    uvec4 identity;
};

struct BranchRecord {
    vec4 radiance;
    Guide guide;
    uvec4 metadata;
};

struct Reservoir {
    vec4 selected;
    vec4 statistics;
    uvec4 identity;
};

struct ShadowTask {
    vec4 originTMax;
    vec4 direction;
    vec4 contribution;
    uvec4 identity;
};

struct SvgfHistory {
    vec4 radianceVariance;
    vec4 momentsLength;
};

struct CameraData {
    vec4 currentPositionTanHalfFov;
    vec4 currentForwardAspect;
    vec4 currentRight;
    vec4 currentUp;
    vec4 previousPositionTanHalfFov;
    vec4 previousForwardAspect;
    vec4 previousRight;
    vec4 previousUp;
    vec4 jitterPixels;
    uvec4 frame;
};

layout(push_constant, scalar) uniform PushConstants {
    uvec4 params;
    uvec4 counts;
    uvec4 extra;
} pc;

layout(set = 0, binding = 1, std430) readonly buffer VertexBuffer { Vertex vertices[]; };
layout(set = 0, binding = 2, std430) readonly buffer IndexBuffer { uint indices[]; };
layout(set = 0, binding = 3, std430) readonly buffer TriangleBuffer { Triangle triangles[]; };
layout(set = 0, binding = 4, std430) readonly buffer MaterialBuffer { Material materials[]; };
layout(set = 0, binding = 5, std430) readonly buffer LightBuffer { Light lights[]; };
layout(set = 0, binding = 6) uniform texture2DArray baseColorTexture;
layout(set = 0, binding = 7) uniform texture2DArray roughnessTexture;
layout(set = 0, binding = 8) uniform sampler materialSampler;
layout(set = 0, binding = 9, std430) buffer PathQueueABuffer { PathState pathsA[]; };
layout(set = 0, binding = 10, std430) buffer PathQueueBBuffer { PathState pathsB[]; };
layout(set = 0, binding = 11, std430) buffer HitBuffer { HitState hitStates[]; };
layout(set = 0, binding = 12, std430) buffer CounterBuffer {
    uint countA;
    uint countB;
    uint shadowCount;
    uint overflowFlag;
};
layout(set = 0, binding = 13, std430) buffer IndirectBuffer { uvec4 indirectArguments[4]; };
layout(set = 0, binding = 14, std430) buffer ShadowTaskBuffer { ShadowTask shadowTasks[]; };
layout(set = 0, binding = 15, std430) buffer ShadowResultBuffer { uint shadowResults[]; };
#ifdef DISPLAY_READ_ONLY
layout(set = 0, binding = 16, std430) readonly buffer BranchRecordBuffer { BranchRecord branchRecords[]; };
#else
layout(set = 0, binding = 16, std430) buffer BranchRecordBuffer { BranchRecord branchRecords[]; };
#endif
layout(set = 0, binding = 17, std430) buffer ReservoirInitialBuffer { Reservoir reservoirInitial[]; };
layout(set = 0, binding = 18, std430) buffer ReservoirTemporalBuffer { Reservoir reservoirTemporal[]; };
layout(set = 0, binding = 19, std430) buffer ReservoirHistoryBuffer { Reservoir reservoirHistory[]; };
layout(set = 0, binding = 20, std430) buffer SvgfHistoryBuffer { SvgfHistory svgfHistory[]; };
#ifdef DISPLAY_READ_ONLY
layout(set = 0, binding = 21, std430) readonly buffer AtrousABuffer { vec4 atrousA[]; };
#else
layout(set = 0, binding = 21, std430) buffer AtrousABuffer { vec4 atrousA[]; };
#endif
layout(set = 0, binding = 22, std430) buffer AtrousBBuffer { vec4 atrousB[]; };
layout(set = 0, binding = 23, std430) readonly buffer CameraBuffer { CameraData cameraData[]; };

#define camera cameraData[pc.params.z & 1u]

uint pixelCount() { return pc.counts.x; }
uint branchCapacity() { return pc.counts.y; }
uint lightCount() { return pc.extra.x; }
uint frameParity() { return pc.params.z & 1u; }
uint currentBranchBase() { return frameParity() * branchCapacity(); }
uint previousBranchBase() { return (1u - frameParity()) * branchCapacity(); }
uint reservoirBase(uint parity) { return parity * pixelCount(); }
uint svgfBase(uint parity, uint domainIndex) {
    return (parity * SVGF_DOMAIN_COUNT + domainIndex) * pixelCount();
}
uint atrousIndex(uint domainIndex, uint pixel) { return domainIndex * pixelCount() + pixel; }

PathState readPath(uint queueIndex, uint index) { return queueIndex == 0u ? pathsA[index] : pathsB[index]; }
void writePath(uint queueIndex, uint index, PathState value) {
    if (queueIndex == 0u) pathsA[index] = value;
    else pathsB[index] = value;
}
uint queueCount(uint queueIndex) { return queueIndex == 0u ? countA : countB; }

uint hashBits(uint value) {
    value ^= value >> 16u;
    value *= 0x7feb352du;
    value ^= value >> 15u;
    value *= 0x846ca68bu;
    return value ^ (value >> 16u);
}

uint hashCombine(uint a, uint b) { return hashBits(a ^ (b + 0x9e3779b9u + (a << 6u) + (a >> 2u))); }

float randomDimension(uint pixel, uint branch, uint bounce, uint dimension) {
    uint value = hashBits(pixel ^ hashBits(camera.frame.z + 0x9e3779b9u));
    value = hashCombine(value, branch * 0x85ebca6bu);
    value = hashCombine(value, bounce * 0xc2b2ae35u);
    value = hashCombine(value, dimension * 0x27d4eb2fu);
    return (float(value >> 8u) + 0.5) * (1.0 / 16777216.0);
}

float luminance(vec3 value) { return dot(value, vec3(0.2126, 0.7152, 0.0722)); }
float maxComponent(vec3 value) { return max(value.x, max(value.y, value.z)); }
float powerHeuristic(float a, float b) {
    float aa = a * a;
    float bb = b * b;
    return aa / max(aa + bb, 1e-30);
}

uint selectLight(float sampleValue) {
    uint count = lightCount();
    for (uint index = 0u; index < count; ++index) {
        if (sampleValue <= lights[index].normalCdf.w) return index;
    }
    return count - 1u;
}

vec2 sampleTriangle(float u0, float u1) {
    float root = sqrt(u0);
    return vec2(1.0 - root, u1 * root);
}

vec3 lightPosition(Light light, vec2 bary) {
    return light.v0Area.xyz + bary.x * light.edge1SelectionPdf.xyz + bary.y * light.edge2AreaPdf.xyz;
}

float ggxD(float nDotH, float alpha) {
    float a2 = alpha * alpha;
    float denominator = nDotH * nDotH * (a2 - 1.0) + 1.0;
    return a2 / max(3.141592653589793 * denominator * denominator, 1e-20);
}

float smithG1(float nDotV, float alpha) {
    float a2 = alpha * alpha;
    return 2.0 * nDotV / max(nDotV + sqrt(a2 + (1.0 - a2) * nDotV * nDotV), 1e-20);
}

vec3 fresnelSchlick(float cosTheta, vec3 f0) {
    float factor = pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
    return f0 + (vec3(1.0) - f0) * factor;
}

vec3 evaluateBsdf(uint materialType, vec3 albedo, float roughness, vec3 normal, vec3 wo, vec3 wi,
                  out float pdf) {
    float nDotL = max(dot(normal, wi), 0.0);
    float nDotV = max(dot(normal, wo), 0.0);
    if (materialType == MATERIAL_DIFFUSE) {
        pdf = nDotL * (1.0 / 3.141592653589793);
        return albedo * (1.0 / 3.141592653589793);
    }
    if (materialType != MATERIAL_ROUGH_METAL || nDotL <= 0.0 || nDotV <= 0.0) {
        pdf = 0.0;
        return vec3(0.0);
    }
    vec3 halfVector = normalize(wo + wi);
    float nDotH = max(dot(normal, halfVector), 0.0);
    float vDotH = max(dot(wo, halfVector), 0.0);
    float alpha = max(roughness * roughness, 0.0025);
    float distribution = ggxD(nDotH, alpha);
    float masking = smithG1(nDotV, alpha) * smithG1(nDotL, alpha);
    vec3 fresnel = fresnelSchlick(vDotH, albedo);
    pdf = distribution * nDotH / max(4.0 * vDotH, 1e-20);
    return fresnel * (distribution * masking / max(4.0 * nDotV * nDotL, 1e-20));
}

vec3 evaluateDirectAtGuide(Guide guide, uint lightIndex, vec2 bary, out float target, out float proposalArea) {
    target = 0.0;
    proposalArea = 0.0;
    if (guide.albedoValidity.w < 0.5 || lightIndex >= lightCount()) return vec3(0.0);
    Light light = lights[lightIndex];
    vec3 lightPoint = lightPosition(light, bary);
    vec3 delta = lightPoint - guide.positionDistance.xyz;
    float distanceSquared = dot(delta, delta);
    if (distanceSquared <= 1e-10) return vec3(0.0);
    vec3 wi = delta * inversesqrt(distanceSquared);
    float receiverCosine = max(dot(guide.normalRoughness.xyz, wi), 0.0);
    float emitterCosine = max(dot(light.normalCdf.xyz, -wi), 0.0);
    if (receiverCosine <= 0.0 || emitterCosine <= 0.0) return vec3(0.0);
    Material material = materials[guide.identity.y];
    Material emitter = materials[light.metadata.y];
    vec3 wo = normalize(camera.currentPositionTanHalfFov.xyz - guide.positionDistance.xyz);
    float bsdfPdf;
    vec3 bsdf = evaluateBsdf(material.metadata.x, guide.albedoValidity.xyz, guide.normalRoughness.w,
                             guide.normalRoughness.xyz, wo, wi, bsdfPdf);
    vec3 contribution = bsdf * receiverCosine * emitter.emissionRoughness.xyz * emitterCosine / distanceSquared;
    target = max(luminance(contribution), 0.0);
    proposalArea = light.edge1SelectionPdf.w * light.edge2AreaPdf.w;
    return contribution;
}

bool guidesCompatible(Guide a, Guide b) {
    if (a.albedoValidity.w < 0.5 || b.albedoValidity.w < 0.5) return false;
    if (a.identity.x != b.identity.x || a.identity.y != b.identity.y || a.identity.z != b.identity.z ||
        a.identity.w != b.identity.w) return false;
    float tolerance = 0.025 + 0.01 * max(a.positionDistance.w, b.positionDistance.w);
    if (distance(a.positionDistance.xyz, b.positionDistance.xyz) > tolerance) return false;
    if (dot(a.normalRoughness.xyz, b.normalRoughness.xyz) < 0.92) return false;
    if (abs(a.normalRoughness.w - b.normalRoughness.w) > 0.08) return false;
    return true;
}

Guide invalidGuide() {
    Guide guide;
    guide.positionDistance = vec4(0.0);
    guide.normalRoughness = vec4(0.0);
    guide.albedoValidity = vec4(0.0);
    guide.identity = uvec4(0u);
    return guide;
}

Reservoir invalidReservoir() {
    Reservoir reservoir;
    reservoir.selected = vec4(0.0);
    reservoir.statistics = vec4(0.0);
    reservoir.identity = uvec4(INVALID_INDEX, 0u, 0u, 0u);
    return reservoir;
}

BranchRecord branchForDomain(uint base, uint pixel, uint domain) {
    BranchRecord first = branchRecords[base + pixel * 2u];
    BranchRecord second = branchRecords[base + pixel * 2u + 1u];
    if (first.metadata.x == domain && first.metadata.y != 0u) return first;
    if (second.metadata.x == domain && second.metadata.y != 0u) return second;
    BranchRecord empty;
    empty.radiance = vec4(0.0);
    empty.guide = invalidGuide();
    empty.metadata = uvec4(domain, 0u, 0u, 0u);
    return empty;
}

vec3 rawSignal(uint base, uint pixel, uint domain) {
    vec3 result = vec3(0.0);
    BranchRecord first = branchRecords[base + pixel * 2u];
    BranchRecord second = branchRecords[base + pixel * 2u + 1u];
    if (first.metadata.x == domain) result += first.radiance.xyz;
    if (second.metadata.x == domain) result += second.radiance.xyz;
    return result;
}

uint signalDomainFromIndex(uint index) { return index + DOMAIN_ORDINARY_DIFFUSE; }
uint signalIndexFromDomain(uint domain) { return domain - DOMAIN_ORDINARY_DIFFUSE; }

ivec2 pixelCoordinate(uint pixel) {
    return ivec2(int(pixel % camera.frame.x), int(pixel / camera.frame.x));
}
uint linearPixel(ivec2 coordinate) { return uint(coordinate.y) * camera.frame.x + uint(coordinate.x); }
bool inFramebuffer(ivec2 coordinate) {
    return coordinate.x >= 0 && coordinate.y >= 0 && coordinate.x < int(camera.frame.x) &&
           coordinate.y < int(camera.frame.y);
}

ivec2 reprojectPrevious(Guide guide, ivec2 currentPixel, uint domain) {
    if (domain == DOMAIN_GLASS_TRANSMISSION) {
        vec2 compensated = vec2(currentPixel) + camera.jitterPixels.xy - camera.jitterPixels.zw;
        return ivec2(floor(compensated + vec2(0.5)));
    }
    vec3 relative = guide.positionDistance.xyz - camera.previousPositionTanHalfFov.xyz;
    float forwardDistance = dot(relative, camera.previousForwardAspect.xyz);
    if (forwardDistance <= 1e-5) return ivec2(-1);
    float ndcX = dot(relative, camera.previousRight.xyz) /
                 (forwardDistance * camera.previousPositionTanHalfFov.w * camera.previousForwardAspect.w);
    float ndcY = dot(relative, camera.previousUp.xyz) /
                 (forwardDistance * camera.previousPositionTanHalfFov.w);
    vec2 projected = (vec2(ndcX, ndcY) * 0.5 + 0.5) * vec2(camera.frame.xy);
    // The stored sample was generated at pixel center + previous jitter. The
    // current hit already contains the current jitter through its world point.
    return ivec2(floor(projected - camera.jitterPixels.zw));
}

