// 共享 GLSL：结构体、描述符绑定、随机数、材质/BSDF、光源采样、路径标志。
// 绑定编号与 src/shared.hpp 保持一致。

#define VPT_PI 3.14159265358979323846
#define VPT_INV_PI 0.31830988618379067154
#define VPT_EPS 1e-6

// 信号域
#define DOMAIN_DIFFUSE 0u
#define DOMAIN_SPECULAR 1u
#define DOMAIN_MIRROR_DIFFUSE 2u
#define DOMAIN_MIRROR_SPECULAR 3u
#define DOMAIN_GLASS_REFLECT 4u
#define DOMAIN_GLASS_TRANSMIT 5u
#define DOMAIN_COUNT 6u

// 材质类型
#define MAT_DIFFUSE 0u
#define MAT_ROUGH_METAL 1u
#define MAT_MIRROR 2u
#define MAT_DIELECTRIC 3u
#define MAT_EMISSIVE 4u

// 路径标志位（ids.y）
#define PF_BRANCH 1u
#define PF_HAS_TRANSMISSION (1u << 1)
#define PF_GLASS_SPLIT_DONE (1u << 2)
#define PF_SUPPRESS_EMITTER (1u << 3)
#define PF_LAST_DELTA (1u << 4)
#define PF_PATHCLASS_SHIFT 5u
#define PF_CHAIN_GLASS (1u << 7)

#define PATHCLASS_PRIMARY_DIFFUSE 0u
#define PATHCLASS_PRIMARY_SPECULAR 1u
#define PATHCLASS_MIRROR 2u
#define PATHCLASS_GLASS 3u

#define SHADOWKIND_BRANCH 0u
#define SHADOWKIND_RESTIR 1u

struct Material {
    vec4 baseColor;   // linear rgb
    vec4 absorption;  // sigma rgb, ior
    vec4 emission;    // Le rgb, metallic
    vec4 params;      // roughness, type, layer, reserved
};

struct Triangle {
    vec4 p0;
    vec4 p1;
    vec4 p2;
    vec4 n;
    uvec4 extra; // material, planeIndex, lightIndex, instance
};

struct InstanceDesc {
    uint triangleOffset;
    uint triangleCount;
    uint material;
    uint pad;
};

struct Light {
    uvec4 ids;           // instance, primitive, globalTriangle, lightIndex
    vec4 emissionArea;   // rgb, area
    vec4 cdf;            // cumulative, probability
};

struct Plane {
    vec4 n; // xyz normal, w offset
};

struct Path {
    vec4 originTmin;
    vec4 dirTmax;
    vec4 throughput; // rgb, scatterCount
    uvec4 ids;       // pixel, flags, rngState, medium
    uvec4 misc;      // chainHash, bsdfPdf bits, planes0, planes1
    uvec4 terminal;  // pos.xy half, (pos.z, rough) half, material|valid<<8, instance<<16|prim
    uvec4 meta;      // chainCount, coneWidth bits, reserved, reserved
};

struct Hit {
    float t;
    uint instance;
    uint primitive;
    float baryX;
    float baryY;
    float pad0;
    float pad1;
    float pad2;
};

struct PrimarySurface {
    vec4 posDepth;
    vec4 normalRough;
    vec4 albedo;
    vec4 uv;
    vec4 ids;  // instance, primitive, material, flags (1 valid, 2 delta, 4 emissive)
    vec4 tag;  // frame, pathClass, reserved, reserved
};

struct Guide {
    uint posDepth0;
    uint posDepth1;
    uint normalRough0;
    uint normalRough1;
    uint chainHash;
    uint terminalId;
    uint materialFlags;
    uint frameTag;
};

struct BranchInfo {
    Guide guide;
    uint domain;
    uint frame;
    uint valid;
    uint pad;
};

struct Reservoir {
    uvec4 sampleData; // lightIndex, valid, baryX bits, baryY bits
    vec4 stats;   // W, M, targetSrc, reserved
};

struct ShadowTask {
    vec4 originTmax;
    vec4 target;
    uvec4 ids; // instance, primitive, targetIndex, kind
    vec4 weight;
};

layout(push_constant) uniform PushConstants {
    uint pass;
    uint frame;
    uint width;
    uint height;
    uint domainIndex;
    uint iteration;
    uint queue;
    uint flags;
    uint pad0;
    uint pad1;
    uint pad2;
    uint pad3;
    uint pad4;
    uint pad5;
    uint pad6;
    uint pad7;
} pc;

