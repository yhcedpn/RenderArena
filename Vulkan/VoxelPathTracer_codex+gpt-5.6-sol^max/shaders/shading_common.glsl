#include "restir_common.glsl"

vec3 sampleGgxVndf(vec3 viewLocal, float alpha, vec2 xi)
{
    vec3 view = normalize(vec3(alpha * viewLocal.x, alpha * viewLocal.y, viewLocal.z));
    float lensq = dot(view.xy, view.xy);
    vec3 tangent1 = lensq > 0.0 ? vec3(-view.y, view.x, 0.0) * inversesqrt(lensq)
                                : vec3(1.0, 0.0, 0.0);
    vec3 tangent2 = cross(view, tangent1);
    float radius = sqrt(xi.x);
    float phi = 2.0 * PI * xi.y;
    float t1 = radius * cos(phi);
    float t2 = radius * sin(phi);
    float blend = 0.5 * (1.0 + view.z);
    t2 = (1.0 - blend) * sqrt(max(0.0, 1.0 - t1 * t1)) + blend * t2;
    vec3 normal = t1 * tangent1 + t2 * tangent2 +
                  sqrt(max(0.0, 1.0 - t1 * t1 - t2 * t2)) * view;
    return normalize(vec3(alpha * normal.x, alpha * normal.y, max(normal.z, 0.0)));
}

vec3 evaluateMetal(vec3 baseColor, float roughness, vec3 normal, vec3 view, vec3 light,
                   out float pdf)
{
    float nDotV = max(dot(normal, view), 0.0);
    float nDotL = max(dot(normal, light), 0.0);
    if (!(nDotV > 0.0 && nDotL > 0.0)) {
        pdf = 0.0;
        return vec3(0.0);
    }
    vec3 halfway = normalize(view + light);
    float nDotH = max(dot(normal, halfway), 0.0);
    float vDotH = max(dot(view, halfway), 0.0);
    float alpha = max(roughness * roughness, 0.0004);
    float distribution = ggxD(nDotH, alpha);
    float geometry = smithG2(nDotV, nDotL, alpha);
    float g1 = 1.0 / (1.0 + smithLambda(nDotV, alpha));
    pdf = distribution * g1 / max(4.0 * nDotV, 1.0e-8);
    return fresnelSchlick(baseColor, vDotH) * distribution * geometry /
           max(4.0 * nDotV * nDotL, 1.0e-8);
}

void writeBranchResult(PathState path)
{
    uint slot = path.meta.x * 2u + min(path.meta.y, 1u);
    BranchResult result;
    result.radiance = vec4(path.radiancePdf.xyz, 1.0);
    result.guidePosition = path.terminalPosition;
    result.guideNormal = path.terminalNormal;
    uint guideFlags = (path.meta.w & PATH_TERMINAL_VALID) != 0u ? GUIDE_VALID : 0u;
    guideFlags |= (path.meta.w & PATH_TERMINAL_SPECULAR) != 0u ? GUIDE_SPECULAR : GUIDE_DIFFUSE;
    guideFlags |= (path.meta.w & PATH_TRANSMITTED) != 0u ? GUIDE_TRANSMITTED : 0u;
    guideFlags |= (path.optical.z & 0xffu) << 8u;
    guideFlags |= (path.optical.y & 0xffu) << 16u;
    result.guideIds = uvec4(floatBitsToUint(path.terminalAlbedo.w), path.optical.w,
                            path.optical.x, guideFlags);
    result.albedo = vec4(path.terminalAlbedo.xyz, 1.0);
    branchResults.values[slot] = result;
}

void writeCommonPrefix(PathState path)
{
    if (all(equal(path.radiancePdf.xyz, vec3(0.0)))) {
        return;
    }
    BranchResult commonValue;
    commonValue.radiance = vec4(path.radiancePdf.xyz, 1.0);
    commonValue.guidePosition = vec4(0.0);
    commonValue.guideNormal = vec4(0.0);
    commonValue.guideIds = uvec4(0u, 0u, path.optical.z, GUIDE_VALID);
    commonValue.albedo = vec4(1.0);
    commonResults.values[path.meta.x] = commonValue;
}

