#ifndef RAE_SCENE_GLSL
#define RAE_SCENE_GLSL

#include "globals.glsl"

layout(std430, set = 0, binding = 1) readonly buffer MaterialsBuffer {
    Material items[];
} materials;
layout(std430, set = 0, binding = 2) readonly buffer TrianglesBuffer {
    Triangle items[];
} triangles;
layout(std430, set = 0, binding = 3) readonly buffer InstanceGroupsBuffer {
    InstanceGroup items[];
} instanceGroups;
layout(std430, set = 0, binding = 4) readonly buffer LightsBuffer {
    LightEntry items[];
} lights;
layout(set = 0, binding = 5) uniform sampler2DArray baseTex;
layout(set = 0, binding = 6) uniform sampler2DArray roughTex;

Globals G() { return globals.items[pc.slot]; }

uint materialType(uint mat) { return uint(materials.items[mat].baseColor.w); }
vec3 materialEmission(uint mat) { return materials.items[mat].emission.xyz; }

vec2 faceUV(vec3 p, vec3 n) {
    vec3 a = abs(n);
    if (a.x >= a.y && a.x >= a.z) {
        return vec2(p.z, p.y);
    }
    if (a.y >= a.z) {
        return vec2(p.x, p.z);
    }
    return vec2(p.x, p.y);
}

vec3 sampleBase(uint mat, vec2 uv, float lod) {
    return textureLod(baseTex, vec3(uv, float(mat)), lod).rgb;
}
float sampleRough(uint mat, vec2 uv, float lod) {
    return textureLod(roughTex, vec3(uv, float(mat)), lod).r;
}

// 自行维护 ray cone footprint，显式选择 mip；不使用固定 LOD 0。
float computeLod(float footprint, float cosTheta) {
    int texSize = textureSize(baseTex, 0).x;
    int levels = textureQueryLevels(baseTex);
    float texels = footprint * float(texSize) / max(cosTheta, 0.15);
    float maxLod = float(max(levels - 1, 1));
    return clamp(log2(max(texels, 1.0)), 0.0, maxLod);
}

vec3 evalBsdf(uint mat, vec3 n, vec3 wo, vec3 wi, vec3 albedo, float rough) {
    uint type = materialType(mat);
    float ndotl = dot(n, wi);
    float ndotv = dot(n, wo);
    if (ndotl <= 0.0 || ndotv <= 0.0) {
        return vec3(0.0);
    }
    if (type == MAT_DIFFUSE) {
        return albedo / RAE_PI;
    }
    if (type == MAT_ROUGH_METAL) {
        float alpha = max(rough * rough, 1e-4);
        return evalGgx(n, wo, wi, albedo, alpha);
    }
    return vec3(0.0);
}

float bsdfPdf(uint mat, vec3 n, vec3 wo, vec3 wi, float rough) {
    uint type = materialType(mat);
    float ndotl = dot(n, wi);
    float ndotv = dot(n, wo);
    if (ndotl <= 0.0 || ndotv <= 0.0) {
        return 0.0;
    }
    if (type == MAT_DIFFUSE) {
        return ndotl / RAE_PI;
    }
    if (type == MAT_ROUGH_METAL) {
        float alpha = max(rough * rough, 1e-4);
        return ggxVndfPdf(n, wo, wi, alpha);
    }
    return 0.0;
}

bool sampleBsdf(uint mat, vec3 n, vec3 wo, vec3 albedo, float rough, inout uint rng,
                out vec3 wi, out float pdf, out vec3 weight) {
    uint type = materialType(mat);
    pdf = 0.0;
    weight = vec3(0.0);
    wi = vec3(0.0);
    if (type == MAT_DIFFUSE) {
        if (!sampleCosine(n, rng, wi, pdf)) {
            return false;
        }
        weight = albedo;
        return true;
    }
    if (type == MAT_ROUGH_METAL) {
        float alpha = max(rough * rough, 1e-4);
        if (!sampleGgxVndf(n, wo, alpha, rng, wi, pdf) || pdf <= 0.0) {
            return false;
        }
        float ndotl = max(dot(n, wi), 0.0);
        weight = evalGgx(n, wo, wi, albedo, alpha) * ndotl / max(pdf, 1e-9);
        return true;
    }
    return false;
}

struct LightSample {
    vec3 pos;
    vec3 nrm;
    vec3 emission;
    vec2 bary; // b0, b1；b2 = 1 - b0 - b1
    uint triangle;
    float pdfArea;
    float lightProb;
};