layout(set = 0, binding = 0) uniform sampler2DArray baseColorTex;
layout(set = 0, binding = 1) uniform sampler2DArray roughnessTex;

layout(std430, set = 0, binding = 2) readonly buffer MaterialsBuf { Material materials[]; };
layout(std430, set = 0, binding = 3) readonly buffer TrianglesBuf { Triangle triangles[]; };
layout(std430, set = 0, binding = 4) readonly buffer InstancesBuf { InstanceDesc instances[]; };
layout(std430, set = 0, binding = 5) readonly buffer LightsBuf { Light lights[]; };
layout(std430, set = 0, binding = 33) readonly buffer PlanesBuf { Plane planes[]; };

layout(std140, set = 0, binding = 6) uniform CameraUBO {
    mat4 viewProjCurr;
    mat4 viewProjPrev;
    vec4 position; // xyz, tanHalfFov
    vec4 right;    // xyz, aspect
    vec4 up;       // xyz, frame parity
    vec4 forward;  // xyz, frame bits
    vec4 extent;   // width, height, 1/width, 1/height
    vec4 flags;    // cameraMoved, reset
    vec4 glassMin;
    vec4 glassMax;
} cam;

uint pcgHash(uint v) {
    v = v * 747796405u + 2891336453u;
    uint w = ((v >> ((v >> 28u) + 4u)) ^ v) * 277803737u;
    return (w >> 22u) ^ w;
}

float rand(inout uint state) {
    state = state * 747796405u + 2891336453u;
    return float(pcgHash(state)) * (1.0 / 4294967296.0);
}

vec2 pixelJitter(uint pixel, uint frameIndex) {
    uint h = pcgHash(pixel * 0x9E3779B1u ^ pcgHash(frameIndex + 0x85EBCA6Bu));
    float x = float(pcgHash(h + 0x68BC21EBu)) * (1.0 / 4294967296.0) - 0.5;
    float y = float(pcgHash(h + 0x02E5BE93u)) * (1.0 / 4294967296.0) - 0.5;
    return vec2(x, y);
}

