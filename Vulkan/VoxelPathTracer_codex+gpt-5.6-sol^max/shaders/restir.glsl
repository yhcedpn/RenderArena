#ifndef VPT_RESTIR_GLSL
#define VPT_RESTIR_GLSL

#include "common.glsl"

struct RestirSurface {
    vec3 position;
    vec3 normal;
    vec3 albedo;
    float roughness;
    uint material;
    uint surface;
    uint domain;
    bool valid;
};

struct LightEvaluation {
    vec3 rgb;
    float target;
    vec3 position;
    vec3 direction;
    float distanceToLight;
    float proposalArea;
};

RestirSurface surfaceFromShading(ShadingPoint point) {
    RestirSurface surface;
    surface.position = point.position.xyz;
    surface.normal = point.normalRoughness.xyz;
    surface.roughness = point.normalRoughness.w;
    surface.albedo = point.albedo.rgb;
    surface.material = point.ids.y;
    surface.surface = point.ids.z;
    surface.domain = point.ids.w;
    surface.valid = point.ids.x != 0u;
    return surface;
}

RestirSurface surfaceFromGuide(Guide guide) {
    RestirSurface surface;
    surface.position = guide.positionDepth.xyz;
    surface.normal = guide.normalRoughness.xyz;
    surface.roughness = guide.normalRoughness.w;
    surface.albedo = guide.albedo.rgb;
    surface.material = guide.ids.y;
    surface.surface = guide.ids.x;
    surface.domain = guideDomain(guide);
    surface.valid = guide.ids.x != 0u;
    return surface;
}

bool restirCompatible(RestirSurface a, RestirSurface b) {
    if (!a.valid || !b.valid || a.surface != b.surface || a.material != b.material ||
        a.domain != b.domain)
        return false;
    if (dot(a.normal, b.normal) < 0.92) return false;
    return distance(a.position, b.position) <= 0.05 + 0.01 * length(a.position);
}

float restirGgxLambda(float noV, float alpha) {
    float noV2 = max(noV * noV, 1.0e-8);
    return 0.5 * (sqrt(1.0 + alpha * alpha * max(0.0, 1.0 - noV2) / noV2) - 1.0);
}

vec3 restirBsdf(RestirSurface surface, vec3 wi) {
    uint type = floatBitsToUint(materials[surface.material].params.x);
    float noL = max(dot(surface.normal, wi), 0.0);
    if (type == 0u) return surface.albedo / PI;
    vec3 wo = safeNormalize(globals.cameraPositionFrame.xyz - surface.position);
    float noV = max(dot(surface.normal, wo), 0.0);
    if (noL <= 0.0 || noV <= 0.0) return vec3(0.0);
    vec3 h = safeNormalize(wo + wi);
    float noH = max(dot(surface.normal, h), 0.0);
    float voH = max(dot(wo, h), 0.0);
    float alpha = max(surface.roughness * surface.roughness, 1.0e-3);
    float a2 = alpha * alpha;
    float denominator = noH * noH * (a2 - 1.0) + 1.0;
    float d = a2 / max(PI * denominator * denominator, 1.0e-20);
    float g = 1.0 / (1.0 + restirGgxLambda(noV, alpha) +
                     restirGgxLambda(noL, alpha));
    vec3 f = surface.albedo + (vec3(1.0) - surface.albedo) *
                                pow(clamp(1.0 - voH, 0.0, 1.0), 5.0);
    return f * (d * g / max(4.0 * noV * noL, 1.0e-20));
}

LightEvaluation evaluateLightSample(RestirSurface surface, uvec4 candidateSample) {
    LightEvaluation result;
    result.rgb = vec3(0.0);
    result.target = 0.0;
    result.position = vec3(0.0);
    result.direction = vec3(0.0, 1.0, 0.0);
    result.distanceToLight = 0.0;
    result.proposalArea = 0.0;
    if (!surface.valid || candidateSample.w == 0u ||
        candidateSample.x >= globals.extentCounts.w)
        return result;
    LightData light = lights[candidateSample.x];
    float b1 = uintBitsToFloat(candidateSample.y);
    float b2 = uintBitsToFloat(candidateSample.z);
    float b0 = 1.0 - b1 - b2;
    if (b0 < 0.0 || b1 < 0.0 || b2 < 0.0) return result;
    result.position = light.p0Area.xyz * b0 + light.p1Cdf.xyz * b1 +
                      light.p2SelectionPdf.xyz * b2;
    vec3 delta = result.position - surface.position;
    float distanceSquared = dot(delta, delta);
    result.distanceToLight = sqrt(distanceSquared);
    result.direction = delta / max(result.distanceToLight, 1.0e-10);
    float receiverCosine = max(dot(surface.normal, result.direction), 0.0);
    float emitterCosine = max(dot(light.normalTriangle.xyz, -result.direction), 0.0);
    if (receiverCosine <= 0.0 || emitterCosine <= 0.0) return result;
    uint triangle = floatBitsToUint(light.normalTriangle.w);
    uint lightMaterial = triangles[triangle].indicesMaterial.w;
    vec3 emission = materials[lightMaterial].emission.rgb;
    vec3 bsdf = restirBsdf(surface, result.direction);
    result.rgb = emission * bsdf * (receiverCosine * emitterCosine /
                                    max(distanceSquared, 1.0e-20));
    result.target = max(luminance(result.rgb), 0.0);
    result.proposalArea = light.p2SelectionPdf.w / light.p0Area.w;
    return result;
}

uint restirSampleLight(float xi) {
    uint lo = 0u;
    uint hi = globals.extentCounts.w;
    while (lo < hi) {
        uint mid = (lo + hi) >> 1u;
        if (xi <= lights[mid].p1Cdf.w)
            hi = mid;
        else
            lo = mid + 1u;
    }
    return min(lo, globals.extentCounts.w - 1u);
}

Reservoir emptyReservoir() {
    Reservoir reservoir;
    reservoir.selected = uvec4(0u);
    reservoir.weights = vec4(0.0);
    return reservoir;
}

float reservoirNormalizer(Reservoir reservoir) {
    return reservoir.weights.w * reservoir.weights.y;
}

vec2 projectedPixel(mat4 viewProjection, vec3 position, vec2 jitter) {
    vec4 clip = viewProjection * vec4(position, 1.0);
    if (clip.w <= 0.0) return vec2(-1.0e10);
    vec2 ndc = clip.xy / clip.w;
    return (ndc * 0.5 + 0.5) * vec2(globals.extentCounts.xy) - vec2(0.5) + jitter;
}

#endif