bool sampleLight(inout uint rng, out LightSample s) {
    Globals g = G();
    uint count = uint(g.glassAbsorption.w);
    float totalPower = g.glassMax.w;
    if (count == 0u || !(totalPower > 0.0)) {
        return false;
    }
    float r = rand01(rng) * totalPower;
    uint lo = 0u;
    uint hi = count - 1u;
    while (lo < hi) {
        uint mid = (lo + hi) >> 1u;
        if (lights.items[mid].cumulative < r) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    LightEntry entry = lights.items[lo];
    if (!(entry.power > 0.0) || !(entry.area > 0.0)) {
        return false;
    }
    float u1 = rand01(rng);
    float u2 = rand01(rng);
    float su = sqrt(u1);
    float b0 = 1.0 - su;
    float b1 = su * (1.0 - u2);
    float b2 = su * u2;
    Triangle tri = triangles.items[entry.triangle];
    s.pos = tri.p0.xyz * b0 + tri.p1.xyz * b1 + tri.p2.xyz * b2;
    s.nrm = normalize(tri.nrm.xyz);
    s.emission = materialEmission(tri.aux.x);
    s.bary = vec2(b0, b1);
    s.triangle = entry.triangle;
    s.lightProb = entry.power / totalPower;
    s.pdfArea = s.lightProb / max(entry.area, 1e-8);
    return s.pdfArea > 0.0;
}

vec3 evalDirectTarget(uint mat, vec3 pos, vec3 n, vec3 wo, vec3 albedo, float rough,
                      LightSample s) {
    vec3 d = s.pos - pos;
    float dist2 = dot(d, d);
    if (dist2 < 1e-10) {
        return vec3(0.0);
    }
    float dist = sqrt(dist2);
    vec3 wi = d / dist;
    float cosR = max(dot(n, wi), 0.0);
    float cosL = max(dot(s.nrm, -wi), 0.0);
    if (cosR <= 0.0 || cosL <= 0.0) {
        return vec3(0.0);
    }
    vec3 f = evalBsdf(mat, n, wo, wi, albedo, rough);
    return f * cosR * s.emission * cosL / dist2;
}

vec3 reconstructLightPoint(uint triIndex, vec2 bary) {
    Triangle tri = triangles.items[triIndex];
    float b0 = bary.x;
    float b1 = bary.y;
    float b2 = max(1.0 - b0 - b1, 0.0);
    return tri.p0.xyz * b0 + tri.p1.xyz * b1 + tri.p2.xyz * b2;
}

float reservoirTargetLum(vec3 pos, vec3 n, vec3 wo, uint mat, vec3 albedo, float rough,
                         uint triIndex, vec2 bary) {
    Triangle tri = triangles.items[triIndex];
    vec3 lp = reconstructLightPoint(triIndex, bary);
    vec3 d = lp - pos;
    float dist2 = dot(d, d);
    if (dist2 < 1e-10) {
        return 0.0;
    }
    float dist = sqrt(dist2);
    vec3 wi = d / dist;
    float cosR = max(dot(n, wi), 0.0);
    float cosL = max(dot(tri.nrm.xyz, -wi), 0.0);
    if (cosR <= 0.0 || cosL <= 0.0) {
        return 0.0;
    }
    vec3 f = evalBsdf(mat, n, wo, wi, albedo, rough);
    vec3 target = f * cosR * materialEmission(tri.aux.x) * cosL / dist2;
    return luminance3(target);
}

vec3 reservoirTargetRGB(vec3 pos, vec3 n, vec3 wo, uint mat, vec3 albedo, float rough,
                        uint triIndex, vec2 bary) {
    Triangle tri = triangles.items[triIndex];
    vec3 lp = reconstructLightPoint(triIndex, bary);
    vec3 d = lp - pos;
    float dist2 = dot(d, d);
    if (dist2 < 1e-10) {
        return vec3(0.0);
    }
    float dist = sqrt(dist2);
    vec3 wi = d / dist;
    float cosR = max(dot(n, wi), 0.0);
    float cosL = max(dot(tri.nrm.xyz, -wi), 0.0);
    if (cosR <= 0.0 || cosL <= 0.0) {
        return vec3(0.0);
    }
    vec3 f = evalBsdf(mat, n, wo, wi, albedo, rough);
    return f * cosR * materialEmission(tri.aux.x) * cosL / dist2;
}

// emitter 命中 MIS 所需的 solid-angle light pdf（从上一个 shading point 到命中点）。
float lightDirPdf(uint triIndex, vec3 prevPos, vec3 rayDir, float dist) {
    uint lightIndex = triangles.items[triIndex].aux.z;
    LightEntry entry = lights.items[lightIndex];
    Globals g = G();
    if (!(entry.power > 0.0) || !(entry.area > 0.0) || !(g.glassMax.w > 0.0)) {
        return 0.0;
    }
    float prob = entry.power / g.glassMax.w;
    float pdfArea = prob / max(entry.area, 1e-8);
    float cosL = max(dot(normalize(triangles.items[triIndex].nrm.xyz), -rayDir), 1e-6);
    return pdfArea * dist * dist / cosL;
}

#endif
