#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_nonuniform_qualifier : require
#ifdef ENABLE_RAY_TRACING_STAGE
#extension GL_EXT_ray_tracing : require
#endif

const uint DOMAIN_DIFFUSE = 0u;
const uint DOMAIN_SPECULAR = 1u;
const uint DOMAIN_REFLECTION = 2u;
const uint DOMAIN_TRANSMISSION = 3u;
const uint DOMAIN_COUNT = 4u;
const uint INVALID_ID = 0xffffffffu;
const float PI = 3.14159265358979323846;

struct Vertex {
    vec4 position;
    vec4 normalU;
    vec4 vMaterialSurface;
};

struct Material {
    vec4 baseColorType;
    vec4 emissionIor;
    vec4 absorptionRoughness;
    uvec4 textureTypePad;
};

struct PathState {
    vec4 originTmin;
    vec4 directionCone;
    vec4 throughputPdf;
    uvec4 identity;
    uvec4 flags;
    vec4 transform0;
    vec4 transform1;
    vec4 transform2;
    uvec4 chain;
};

struct HitRecord {
    vec4 baryT;
    uvec4 identity;
};

struct Guide {
    vec4 worldValid;
    vec4 virtualDepth;
    vec4 normalRoughness;
    uvec4 identity;
};

struct BranchOutput {
    vec4 signal0;
    vec4 signal1;
    vec4 signal2;
    vec4 signal3;
    vec4 emission;
    Guide guide;
    uvec4 guideMeta;
};

struct Surface {
    vec4 positionValid;
    vec4 normalRoughness;
    vec4 albedoType;
    uvec4 identity;
};

struct ShadowTask {
    vec4 originMaxT;
    vec4 directionPad;
    vec4 contribution;
    uvec4 identity;
};

struct Reservoir {
    uvec4 identity;
    vec4 baryTarget;
    vec4 weights;
    vec4 samplePosition;
};

struct HistoryRecord {
    vec4 radianceVariance;
    vec4 moments;
    Guide guide;
};

struct Light {
    vec4 p0Area;
    vec4 p1Cdf;
    vec4 p2Pad;
    vec4 normalPad;
    vec4 emissionPower;
    uvec4 trianglePad;
};

struct FrameData {
    vec4 cameraPositionTanFov;
    vec4 cameraForwardAspect;
    vec4 cameraRightWidth;
    vec4 cameraUpHeight;
    vec4 previousPositionTanFov;
    vec4 previousForwardAspect;
    vec4 previousRightWidth;
    vec4 previousUpHeight;
    vec4 jitterAndTime;
    uvec4 frameInfo;
};

#ifdef ENABLE_RAY_TRACING_STAGE
layout(set = 0, binding = 0) uniform accelerationStructureEXT topLevel;
#endif
layout(set = 0, binding = 1, std430) readonly buffer Vertices { Vertex vertices[]; };
layout(set = 0, binding = 2, std430) readonly buffer Indices { uint indices[]; };
layout(set = 0, binding = 3, std430) readonly buffer Materials { Material materials[]; };
layout(set = 0, binding = 4) uniform sampler2DArray baseColorTextures;
layout(set = 0, binding = 5) uniform sampler2DArray roughnessTextures;
layout(set = 0, binding = 6, std430) buffer PathsA { PathState pathsA[]; };
layout(set = 0, binding = 7, std430) buffer PathsB { PathState pathsB[]; };
layout(set = 0, binding = 8, std430) buffer Hits { HitRecord hits[]; };
layout(set = 0, binding = 9, std430) buffer Counters { uint currentCount; uint nextCount; uint shadowCount; uint overflowed; };
layout(set = 0, binding = 10, std430) buffer IndirectCommands { uvec4 commands[]; };
layout(set = 0, binding = 11, std430) buffer ShadowTasks { ShadowTask shadowTasks[]; };
layout(set = 0, binding = 12, std430) buffer ShadowResults { uint shadowResults[]; };
layout(set = 0, binding = 13, std430) buffer BranchOutputs { BranchOutput branchOutputs[]; };
layout(set = 0, binding = 14, std430) buffer Surfaces { Surface surfaces[]; };
layout(set = 0, binding = 15, std430) buffer Reservoirs { Reservoir reservoirs[]; };
layout(set = 0, binding = 16, std430) buffer Histories { HistoryRecord histories[]; };
layout(set = 0, binding = 17, std430) buffer AtrousA { vec4 atrousA[]; };
layout(set = 0, binding = 18, std430) buffer AtrousB { vec4 atrousB[]; };
layout(set = 0, binding = 19, std430) readonly buffer Lights { Light lights[]; };
layout(set = 0, binding = 20, std430) readonly buffer Frames { FrameData frames[]; };

layout(push_constant, scalar) uniform PushConstants {
    uint width;
    uint height;
    uint bounce;
    uint queueParity;
    uint frameSlot;
    uint frameParity;
    uint phase;
    uint iteration;
} pc;