float luminance(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

// 世界坐标决定 UV，保证 greedy 合并后仍每世界单位重复一次。
vec2 worldUV(vec3 p, vec3 n) {
    vec3 a = abs(n);
    if (a.x >= a.y && a.x >= a.z) return n.x > 0.0 ? vec2(p.z, p.y) : vec2(-p.z, p.y);
    if (a.y >= a.z) return n.y > 0.0 ? vec2(p.x, p.z) : vec2(p.x, -p.z);
    return n.z > 0.0 ? vec2(p.x, p.y) : vec2(-p.x, p.y);
}

float computeMipLod(float coneWidth, float distance, vec3 n, vec3 dir) {
    float cosAngle = max(abs(dot(n, dir)), 0.05);
    float texSize = float(textureSize(baseColorTex, 0).x);
    float footprint = max(distance * coneWidth / cosAngle, 1e-7);
    float lod = log2(footprint * texSize);
    // 该 AMD 驱动的 compute 中 textureQueryLevels 返回不可靠，改用相机上传的 mip 上限。
    float maxLod = max(0.0, cam.glassMax.w);
    return clamp(lod, 0.0, maxLod);
}

vec3 sampleBaseColor(uint layer, vec2 uv, float lod) {
    return textureLod(baseColorTex, vec3(uv, float(layer)), lod).rgb;
}

float sampleRoughness(uint layer, vec2 uv, float lod) {
    return textureLod(roughnessTex, vec3(uv, float(layer)), lod).r;
}

void orthonormalBasis(vec3 n, out vec3 t, out vec3 b) {
    float sign = n.z >= 0.0 ? 1.0 : -1.0;
    float a = -1.0 / (sign + n.z);
    float c = n.x * n.y * a;
    t = vec3(1.0 + sign * n.x * n.x * a, sign * c, -sign * n.x);
    b = vec3(c, sign + n.y * n.y * a, -n.y);
}

vec3 fresnelSchlickVec(float cosTheta, vec3 f0) {
    float m = clamp(1.0 - cosTheta, 0.0, 1.0);
    float m2 = m * m;
    return f0 + (vec3(1.0) - f0) * (m2 * m2 * m);
}

float dielectricFresnel(float cosI, float eta) {
    float sinT2 = eta * eta * (1.0 - cosI * cosI);
    if (sinT2 >= 1.0) return 1.0;
    float cosT = sqrt(max(0.0, 1.0 - sinT2));
    float rPar = (cosI - eta * cosT) / max(cosI + eta * cosT, 1e-8);
    float rPerp = (eta * cosI - cosT) / max(eta * cosI + cosT, 1e-8);
    return 0.5 * (rPar * rPar + rPerp * rPerp);
}

float ggxD(float noH, float alpha) {
    float a2 = alpha * alpha;
    float d = noH * noH * (a2 - 1.0) + 1.0;
    return a2 / max(VPT_PI * d * d, 1e-12);
}

float smithG1(float noV, float alpha) {
    float a2 = alpha * alpha;
    return 2.0 * noV / max(noV + sqrt(a2 + (1.0 - a2) * noV * noV), 1e-8);
}

float smithLambda(float c, float alpha) {
    float a2 = alpha * alpha;
    float c2 = max(c * c, 1e-8);
    return 0.5 * (-1.0 + sqrt(1.0 + a2 * (1.0 - c2) / c2));
}

float smithG2(float noV, float noL, float alpha) {
    return 1.0 / (1.0 + smithLambda(noV, alpha) + smithLambda(noL, alpha));
}

// GGX 可见法线分布采样（Heitz）。wo/wi 均在局部 +Z 半球。
vec3 sampleGgxVndf(vec3 wo, float alpha, float u1, float u2, out float pdf) {
    vec3 vh = normalize(vec3(alpha * wo.x, alpha * wo.y, wo.z));
    float lensq = vh.x * vh.x + vh.y * vh.y;
    vec3 t1 = lensq > 0.0 ? vec3(-vh.y, vh.x, 0.0) * inversesqrt(lensq) : vec3(1.0, 0.0, 0.0);
    vec3 t2 = cross(vh, t1);
    float r = sqrt(u1);
    float phi = 2.0 * VPT_PI * u2;
    float p1 = r * cos(phi);
    float p2 = r * sin(phi);
    float s = 0.5 * (1.0 + vh.z);
    p2 = (1.0 - s) * sqrt(max(0.0, 1.0 - p1 * p1)) + s * p2;
    vec3 nh = p1 * t1 + p2 * t2 + sqrt(max(0.0, 1.0 - p1 * p1 - p2 * p2)) * vh;
    vec3 ne = normalize(vec3(alpha * nh.x, alpha * nh.y, max(0.0, nh.z)));
    vec3 wi = reflect(-wo, ne);
    float d = ggxD(max(ne.z, 0.0), alpha);
    float g1 = smithG1(max(wo.z, 1e-4), alpha);
    pdf = d * g1 / (4.0 * max(wo.z, 1e-4));
    return wi;
}

struct LightSample {
    vec3 point;
    vec3 normal;
    vec3 emission;
    float area;
    float prob;
    uint instance;
    uint primitive;
    uint lightIndex;
    vec3 bary;
};

bool sampleLight(inout uint rng, out LightSample ls) {
    uint count = uint(lights.length());
    if (count == 0u) return false;
    float u = rand(rng);
    uint idx = count - 1u;
    for (uint i = 0u; i < count; ++i) {
        if (u <= lights[i].cdf.x) { idx = i; break; }
    }
    Light light = lights[idx];
    Triangle tri = triangles[light.ids.z];
    float u1 = rand(rng);
    float u2 = rand(rng);
    float su = sqrt(u1);
    ls.bary = vec3(1.0 - su, su * (1.0 - u2), su * u2);
    ls.point = ls.bary.x * tri.p0.xyz + ls.bary.y * tri.p1.xyz + ls.bary.z * tri.p2.xyz;
    ls.normal = normalize(tri.n.xyz);
    ls.emission = light.emissionArea.rgb;
    ls.area = light.emissionArea.w;
    ls.prob = light.cdf.y;
    ls.instance = light.ids.x;
    ls.primitive = light.ids.y;
    ls.lightIndex = idx;
    return true;
}

bool reconstructLightSample(uint lightIndex, vec2 bary2, out LightSample ls) {
    if (lightIndex >= uint(lights.length())) return false;
    Light light = lights[lightIndex];
    Triangle tri = triangles[light.ids.z];
    ls.bary = vec3(bary2.x, bary2.y, 1.0 - bary2.x - bary2.y);
    ls.point = ls.bary.x * tri.p0.xyz + ls.bary.y * tri.p1.xyz + ls.bary.z * tri.p2.xyz;
    ls.normal = normalize(tri.n.xyz);
    ls.emission = light.emissionArea.rgb;
    ls.area = light.emissionArea.w;
    ls.prob = light.cdf.y;
    ls.instance = light.ids.x;
    ls.primitive = light.ids.y;
    ls.lightIndex = lightIndex;
    return true;
}

// 返回未计可见性的直接光 RGB（面积测度），并给出接收端 BSDF 的立体角 PDF。
vec3 evaluateDirect(vec3 pos, vec3 n, vec3 wo, uint materialIndex, vec3 albedo, float alpha,
                    LightSample ls, out float bsdfPdf, out float cosO, out float cosL,
                    out float dist2) {
    vec3 toLight = ls.point - pos;
    dist2 = max(dot(toLight, toLight), 1e-12);
    vec3 wi = toLight * inversesqrt(dist2);
    cosO = dot(n, wi);
    cosL = dot(ls.normal, -wi);
    bsdfPdf = 0.0;
    if (cosO <= 0.0 || cosL <= 0.0) return vec3(0.0);
    uint type = uint(materials[materialIndex].params.y);
    vec3 f;
    if (type == MAT_DIFFUSE) {
        f = albedo * VPT_INV_PI;
        bsdfPdf = cosO * VPT_INV_PI;
    } else if (type == MAT_ROUGH_METAL) {
        vec3 h = normalize(wo + wi);
        float noV = max(dot(n, wo), 1e-4);
        float noL = max(cosO, 1e-4);
        float noH = max(dot(n, h), 0.0);
        float d = ggxD(noH, alpha);
        float g = smithG2(noV, noL, alpha);
        vec3 fr = fresnelSchlickVec(max(dot(wo, h), 0.0), albedo);
        f = fr * d * g / max(4.0 * noV * noL, 1e-8);
        bsdfPdf = d * smithG1(noV, alpha) / (4.0 * noV);
    } else {
        return vec3(0.0);
    }
    return f * cosO * ls.emission * cosL / dist2;
}

float misWeight(float pdfA, float pdfB) {
    float a2 = pdfA * pdfA;
    float b2 = pdfB * pdfB;
    return a2 / max(a2 + b2, 1e-12);
}

// 路径标志/光学链。
void appendChainPlane(inout Path p, uint planeIndex) {
    uint count = p.meta.x;
    if (count >= 8u) return;
    if (count < 4u) {
        p.misc.z |= (planeIndex & 0xFFu) << (count * 8u);
    } else {
        p.misc.w |= (planeIndex & 0xFFu) << ((count - 4u) * 8u);
    }
    p.meta.x = count + 1u;
    p.misc.x = pcgHash(p.misc.x ^ planeIndex ^ 0x9E3779B9u);
}

uint chainPlaneAt(Path p, uint index) {
    if (index < 4u) return (p.misc.z >> (index * 8u)) & 0xFFu;
    return (p.misc.w >> ((index - 4u) * 8u)) & 0xFFu;
}

void setTerminal(inout Path p, vec3 worldPos, float roughness, uint materialIndex,
                 uint instance, uint primitive) {
    p.terminal.x = packHalf2x16(vec2(worldPos.x, worldPos.y));
    p.terminal.y = packHalf2x16(vec2(worldPos.z, roughness));
    p.terminal.z = (materialIndex & 0xFFu) | 0x100u;
    p.terminal.w = ((instance & 0xFFFFu) << 16) | (primitive & 0xFFFFu);
    p.misc.x = pcgHash(p.misc.x ^ (instance * 73856093u) ^ (primitive * 19349663u) ^ 0x51ED270Bu);
}

bool terminalValid(Path p) { return (p.terminal.z & 0x100u) != 0u; }

uint pathClass(Path p) { return (p.ids.y >> PF_PATHCLASS_SHIFT) & 3u; }

uint classifyDomain(Path p) {
    uint cls = pathClass(p);
    if (cls == PATHCLASS_PRIMARY_DIFFUSE) return DOMAIN_DIFFUSE;
    if (cls == PATHCLASS_PRIMARY_SPECULAR) return DOMAIN_SPECULAR;
    if (cls == PATHCLASS_MIRROR) {
        if (!terminalValid(p)) return DOMAIN_MIRROR_SPECULAR;
        uint mat = p.terminal.z & 0xFFu;
        return uint(materials[mat].params.y) == MAT_DIFFUSE ? DOMAIN_MIRROR_DIFFUSE
                                                            : DOMAIN_MIRROR_SPECULAR;
    }
    return (p.ids.y & PF_HAS_TRANSMISSION) != 0u ? DOMAIN_GLASS_TRANSMIT : DOMAIN_GLASS_REFLECT;
}

// 终端世界位置/法线/粗糙度/材质。
void decodeTerminal(Path p, out vec3 pos, out vec3 normal, out float roughness, out uint materialIndex,
                    out uint instance, out uint primitive) {
    vec2 xy = unpackHalf2x16(p.terminal.x);
    vec2 zr = unpackHalf2x16(p.terminal.y);
    pos = vec3(xy.x, xy.y, zr.x);
    roughness = zr.y;
    materialIndex = p.terminal.z & 0xFFu;
    instance = p.terminal.w >> 16;
    primitive = p.terminal.w & 0xFFFFu;
    InstanceDesc inst = instances[instance];
    Triangle tri = triangles[inst.triangleOffset + primitive];
    normal = normalize(tri.n.xyz);
}

// 反射链的虚拟位置/法线：按命中顺序逆序反射。
void reflectThroughChain(Path p, inout vec3 pos, inout vec3 normal) {
    uint count = p.meta.x;
    for (uint i = 0u; i < count; ++i) {
        uint idx = count - 1u - i;
        vec4 plane = planes[chainPlaneAt(p, idx)].n;
        float d = dot(plane.xyz, pos) - plane.w;
        pos -= 2.0 * d * plane.xyz;
        normal -= 2.0 * dot(plane.xyz, normal) * plane.xyz;
    }
    normal = normalize(normal);
}

// Guide 打包。
void packGuide(out Guide g, vec3 pos, float depth, vec3 normal, float roughness,
               uint chainHash, uint terminalId, uint material, uint flags, uint frameTag) {
    g.posDepth0 = packHalf2x16(vec2(pos.x, pos.y));
    g.posDepth1 = packHalf2x16(vec2(pos.z, depth));
    g.normalRough0 = packHalf2x16(vec2(normal.x, normal.y));
    g.normalRough1 = packHalf2x16(vec2(normal.z, roughness));
    g.chainHash = chainHash;
    g.terminalId = terminalId;
    g.materialFlags = (material & 0xFFFFu) | (flags << 16);
    g.frameTag = frameTag;
}

void unpackGuide(Guide g, out vec3 pos, out float depth, out vec3 normal, out float roughness,
                 out uint material, out uint flags) {
    vec2 xy = unpackHalf2x16(g.posDepth0);
    vec2 zd = unpackHalf2x16(g.posDepth1);
    pos = vec3(xy.x, xy.y, zd.x);
    depth = zd.y;
    vec2 nxny = unpackHalf2x16(g.normalRough0);
    vec2 nzr = unpackHalf2x16(g.normalRough1);
    normal = vec3(nxny.x, nxny.y, nzr.x);
    roughness = nzr.y;
    material = g.materialFlags & 0xFFFFu;
    flags = g.materialFlags >> 16;
}

float viewDepth(vec3 p) { return dot(cam.forward.xyz, p - cam.position.xyz); }

// 上一帧无 jitter 投影 + 两帧 jitter 显式补偿。
vec2 reprojectUV(vec3 worldPos, uint pixel, uint frameIndex) {
    vec4 clip = cam.viewProjPrev * vec4(worldPos, 1.0);
    if (clip.w <= 1e-6) return vec2(-1.0);
    vec2 uv = (clip.xy / clip.w) * 0.5 + 0.5;
    vec2 jitterCurr = pixelJitter(pixel, frameIndex);
    vec2 jitterPrev = pixelJitter(pixel, frameIndex - 1u);
    return uv + (jitterCurr - jitterPrev) / cam.extent.xy;
}
