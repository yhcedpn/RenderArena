#include "common.glsl"

uint restirSelectLight(float xi, out float selectionPdf) {
    uint lightCount = frames[pc.frameSlot].frameInfo.y;
    uint selected = lightCount - 1u;
    float previous = 0.0;
    for (uint i = 0u; i < lightCount; ++i) {
        float cdf = lights[i].p1Cdf.w;
        if (xi <= cdf && selected == lightCount - 1u) selected = i;
        if (i == selected) {
            selectionPdf = cdf - previous;
            return selected;
        }
        previous = cdf;
    }
    selectionPdf = 1.0;
    return selected;
}

vec3 restirSamplePosition(uint lightIndex, vec2 bary) {
    float su = sqrt(bary.x);
    return (1.0 - su) * lights[lightIndex].p0Area.xyz
        + su * (1.0 - bary.y) * lights[lightIndex].p1Cdf.xyz
        + su * bary.y * lights[lightIndex].p2Pad.xyz;
}

vec3 evaluateDirect(Surface surface, uint lightIndex, vec2 bary, out float target, out float proposalPdf) {
    Light light = lights[lightIndex];
    vec3 samplePosition = restirSamplePosition(lightIndex, bary);
    vec3 toLight = samplePosition - surface.positionValid.xyz;
    float distanceSquared = dot(toLight, toLight);
    vec3 wi = toLight * inversesqrt(max(distanceSquared, 1e-12));
    float receiverCosine = max(dot(surface.normalRoughness.xyz, wi), 0.0);
    float emitterCosine = max(dot(light.normalPad.xyz, -wi), 0.0);
    float previous = lightIndex == 0u ? 0.0 : lights[lightIndex - 1u].p1Cdf.w;
    float selectionPdf = light.p1Cdf.w - previous;
    proposalPdf = selectionPdf / light.p0Area.w;
    vec3 f;
    if (uint(surface.albedoType.w + 0.5) == 0u) {
        f = surface.albedoType.xyz / PI;
    } else {
        float ignored;
        vec3 viewDirection = normalize(frames[pc.frameSlot].cameraPositionTanFov.xyz - surface.positionValid.xyz);
        f = evaluateGgx(viewDirection, wi, surface.normalRoughness.xyz, surface.albedoType.xyz, surface.normalRoughness.w, ignored);
    }
    vec3 value = f * light.emissionPower.xyz * (receiverCosine * emitterCosine / max(distanceSquared, 1e-12));
    target = max(luminance(value), 0.0);
    return value;
}

bool compatibleSurface(Surface a, Surface b) {
    if (a.positionValid.w == 0.0 || b.positionValid.w == 0.0) return false;
    if (a.identity.x != b.identity.x || a.identity.y != b.identity.y) return false;
    if (dot(a.normalRoughness.xyz, b.normalRoughness.xyz) < 0.9) return false;
    if (distance(a.positionValid.xyz, b.positionValid.xyz) > 0.1) return false;
    return abs(a.normalRoughness.w - b.normalRoughness.w) <= 0.1;
}

void emptyReservoir(out Reservoir reservoir) {
    reservoir.identity = uvec4(INVALID_ID, 0u, 0u, 0u);
    reservoir.baryTarget = vec4(0.0);
    reservoir.weights = vec4(0.0);
    reservoir.samplePosition = vec4(0.0);
}

void reservoirUpdate(inout Reservoir reservoir, uint lightIndex, vec2 bary, float target, float sourceTarget, float weight, uint multiplicity, float randomValue) {
    if (!(weight > 0.0) || isnan(weight) || isinf(weight)) return;
    float newSum = reservoir.weights.x + weight;
    if (randomValue * newSum < weight) {
        reservoir.identity.x = lightIndex;
        reservoir.baryTarget = vec4(bary, target, sourceTarget);
        reservoir.samplePosition = vec4(restirSamplePosition(lightIndex, bary), 1.0);
    }
    reservoir.weights.x = newSum;
    reservoir.identity.y += multiplicity;
    reservoir.identity.z = 1u;
}