uint pixelCount() { return pc.width * pc.height; }
uint pathCapacity() { return pixelCount() * 2u; }
uint branchIndex(uint pixel, uint branch) { return pixel * 2u + branch; }
uint domainIndex(uint pixel, uint domain) { return pixel * DOMAIN_COUNT + domain; }
uint historyIndex(uint parity, uint pixel, uint domain) { return parity * pixelCount() * DOMAIN_COUNT + domainIndex(pixel, domain); }
uint surfaceIndex(uint parity, uint pixel) { return parity * pixelCount() + pixel; }
uint reservoirIndex(uint region, uint pixel) { return region * pixelCount() + pixel; }
#ifndef ENABLE_RAY_TRACING_STAGE
uint linearInvocationIndex() {
    return gl_GlobalInvocationID.x + gl_GlobalInvocationID.y * gl_NumWorkGroups.x * 64u;
}
#endif

PathState loadCurrentPath(uint index) { return pc.queueParity == 0u ? pathsA[index] : pathsB[index]; }
void storeNextPath(uint index, PathState path) {
    if (pc.queueParity == 0u) pathsB[index] = path;
    else pathsA[index] = path;
}

uint hash32(uint x) {
    x ^= x >> 16u;
    x *= 0x7feb352du;
    x ^= x >> 15u;
    x *= 0x846ca68bu;
    return x ^ (x >> 16u);
}

float randomFloat(inout PathState path) {
    uint value = hash32(path.identity.x ^ hash32(path.chain.z + path.identity.w * 0x9e3779b9u) ^ hash32(path.identity.y * 0x85ebca6bu));
    path.identity.w++;
    return float(value) * (1.0 / 4294967296.0);
}

float luminance(vec3 value) { return dot(value, vec3(0.2126, 0.7152, 0.0722)); }
float powerHeuristic(float a, float b) { float aa = a * a; float bb = b * b; return aa / max(aa + bb, 1e-20); }

void orthonormalBasis(vec3 n, out vec3 t, out vec3 b) {
    float signValue = n.z >= 0.0 ? 1.0 : -1.0;
    float a = -1.0 / (signValue + n.z);
    float xy = n.x * n.y * a;
    t = vec3(1.0 + signValue * n.x * n.x * a, signValue * xy, -signValue * n.x);
    b = vec3(xy, signValue + n.y * n.y * a, -n.y);
}

vec3 cosineHemisphere(vec2 xi, vec3 n) {
    float r = sqrt(xi.x);
    float phi = 2.0 * PI * xi.y;
    vec3 t;
    vec3 b;
    orthonormalBasis(n, t, b);
    return normalize(t * (r * cos(phi)) + b * (r * sin(phi)) + n * sqrt(max(0.0, 1.0 - xi.x)));
}

float ggxD(float noH, float alpha) {
    float a2 = alpha * alpha;
    float d = noH * noH * (a2 - 1.0) + 1.0;
    return a2 / max(PI * d * d, 1e-8);
}

float smithLambda(float noV, float alpha) {
    float a2 = alpha * alpha;
    float cos2 = noV * noV;
    return 0.5 * (-1.0 + sqrt(1.0 + a2 * max(0.0, 1.0 - cos2) / max(cos2, 1e-8)));
}

vec3 fresnelSchlick(vec3 f0, float voH) { return f0 + (vec3(1.0) - f0) * pow(1.0 - clamp(voH, 0.0, 1.0), 5.0); }

vec3 sampleGgxVndf(vec3 viewWorld, vec3 n, float alpha, vec2 xi) {
    vec3 t;
    vec3 b;
    orthonormalBasis(n, t, b);
    vec3 view = vec3(dot(viewWorld, t), dot(viewWorld, b), dot(viewWorld, n));
    vec3 stretched = normalize(vec3(alpha * view.x, alpha * view.y, view.z));
    float lensq = stretched.x * stretched.x + stretched.y * stretched.y;
    vec3 t1 = lensq > 0.0 ? vec3(-stretched.y, stretched.x, 0.0) / sqrt(lensq) : vec3(1.0, 0.0, 0.0);
    vec3 t2 = cross(stretched, t1);
    float r = sqrt(xi.x);
    float phi = 2.0 * PI * xi.y;
    float p1 = r * cos(phi);
    float p2 = r * sin(phi);
    float s = 0.5 * (1.0 + stretched.z);
    p2 = mix(sqrt(max(0.0, 1.0 - p1 * p1)), p2, s);
    vec3 h = p1 * t1 + p2 * t2 + sqrt(max(0.0, 1.0 - p1 * p1 - p2 * p2)) * stretched;
    vec3 unstretched = normalize(vec3(alpha * h.x, alpha * h.y, max(0.0, h.z)));
    return normalize(t * unstretched.x + b * unstretched.y + n * unstretched.z);
}

