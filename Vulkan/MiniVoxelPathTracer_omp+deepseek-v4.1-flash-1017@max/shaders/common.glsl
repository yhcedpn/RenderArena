#ifndef RAE_COMMON_GLSL
#define RAE_COMMON_GLSL

// 与 C++ rae_common.hpp 严格一致的 GPU 结构；全部按 vec4/uvec4 对齐。
struct Push {
    uint slot;
    uint queueCur;
    uint queueNext;
    uint count;
    uint width;
    uint height;
    uint step;
    uint flags;
    uint frameIndex;
    uint pad0;
    uint pad1;
    uint pad2;
};
layout(push_constant, std430) uniform PushBlock { Push pc; };

struct Globals {
    vec4 camPos;
    vec4 camFwd;
    vec4 camRight;
    vec4 camUp;
    vec4 prevCamPos;
    vec4 prevCamFwd;
    vec4 prevCamRight;
    vec4 prevCamUp;
    vec4 extent;
    vec4 glassMin;
    vec4 glassMax;
    vec4 glassAbsorption;
    uvec4 flags;
    uvec4 misc;
};

struct Path {
    vec4 origin;     // xyz, tmin
    vec4 dir;        // xyz, cone angle
    vec4 throughput; // rgb, prev BSDF pdf
    vec4 prevPos;    // xyz, cone width
    vec4 prevNormal; // xyz, 保留
    uvec4 virtualT;  // 反射链累计变换
    uvec4 meta0;     // pixel, rng, depth, flags
    uvec4 meta1;     // slot, domain, chainHash, terminalSurfaceId
};

struct HitPayload {
    vec4 pos;    // xyz, t
    vec4 normal; // xyz, hit
    uvec4 geom;  // instance, primitive, bary0 bits, bary1 bits
};

struct ShadowPayload {
    uint visible;
};

struct ShadowTask {
    vec4 origin; // xyz, tmax
    vec4 dir;    // xyz, 保留
    vec4 weight; // rgb, 保留
    uvec4 meta;  // slotIndex
};

struct Guide {
    vec4 posRough; // xyz, roughness
    vec4 nrm;      // xyz, hitDistance
    vec4 albedo;   // rgb 终端/主表面线性 albedo, w 保留
    uvec4 meta;    // chainHash, surfaceId, packedFlags, guideFrame
};

struct PrimaryInfo {
    vec4 pos;    // xyz, valid
    vec4 nrm;    // xyz, view depth
    vec4 albedo; // linear
    vec4 params; // roughness, lobe, material, domain
};

struct Reservoir {
    uvec4 selected; // triangle, M, valid, 保留
    vec4 weights;   // bary0, bary1, weightSum, targetLum
};

struct Material {
    vec4 baseColor;  // rgb linear, w=type
    vec4 params;     // metallic, roughness, ior, baseTexLayer
    vec4 absorption; // rgb, w=roughTexLayer
    vec4 emission;   // rgb, w=pattern
};

struct Triangle {
    vec4 p0;
    vec4 p1;
    vec4 p2;
    vec4 nrm;     // xyz normal, w=area
    uvec4 aux;    // material, group, lightIndex, 保留
};

struct InstanceGroup {
    uvec4 data; // triOffset, material, isGlass, 保留
};

struct LightEntry {
    float cumulative;
    float area;
    float power;
    uint triangle;
};

struct Counts {
    uint pathCount;
    uint shadowCount;
    uint nextPathCount;
    uint nextShadowCount;
    uint pathCapacity;
    uint shadowCapacity;
    uint overflowPath;
    uint overflowShadow;
};

const float RAE_PI = 3.14159265358979323846;
const uint kSlotsPerPixel = 3u;

// Path flags，与 C++ 一致。
const uint PATH_MEDIUM = 1u << 0;
const uint PATH_SPLIT = 1u << 1;
const uint PATH_TERMINAL = 1u << 2;
const uint PATH_EMITTER_SHIFT = 3u;
const uint PATH_BRANCH_SHIFT = 5u;
const uint PATH_CHAIN_SHIFT = 7u;