void establishTerminal(inout PathState path, HitRecord hit, vec3 orientedNormal,
                       vec3 albedo, float roughness, bool specular)
{
    if ((path.meta.w & PATH_PURE_DELTA) == 0u || (path.meta.w & PATH_TERMINAL_VALID) != 0u) {
        return;
    }
    vec3 terminalPosition = (path.meta.w & PATH_TRANSMITTED) != 0u
                                ? hit.positionT.xyz
                                : transformPoint(path, hit.positionT.xyz);
    vec3 terminalNormal = (path.meta.w & PATH_TRANSMITTED) != 0u
                              ? orientedNormal
                              : transformNormal(path, orientedNormal);
    float depth = dot(terminalPosition - frameData().cameraPosition.xyz,
                      frameData().cameraForward.xyz);
    path.terminalPosition = vec4(terminalPosition, depth);
    path.terminalNormal = vec4(terminalNormal, roughness);
    path.terminalAlbedo = vec4(albedo, uintBitsToFloat(hit.ids.y));
    path.optical.w = hit.ids.x;
    path.meta.w |= PATH_TERMINAL_VALID;
    if (specular) {
        path.meta.w |= PATH_TERMINAL_SPECULAR;
    }
}

bool russianRoulette(inout PathState path, uint branchSalt)
{
    if (path.meta.z < 4u) {
        return true;
    }
    float probability = clamp(max(path.throughputEta.x,
                                  max(path.throughputEta.y, path.throughputEta.z)),
                              0.05, 0.95);
    float xi = randomDimension(path.meta.x, frameData().extentFrameFlags.z,
                               101u + path.meta.z * 7u, branchSalt);
    if (xi >= probability) {
        return false;
    }
    path.throughputEta.xyz /= probability;
    return true;
}

uint appendPath(PathState path, bool writeToA)
{
    uint index = atomicAdd(counters.words[1], 1u);
    if (index >= frameData().counts.y) {
        atomicOr(counters.words[3], 1u);
        return 0xffffffffu;
    }
    if (writeToA) {
        pathsA.values[index] = path;
    } else {
        pathsB.values[index] = path;
    }
    return index;
}

void appendShadowTask(vec3 origin, vec3 direction, float maximumDistance, vec3 contribution,
                      uint targetIndex, uint targetType, uint targetInA)
{
    uint index = atomicAdd(counters.words[2], 1u);
    if (index >= frameData().counts.y) {
        atomicOr(counters.words[3], 2u);
        return;
    }
    ShadowTask task;
    task.originTmax = vec4(origin, maximumDistance);
    task.direction = vec4(direction, 0.0);
    task.contribution = vec4(contribution, 0.0);
    task.meta = uvec4(targetIndex, targetType, targetInA, 0u);
    shadowTasks.values[index] = task;
}

vec3 makeNeeContribution(PathState path, HitRecord hit, vec3 normal, vec3 view,
                         vec3 baseColor, float roughness, uint lightIndex, vec2 sampleData,
                         out vec3 direction, out float maximumDistance)
{
    Light light = lights.values[lightIndex];
    vec3 lightPosition = lightPoint(light, sampleData);
    vec3 delta = lightPosition - hit.positionT.xyz;
    float distanceSquared = dot(delta, delta);
    maximumDistance = sqrt(max(distanceSquared, 1.0e-20));
    direction = delta / maximumDistance;
    float nDotL = max(dot(normal, direction), 0.0);
    float emitterCosine = max(dot(lightNormal(light), -direction), 0.0);
    if (!(nDotL > 0.0 && emitterCosine > 0.0)) {
        return vec3(0.0);
    }
    uint type = materials.values[hit.ids.x].typeTexture.x;
    vec3 bsdf;
    float bsdfPdf;
    if (type == MATERIAL_DIFFUSE) {
        bsdf = baseColor / PI;
        bsdfPdf = nDotL / PI;
    } else {
        bsdf = evaluateMetal(baseColor, roughness, normal, view, direction, bsdfPdf);
    }
    float areaPdf = light.cProbability.w / max(light.aArea.w, 1.0e-10);
    float directionPdf = areaPdf * distanceSquared / max(emitterCosine, 1.0e-10);
    float mis = powerHeuristic(directionPdf, bsdfPdf);
    vec3 emission = materials.values[light.data.y].emissionRoughness.xyz;
    return path.throughputEta.xyz * emission * bsdf *
           (nDotL * emitterCosine / max(distanceSquared * areaPdf, 1.0e-20)) * mis;
}

float emitterDirectionPdf(HitRecord hit, PathState path)
{
    uint lightPlusOne = triangles.values[hit.ids.z].data.w;
    if (lightPlusOne == 0u) {
        return 0.0;
    }
    Light light = lights.values[lightPlusOne - 1u];
    vec3 delta = hit.positionT.xyz - path.originTmin.xyz;
    float distanceSquared = dot(delta, delta);
    float emitterCosine = max(dot(lightNormal(light), -normalize(delta)), 0.0);
    float areaPdf = light.cProbability.w / max(light.aArea.w, 1.0e-10);
    return areaPdf * distanceSquared / max(emitterCosine, 1.0e-10);
}