vec3 evaluateGgx(vec3 wo, vec3 wi, vec3 n, vec3 f0, float roughness, out float pdf) {
    float noV = max(dot(n, wo), 0.0);
    float noL = max(dot(n, wi), 0.0);
    if (noV <= 0.0 || noL <= 0.0) { pdf = 0.0; return vec3(0.0); }
    vec3 h = normalize(wo + wi);
    float noH = max(dot(n, h), 0.0);
    float voH = max(dot(wo, h), 0.0);
    float alpha = max(roughness * roughness, 0.0025);
    float d = ggxD(noH, alpha);
    float g = 1.0 / (1.0 + smithLambda(noV, alpha) + smithLambda(noL, alpha));
    vec3 f = fresnelSchlick(f0, voH);
    float g1 = 1.0 / (1.0 + smithLambda(noV, alpha));
    pdf = d * g1 / max(4.0 * noV, 1e-8);
    return f * (d * g / max(4.0 * noV * noL, 1e-8));
}

float dielectricFresnel(float cosThetaI, float etaI, float etaT) {
    cosThetaI = clamp(cosThetaI, -1.0, 1.0);
    float sinThetaT = etaI / etaT * sqrt(max(0.0, 1.0 - cosThetaI * cosThetaI));
    if (sinThetaT >= 1.0) return 1.0;
    float cosThetaT = sqrt(max(0.0, 1.0 - sinThetaT * sinThetaT));
    cosThetaI = abs(cosThetaI);
    float rs = (etaT * cosThetaI - etaI * cosThetaT) / (etaT * cosThetaI + etaI * cosThetaT);
    float rp = (etaI * cosThetaI - etaT * cosThetaT) / (etaI * cosThetaI + etaT * cosThetaT);
    return 0.5 * (rs * rs + rp * rp);
}

vec3 transformPoint(PathState path, vec3 p) {
    return vec3(dot(path.transform0.xyz, p) + path.transform0.w,
                dot(path.transform1.xyz, p) + path.transform1.w,
                dot(path.transform2.xyz, p) + path.transform2.w);
}

vec3 transformVector(PathState path, vec3 n) {
    return normalize(vec3(dot(path.transform0.xyz, n), dot(path.transform1.xyz, n), dot(path.transform2.xyz, n)));
}

void appendReflectionTransform(inout PathState path, vec3 n, vec3 point, uint surfaceId) {
    mat3 reflection = mat3(1.0) - 2.0 * outerProduct(n, n);
    vec3 translation = 2.0 * dot(n, point) * n;
    mat3 current = transpose(mat3(path.transform0.xyz, path.transform1.xyz, path.transform2.xyz));
    vec3 currentTranslation = vec3(path.transform0.w, path.transform1.w, path.transform2.w);
    mat3 composed = current * reflection;
    vec3 translated = current * translation + currentTranslation;
    path.transform0 = vec4(composed[0][0], composed[1][0], composed[2][0], translated.x);
    path.transform1 = vec4(composed[0][1], composed[1][1], composed[2][1], translated.y);
    path.transform2 = vec4(composed[0][2], composed[1][2], composed[2][2], translated.z);
    path.chain.x = hash32(path.chain.x ^ hash32(surfaceId + path.chain.y * 0x9e3779b9u));
    path.chain.y++;
}

bool projectPrevious(vec3 world, FrameData frame, out ivec2 pixel, out float depth) {
    vec3 relative = world - frame.previousPositionTanFov.xyz;
    depth = dot(relative, frame.previousForwardAspect.xyz);
    if (depth <= 0.0) return false;
    float x = dot(relative, frame.previousRightWidth.xyz) / (depth * frame.previousPositionTanFov.w * frame.previousForwardAspect.w);
    float y = dot(relative, frame.previousUpHeight.xyz) / (depth * frame.previousPositionTanFov.w);
    vec2 ndc = vec2(x, -y);
    vec2 previousJitter = frame.jitterAndTime.zw;
    vec2 uv = ndc * 0.5 + 0.5 - previousJitter / vec2(pc.width, pc.height);
    pixel = ivec2(floor(uv * vec2(pc.width, pc.height)));
    return all(greaterThanEqual(pixel, ivec2(0))) && all(lessThan(pixel, ivec2(pc.width, pc.height)));
}

bool compatibleGuide(Guide a, Guide b) {
    if (a.worldValid.w == 0.0 || b.worldValid.w == 0.0) return false;
    if (a.identity.x != b.identity.x || a.identity.y != b.identity.y || a.identity.z != b.identity.z || a.identity.w != b.identity.w) return false;
    if (dot(a.normalRoughness.xyz, b.normalRoughness.xyz) < 0.9) return false;
    float scale = max(1.0, abs(a.virtualDepth.w));
    if (distance(a.virtualDepth.xyz, b.virtualDepth.xyz) > 0.02 * scale) return false;
    return abs(a.normalRoughness.w - b.normalRoughness.w) <= 0.1;
}

vec4 getSignal(BranchOutput outputValue, uint domain) {
    if (domain == 0u) return outputValue.signal0;
    if (domain == 1u) return outputValue.signal1;
    if (domain == 2u) return outputValue.signal2;
    return outputValue.signal3;
}

void setSignal(inout BranchOutput outputValue, uint domain, vec4 value) {
    if (domain == 0u) outputValue.signal0 = value;
    else if (domain == 1u) outputValue.signal1 = value;
    else if (domain == 2u) outputValue.signal2 = value;
    else outputValue.signal3 = value;
}