const uint DOMAIN_PRIMARY_DIFFUSE = 0u;
const uint DOMAIN_PRIMARY_GLOSSY = 1u;
const uint DOMAIN_REFLECTION = 2u;
const uint DOMAIN_TRANSMISSION = 3u;
const uint DOMAIN_NONE = 0xFFFFFFFFu;

const uint CHAIN_PRIMARY = 0u;
const uint CHAIN_MIRROR = 1u;
const uint CHAIN_GLASS_REFLECTION = 2u;
const uint CHAIN_GLASS_TRANSMISSION = 3u;

const uint MOTION_PRIMARY = 0u;
const uint MOTION_REFLECTION = 1u;
const uint MOTION_TRANSMISSION = 2u;

const uint LOBE_DIFFUSE = 0u;
const uint LOBE_GLOSSY = 1u;
const uint LOBE_EMISSIVE = 2u;

const uint MAT_DIFFUSE = 0u;
const uint MAT_ROUGH_METAL = 1u;
const uint MAT_MIRROR = 2u;
const uint MAT_DIELECTRIC = 3u;
const uint MAT_EMISSIVE = 4u;

bool pathMedium(uint f) { return (f & PATH_MEDIUM) != 0u; }
bool pathSplit(uint f) { return (f & PATH_SPLIT) != 0u; }
bool pathTerminal(uint f) { return (f & PATH_TERMINAL) != 0u; }
uint pathEmitter(uint f) { return (f >> PATH_EMITTER_SHIFT) & 3u; }
uint pathBranch(uint f) { return (f >> PATH_BRANCH_SHIFT) & 3u; }
uint pathChain(uint f) { return (f >> PATH_CHAIN_SHIFT) & 7u; }
uint pathSetFlag(uint f, uint bit, bool value) {
    return value ? (f | bit) : (f & ~bit);
}
uint pathSetField(uint f, uint shift, uint mask, uint value) {
    return (f & ~(mask << shift)) | ((value & mask) << shift);
}

uint guideFlags(bool valid, uint lobe, uint motion, uint chainType, uint domain, uint material,
                uint chainLength) {
    return (valid ? 1u : 0u) | ((lobe & 3u) << 1) | ((motion & 3u) << 3) |
           ((chainType & 7u) << 5) | ((domain & 7u) << 8) | ((material & 31u) << 11) |
           ((chainLength & 15u) << 16);
}
bool guideValid(uint f) { return (f & 1u) != 0u; }
uint guideLobe(uint f) { return (f >> 1) & 3u; }
uint guideMotion(uint f) { return (f >> 3) & 3u; }
uint guideChain(uint f) { return (f >> 5) & 7u; }
uint guideDomain(uint f) { return (f >> 8) & 7u; }
uint guideMaterial(uint f) { return (f >> 11) & 31u; }
uint guideChainLength(uint f) { return (f >> 16) & 15u; }

uint hashU32(uint x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}
uint hashCombine(uint a, uint b) {
    return hashU32(a ^ (b + 0x9e3779b9u + (a << 6) + (a >> 2)));
}
uint pcgNext(inout uint state) {
    state = state * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28) + 4u)) ^ state) * 277803737u;
    return (word >> 22) ^ word;
}
float rand01(inout uint state) {
    return float(pcgNext(state) >> 8) * (1.0 / 16777216.0);
}
vec2 rand2(inout uint state) {
    return vec2(rand01(state), rand01(state));
}

float luminance3(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }
float powerHeuristic(float a, float b) {
    float aa = a * a;
    float bb = b * b;
    float d = aa + bb;
    return d > 0.0 ? aa / d : 0.0;
}

// 反射链累计变换：轴置换 + 平移。identity map = x<-x, y<-y, z<-z 的源轴 0,1,2。
const uint IDENTITY_MAP = 0u | (1u << 3) | (2u << 6);

uvec4 identityTransform() { return uvec4(IDENTITY_MAP, 0u, 0u, 0u); }

float selectComp(vec3 v, uint axis) {
    return axis == 0u ? v.x : (axis == 1u ? v.y : v.z);
}

