#include "common.glsl"

const uint FRAME_HISTORY_VALID = 1u;
const uint FRAME_CAMERA_MOVED = 2u;
const uint FRAME_GLOBAL_RESET = 4u;

uint primaryDomain(uint materialIndex)
{
    return materials.values[materialIndex].typeTexture.x == MATERIAL_ROUGH_METAL
               ? DOMAIN_OPAQUE_SPECULAR
               : DOMAIN_OPAQUE_DIFFUSE;
}

BranchResult makePrimaryGuide(uint pixel, HitRecord hit, PathState path)
{
    BranchResult guide;
    vec3 normal = hit.normalFront.w > 0.5 ? hit.normalFront.xyz : -hit.normalFront.xyz;
    float cone = path.directionCone.w * max(hit.positionT.w, 0.0);
    float roughness = sampleRoughness(hit.ids.x, hit.uvCone.xy, cone);
    vec3 albedo = sampleBaseColor(hit.ids.x, hit.uvCone.xy, cone);
    guide.radiance = vec4(0.0);
    guide.guidePosition = vec4(hit.positionT.xyz, hit.positionT.w);
    guide.guideNormal = vec4(normal, roughness);
    guide.guideIds = uvec4(hit.ids.y, hit.ids.x, 0u,
                           GUIDE_VALID | (materials.values[hit.ids.x].typeTexture.x == MATERIAL_ROUGH_METAL
                                              ? GUIDE_SPECULAR
                                              : GUIDE_DIFFUSE));
    guide.albedo = vec4(albedo, 1.0);
    return guide;
}

float ggxD(float nDotH, float alpha)
{
    float a2 = alpha * alpha;
    float denominator = nDotH * nDotH * (a2 - 1.0) + 1.0;
    return a2 / max(PI * denominator * denominator, 1.0e-20);
}

float smithLambda(float nDotV, float alpha)
{
    float cosine2 = nDotV * nDotV;
    float tangent2 = max(1.0 - cosine2, 0.0) / max(cosine2, 1.0e-8);
    return 0.5 * (sqrt(1.0 + alpha * alpha * tangent2) - 1.0);
}

float smithG2(float nDotV, float nDotL, float alpha)
{
    return 1.0 / (1.0 + smithLambda(nDotV, alpha) + smithLambda(nDotL, alpha));
}

vec3 evaluateDirectAtHit(HitRecord hit, PathState path, uint lightIndex, vec2 sampleData,
                         out float target, out float proposal, out vec3 direction,
                         out float distanceToLight)
{
    Light light = lights.values[lightIndex];
    vec3 lightPosition = lightPoint(light, sampleData);
    vec3 delta = lightPosition - hit.positionT.xyz;
    float distanceSquared = dot(delta, delta);
    distanceToLight = sqrt(max(distanceSquared, 1.0e-20));
    direction = delta / distanceToLight;
    vec3 normal = hit.normalFront.w > 0.5 ? hit.normalFront.xyz : -hit.normalFront.xyz;
    float receiverCosine = max(dot(normal, direction), 0.0);
    float emitterCosine = max(dot(lightNormal(light), -direction), 0.0);
    Material material = materials.values[hit.ids.x];
    Material emitter = materials.values[light.data.y];
    float cone = path.directionCone.w * max(hit.positionT.w, 0.0);
    vec3 baseColor = sampleBaseColor(hit.ids.x, hit.uvCone.xy, cone);
    vec3 view = normalize(-path.directionCone.xyz);
    vec3 bsdf = vec3(0.0);
    if (material.typeTexture.x == MATERIAL_DIFFUSE) {
        bsdf = baseColor / PI;
    } else if (material.typeTexture.x == MATERIAL_ROUGH_METAL && receiverCosine > 0.0) {
        vec3 halfway = normalize(view + direction);
        float nDotV = max(dot(normal, view), 0.0);
        float nDotL = receiverCosine;
        float nDotH = max(dot(normal, halfway), 0.0);
        float vDotH = max(dot(view, halfway), 0.0);
        float roughness = max(sampleRoughness(hit.ids.x, hit.uvCone.xy, cone), 0.02);
        float alpha = roughness * roughness;
        bsdf = fresnelSchlick(baseColor, vDotH) * ggxD(nDotH, alpha) *
               smithG2(nDotV, nDotL, alpha) / max(4.0 * nDotV * nDotL, 1.0e-8);
    }
    vec3 contribution = emitter.emissionRoughness.xyz * bsdf *
                        (receiverCosine * emitterCosine / max(distanceSquared, 1.0e-10));
    proposal = light.cProbability.w / max(light.aArea.w, 1.0e-10);
    target = luminance(contribution);
    if (!(target > 0.0) || !(proposal > 0.0)) {
        target = 0.0;
        contribution = vec3(0.0);
    }
    return contribution;
}

float targetAtGuide(BranchResult guide, uint lightIndex, vec2 sampleData, vec3 cameraPosition)
{
    if ((guide.guideIds.w & GUIDE_VALID) == 0u) {
        return 0.0;
    }
    Light light = lights.values[lightIndex];
    vec3 delta = lightPoint(light, sampleData) - guide.guidePosition.xyz;
    float distanceSquared = dot(delta, delta);
    vec3 direction = delta * inversesqrt(max(distanceSquared, 1.0e-20));
    float receiverCosine = max(dot(guide.guideNormal.xyz, direction), 0.0);
    float emitterCosine = max(dot(lightNormal(light), -direction), 0.0);
    if (!(receiverCosine > 0.0 && emitterCosine > 0.0)) {
        return 0.0;
    }
    Material material = materials.values[guide.guideIds.y];
    Material emitter = materials.values[light.data.y];
    vec3 bsdf = guide.albedo.xyz / PI;
    if ((guide.guideIds.w & GUIDE_SPECULAR) != 0u) {
        vec3 view = normalize(cameraPosition - guide.guidePosition.xyz);
        vec3 halfway = normalize(view + direction);
        float nDotV = max(dot(guide.guideNormal.xyz, view), 0.0);
        float nDotL = receiverCosine;
        float nDotH = max(dot(guide.guideNormal.xyz, halfway), 0.0);
        float vDotH = max(dot(view, halfway), 0.0);
        float alpha = max(guide.guideNormal.w * guide.guideNormal.w, 0.0004);
        bsdf = fresnelSchlick(guide.albedo.xyz, vDotH) * ggxD(nDotH, alpha) *
               smithG2(nDotV, nDotL, alpha) / max(4.0 * nDotV * nDotL, 1.0e-8);
    }
    vec3 value = emitter.emissionRoughness.xyz * bsdf *
                 (receiverCosine * emitterCosine / max(distanceSquared, 1.0e-10));
    return max(luminance(value), 0.0);
}

Reservoir emptyReservoir()
{
    Reservoir reservoir;
    reservoir.sampleData = vec4(0.0);
    reservoir.weights = vec4(0.0);
    reservoir.ids = uvec4(0xffffffffu, 0xffffffffu, 0u, 0u);
    return reservoir;
}

void reservoirUpdate(inout Reservoir reservoir, uint lightIndex, vec2 sampleData, float target,
                     float proposal, float candidateWeight, float randomValue)
{
    if (!(candidateWeight > 0.0) || isnan(candidateWeight) || isinf(candidateWeight)) {
        return;
    }
    reservoir.weights.x += candidateWeight;
    if (randomValue * reservoir.weights.x < candidateWeight) {
        reservoir.sampleData = vec4(sampleData, target, proposal);
        reservoir.ids.x = lightIndex;
        reservoir.ids.y = lights.values[lightIndex].data.x;
    }
}