vec3 virtualDir(vec3 v, uvec4 t) {
    uint map = t.x;
    vec3 outv;
    uint s0 = map & 3u;
    uint s1 = (map >> 3) & 3u;
    uint s2 = (map >> 6) & 3u;
    outv.x = selectComp(v, s0) * ((((map >> 2) & 1u) != 0u) ? -1.0 : 1.0);
    outv.y = selectComp(v, s1) * ((((map >> 5) & 1u) != 0u) ? -1.0 : 1.0);
    outv.z = selectComp(v, s2) * ((((map >> 8) & 1u) != 0u) ? -1.0 : 1.0);
    return outv;
}

vec3 virtualPos(vec3 p, uvec4 t) {
    return virtualDir(p, t) + vec3(uintBitsToFloat(t.y), uintBitsToFloat(t.z),
                                   uintBitsToFloat(t.w));
}

// T_new = T_old o R_plane。平面法线必须是轴对齐的。
uvec4 reflectTransform(uvec4 t, vec3 n, vec3 q) {
    uint axis = abs(n.x) > 0.5 ? 0u : (abs(n.y) > 0.5 ? 1u : 2u);
    uint map = t.x;
    if ((map & 3u) == axis) {
        map ^= 1u << 2;
    }
    if (((map >> 3) & 3u) == axis) {
        map ^= 1u << 5;
    }
    if (((map >> 6) & 3u) == axis) {
        map ^= 1u << 8;
    }
    vec3 tR = 2.0 * dot(n, q) * n;
    vec3 moved = virtualDir(tR, t) + vec3(uintBitsToFloat(t.y), uintBitsToFloat(t.z),
                                          uintBitsToFloat(t.w));
    return uvec4(map, floatBitsToUint(moved.x), floatBitsToUint(moved.y),
                 floatBitsToUint(moved.z));
}

// GGX / Smith / Fresnel。
float ggxD(float ndoth, float alpha) {
    float a2 = alpha * alpha;
    float d = ndoth * ndoth * (a2 - 1.0) + 1.0;
    return a2 / max(RAE_PI * d * d, 1e-12);
}
float smithLambda(float c, float alpha) {
    float cc = max(c * c, 1e-8);
    float a2 = alpha * alpha;
    return 0.5 * (sqrt(1.0 + a2 * (1.0 - cc) / cc) - 1.0);
}
float ggxG2(float ndotv, float ndotl, float alpha) {
    return 1.0 / (1.0 + smithLambda(ndotv, alpha) + smithLambda(ndotl, alpha));
}
vec3 fresnelSchlick(float cosTheta, vec3 f0) {
    float f = pow(1.0 - clamp(cosTheta, 0.0, 1.0), 5.0);
    return f0 + (vec3(1.0) - f0) * f;
}
float fresnelDielectric(float cosI, float etaI, float etaT) {
    float sinT2 = (etaI / etaT) * (etaI / etaT) * max(1.0 - cosI * cosI, 0.0);
    if (sinT2 >= 1.0) {
        return 1.0;
    }
    float cosT = sqrt(max(1.0 - sinT2, 0.0));
    float rs = (etaI * cosI - etaT * cosT) / max(etaI * cosI + etaT * cosT, 1e-8);
    float rp = (etaT * cosI - etaI * cosT) / max(etaT * cosI + etaI * cosT, 1e-8);
    return clamp(0.5 * (rs * rs + rp * rp), 0.0, 1.0);
}
bool refractDir(vec3 incident, vec3 normal, float eta, out vec3 refracted) {
    float cosI = max(-dot(incident, normal), 0.0);
    float sinT2 = eta * eta * max(1.0 - cosI * cosI, 0.0);
    if (sinT2 >= 1.0) {
        refracted = vec3(0.0);
        return false;
    }
    refracted = eta * incident + (eta * cosI - sqrt(1.0 - sinT2)) * normal;
    refracted = normalize(refracted);
    return true;
}
vec3 evalGgx(vec3 n, vec3 wo, vec3 wi, vec3 f0, float alpha) {
    float ndotv = max(dot(n, wo), 1e-4);
    float ndotl = max(dot(n, wi), 1e-4);
    if (dot(n, wo) <= 0.0 || dot(n, wi) <= 0.0) {
        return vec3(0.0);
    }
    vec3 h = normalize(wo + wi);
    float ndoth = max(dot(n, h), 0.0);
    float vdoth = max(dot(wo, h), 0.0);
    float d = ggxD(ndoth, alpha);
    float g = ggxG2(ndotv, ndotl, alpha);
    vec3 f = fresnelSchlick(vdoth, f0);
    return d * g * f / (4.0 * ndotv * ndotl);
}
float ggxVndfPdf(vec3 n, vec3 wo, vec3 wi, float alpha) {
    float ndotv = dot(n, wo);
    if (ndotv <= 0.0 || dot(n, wi) <= 0.0) {
        return 0.0;
    }
    vec3 h = normalize(wo + wi);
    float ndoth = max(dot(n, h), 0.0);
    float d = ggxD(ndoth, alpha);
    float g1 = 1.0 / (1.0 + smithLambda(ndotv, alpha));
    return d * g1 / (4.0 * max(ndotv, 1e-4));
}
bool sampleGgxVndf(vec3 n, vec3 wo, float alpha, inout uint rng, out vec3 wi, out float pdf) {
    float ndotv = dot(n, wo);
    if (ndotv <= 0.0) {
        wi = vec3(0.0);
        pdf = 0.0;
        return false;
    }
    vec3 t = normalize(abs(n.z) < 0.999 ? cross(vec3(0.0, 0.0, 1.0), n)
                                        : cross(vec3(1.0, 0.0, 0.0), n));
    vec3 b = cross(n, t);
    vec3 woT = vec3(dot(wo, t), dot(wo, b), ndotv);
    vec3 vh = normalize(vec3(alpha * woT.x, alpha * woT.y, woT.z));
    float lensq = vh.x * vh.x + vh.y * vh.y;
    vec3 t1 = lensq > 0.0 ? vec3(-vh.y, vh.x, 0.0) / sqrt(lensq) : vec3(1.0, 0.0, 0.0);
    vec3 t2 = cross(vh, t1);
    float r = sqrt(rand01(rng));
    float phi = 2.0 * RAE_PI * rand01(rng);
    float p1 = r * cos(phi);
    float p2 = r * sin(phi);
    float s = 0.5 * (1.0 + vh.z);
    p2 = (1.0 - s) * sqrt(max(1.0 - p1 * p1, 0.0)) + s * p2;
    vec3 nh = p1 * t1 + p2 * t2 + sqrt(max(1.0 - p1 * p1 - p2 * p2, 0.0)) * vh;
    if (nh.z <= 0.0) {
        wi = vec3(0.0);
        pdf = 0.0;
        return false;
    }
    vec3 h = normalize(t * nh.x + b * nh.y + n * nh.z);
    wi = reflect(-wo, h);
    if (dot(n, wi) <= 0.0) {
        pdf = 0.0;
        return false;
    }
    float d = ggxD(nh.z, alpha);
    float g1 = 1.0 / (1.0 + smithLambda(ndotv, alpha));
    pdf = d * g1 / (4.0 * max(ndotv, 1e-4));
    return pdf > 0.0;
}
bool sampleCosine(vec3 n, inout uint rng, out vec3 wi, out float pdf) {
    vec3 t = normalize(abs(n.z) < 0.999 ? cross(vec3(0.0, 0.0, 1.0), n)
                                        : cross(vec3(1.0, 0.0, 0.0), n));
    vec3 b = cross(n, t);
    float u1 = rand01(rng);
    float u2 = rand01(rng);
    float r = sqrt(u1);
    float phi = 2.0 * RAE_PI * u2;
    wi = normalize(t * (r * cos(phi)) + b * (r * sin(phi)) + n * sqrt(max(1.0 - u1, 0.0)));
    float ndotl = dot(n, wi);
    if (ndotl <= 0.0) {
        pdf = 0.0;
        return false;
    }
    pdf = ndotl / RAE_PI;
    return true;
}

#endif
