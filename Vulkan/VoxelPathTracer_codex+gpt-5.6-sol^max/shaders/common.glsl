#extension GL_EXT_scalar_block_layout : require

const uint DOMAIN_OPAQUE_DIFFUSE = 0u;
const uint DOMAIN_OPAQUE_SPECULAR = 1u;
const uint DOMAIN_REFLECTION = 2u;
const uint DOMAIN_GLASS_REFLECTION = 3u;
const uint DOMAIN_TRANSMISSION = 4u;
const uint DOMAIN_COUNT = 5u;
const uint INVALID_DOMAIN = 0xffffffffu;

const uint MATERIAL_DIFFUSE = 0u;
const uint MATERIAL_ROUGH_METAL = 1u;
const uint MATERIAL_MIRROR = 2u;
const uint MATERIAL_DIELECTRIC = 3u;
const uint MATERIAL_EMISSIVE = 4u;

const uint PATH_INSIDE_GLASS = 1u << 0u;
const uint PATH_GLASS_SPLIT = 1u << 1u;
const uint PATH_PURE_DELTA = 1u << 2u;
const uint PATH_LAST_RESTIR = 1u << 3u;
const uint PATH_LAST_DELTA = 1u << 4u;
const uint PATH_TERMINAL_VALID = 1u << 5u;
const uint PATH_TERMINAL_SPECULAR = 1u << 6u;
const uint PATH_TRANSMITTED = 1u << 7u;

const uint GUIDE_VALID = 1u;
const uint GUIDE_DIFFUSE = 1u << 1u;
const uint GUIDE_SPECULAR = 1u << 2u;
const uint GUIDE_TRANSMITTED = 1u << 3u;

const float PI = 3.14159265358979323846;
const float RAY_EPSILON = 0.001;

struct FrameData {
    vec4 cameraPosition;
    vec4 cameraRight;
    vec4 cameraUp;
    vec4 cameraForward;
    vec4 previousPosition;
    vec4 previousRight;
    vec4 previousUp;
    vec4 previousForward;
    uvec4 extentFrameFlags;
    vec4 jitter;
    vec4 projection;
    uvec4 counts;
    vec4 glassMinimum;
    vec4 glassMaximum;
};

struct Vertex {
    vec4 position;
    vec4 normal;
    vec4 uv;
};

struct TriangleInfo {
    uvec4 data;
};

struct Material {
    vec4 baseMetal;
    vec4 absorptionIor;
    vec4 emissionRoughness;
    uvec4 typeTexture;
};

struct Light {
    vec4 aArea;
    vec4 bCdf;
    vec4 cProbability;
    uvec4 data;
};

struct PathState {
    vec4 originTmin;
    vec4 directionCone;
    vec4 throughputEta;
    vec4 radiancePdf;
    vec4 affine0;
    vec4 affine1;
    vec4 affine2;
    vec4 terminalPosition;
    vec4 terminalNormal;
    vec4 terminalAlbedo;
    uvec4 meta;
    uvec4 optical;
};

struct HitRecord {
    vec4 positionT;
    vec4 normalFront;
    vec4 uvCone;
    uvec4 ids;
};

struct ShadowTask {
    vec4 originTmax;
    vec4 direction;
    vec4 contribution;
    uvec4 meta;
};

struct BranchResult {
    vec4 radiance;
    vec4 guidePosition;
    vec4 guideNormal;
    uvec4 guideIds;
    vec4 albedo;
};

struct Reservoir {
    vec4 sampleData;
    vec4 weights;
    uvec4 ids;
};

layout(push_constant, scalar) uniform PushConstants {
    uint frameSlot;
    uint passIndex;
    uint auxiliary;
    uint flags;
} pc;

layout(set = 0, binding = 0, scalar) readonly buffer FrameBuffer { FrameData frames[]; } frameBuffer;
layout(set = 0, binding = 1, scalar) readonly buffer VertexBuffer { Vertex values[]; } vertices;
layout(set = 0, binding = 2, scalar) readonly buffer IndexBuffer { uint values[]; } indices;
layout(set = 0, binding = 3, scalar) readonly buffer TriangleBuffer { TriangleInfo values[]; } triangles;
layout(set = 0, binding = 4, scalar) readonly buffer MaterialBuffer { Material values[]; } materials;
layout(set = 0, binding = 5, scalar) readonly buffer LightBuffer { Light values[]; } lights;
layout(set = 0, binding = 7, scalar) buffer PathBufferA { PathState values[]; } pathsA;
layout(set = 0, binding = 8, scalar) buffer PathBufferB { PathState values[]; } pathsB;
layout(set = 0, binding = 9, scalar) buffer HitBuffer { HitRecord values[]; } hits;
layout(set = 0, binding = 10, scalar) buffer CounterBuffer { uint words[]; } counters;
layout(set = 0, binding = 11, scalar) buffer ShadowTaskBuffer { ShadowTask values[]; } shadowTasks;
layout(set = 0, binding = 12, scalar) buffer ShadowVisibilityBuffer { uint values[]; } shadowVisibility;
layout(set = 0, binding = 13, scalar) buffer BranchResultBuffer { BranchResult values[]; } branchResults;
layout(set = 0, binding = 14, scalar) buffer CommonResultBuffer { BranchResult values[]; } commonResults;
layout(set = 0, binding = 15, scalar) buffer AnalyticBuffer { vec4 values[]; } analyticValues;
layout(set = 0, binding = 16, scalar) buffer RestirDirectBuffer { vec4 values[]; } restirDirect;
layout(set = 0, binding = 17, scalar) buffer ReservoirInitialBuffer { Reservoir values[]; } reservoirInitial;
layout(set = 0, binding = 18, scalar) buffer ReservoirTemporalBuffer { Reservoir values[]; } reservoirTemporal;
layout(set = 0, binding = 19, scalar) buffer ReservoirSpatialBuffer { Reservoir values[]; } reservoirSpatial;
layout(set = 0, binding = 20, scalar) readonly buffer ReservoirPreviousBuffer { Reservoir values[]; } reservoirPrevious;
layout(set = 0, binding = 21, scalar) buffer RawSignalBuffer { vec4 values[]; } rawSignals;
layout(set = 0, binding = 22, scalar) buffer AlbedoBuffer { vec4 values[]; } albedoValues;
layout(set = 0, binding = 23, scalar) buffer GuideCurrentBuffer { BranchResult values[]; } guidesCurrent;
layout(set = 0, binding = 24, scalar) readonly buffer GuidePreviousBuffer { BranchResult values[]; } guidesPrevious;
layout(set = 0, binding = 25, scalar) readonly buffer HistoryColorPreviousBuffer { vec4 values[]; } historyColorPrevious;
layout(set = 0, binding = 26, scalar) buffer HistoryColorCurrentBuffer { vec4 values[]; } historyColorCurrent;
layout(set = 0, binding = 27, scalar) readonly buffer HistoryMomentPreviousBuffer { vec4 values[]; } historyMomentPrevious;
layout(set = 0, binding = 28, scalar) buffer HistoryMomentCurrentBuffer { vec4 values[]; } historyMomentCurrent;
layout(set = 0, binding = 29, scalar) readonly buffer HistoryLengthPreviousBuffer { uint values[]; } historyLengthPrevious;
layout(set = 0, binding = 30, scalar) buffer HistoryLengthCurrentBuffer { uint values[]; } historyLengthCurrent;
layout(set = 0, binding = 31, scalar) buffer AtrousBufferA { vec4 values[]; } atrousA;
layout(set = 0, binding = 32, scalar) buffer AtrousBufferB { vec4 values[]; } atrousB;
#ifdef FINAL_COLOR_READ_ONLY
layout(set = 0, binding = 33, scalar) readonly buffer FinalColorBuffer { vec4 values[]; } finalColors;
#else
layout(set = 0, binding = 33, scalar) buffer FinalColorBuffer { vec4 values[]; } finalColors;
#endif
layout(set = 0, binding = 34) uniform sampler2DArray colorTextures;
layout(set = 0, binding = 35) uniform sampler2DArray roughnessTextures;

FrameData frameData()
{
    return frameBuffer.frames[pc.frameSlot];
}

uint pixelIndex(uvec2 pixel, uvec2 extent)
{
    return pixel.y * extent.x + pixel.x;
}

uint signalIndex(uint pixel, uint domain, uint pixelCount)
{
    return domain * pixelCount + pixel;
}

uint hashBits(uint value)
{
    value ^= value >> 16u;
    value *= 0x7feb352du;
    value ^= value >> 15u;
    value *= 0x846ca68bu;
    value ^= value >> 16u;
    return value;
}

float randomDimension(uint pixel, uint frame, uint dimension, uint branch)
{
    uint value = pixel * 0x9e3779b9u ^ frame * 0x85ebca6bu ^ dimension * 0xc2b2ae35u ^
                 branch * 0x27d4eb2fu;
    return float(hashBits(value) & 0x00ffffffu) * (1.0 / 16777216.0);
}

float luminance(vec3 value)
{
    return dot(value, vec3(0.2126, 0.7152, 0.0722));
}

mat3 makeBasis(vec3 normal)
{
    vec3 tangent = normalize(abs(normal.z) < 0.999 ? cross(vec3(0.0, 0.0, 1.0), normal)
                                                    : cross(vec3(0.0, 1.0, 0.0), normal));
    return mat3(tangent, cross(normal, tangent), normal);
}

vec3 sampleCosineHemisphere(vec2 xi)
{
    float radius = sqrt(xi.x);
    float phi = 2.0 * PI * xi.y;
    return vec3(radius * cos(phi), radius * sin(phi), sqrt(max(0.0, 1.0 - xi.x)));
}

vec3 sampleBaseColor(uint materialIndex, vec2 uv, float coneWidth)
{
    Material material = materials.values[materialIndex];
    float lod = clamp(log2(max(coneWidth * float(material.typeTexture.z), 1.0)), 0.0,
                      float(textureQueryLevels(colorTextures) - 1));
    return textureLod(colorTextures, vec3(uv, float(material.typeTexture.y)), lod).rgb;
}

float sampleRoughness(uint materialIndex, vec2 uv, float coneWidth)
{
    Material material = materials.values[materialIndex];
    float lod = clamp(log2(max(coneWidth * float(material.typeTexture.z), 1.0)), 0.0,
                      float(textureQueryLevels(roughnessTextures) - 1));
    return textureLod(roughnessTextures, vec3(uv, float(material.typeTexture.y)), lod).r;
}

vec3 transformPoint(PathState path, vec3 point)
{
    return vec3(dot(path.affine0.xyz, point) + path.affine0.w,
                dot(path.affine1.xyz, point) + path.affine1.w,
                dot(path.affine2.xyz, point) + path.affine2.w);
}

vec3 transformNormal(PathState path, vec3 normal)
{
    return normalize(vec3(dot(path.affine0.xyz, normal), dot(path.affine1.xyz, normal),
                          dot(path.affine2.xyz, normal)));
}

void appendReflectionTransform(inout PathState path, vec3 normal, vec3 point)
{
    mat3 reflection = mat3(1.0) - 2.0 * outerProduct(normal, normal);
    vec3 translation = 2.0 * normal * dot(normal, point);
    mat3 current = mat3(path.affine0.xyz, path.affine1.xyz, path.affine2.xyz);
    current = transpose(current);
    vec3 currentTranslation = vec3(path.affine0.w, path.affine1.w, path.affine2.w);
    mat3 combined = current * reflection;
    vec3 combinedTranslation = current * translation + currentTranslation;
    mat3 rows = transpose(combined);
    path.affine0 = vec4(rows[0], combinedTranslation.x);
    path.affine1 = vec4(rows[1], combinedTranslation.y);
    path.affine2 = vec4(rows[2], combinedTranslation.z);
}

vec2 projectCurrent(vec3 world)
{
    FrameData frame = frameData();
    vec3 relative = world - frame.cameraPosition.xyz;
    float depth = dot(relative, frame.cameraForward.xyz);
    if (depth <= 0.0001) {
        return vec2(-1.0e20);
    }
    vec2 ndc = vec2(dot(relative, frame.cameraRight.xyz) /
                        (depth * frame.projection.x * frame.projection.y),
                    dot(relative, frame.cameraUp.xyz) / (depth * frame.projection.x));
    vec2 unjittered = vec2((ndc.x * 0.5 + 0.5) * float(frame.extentFrameFlags.x) - 0.5,
                           (0.5 - ndc.y * 0.5) * float(frame.extentFrameFlags.y) - 0.5);
    return unjittered - frame.jitter.xy;
}

vec2 projectPrevious(vec3 world)
{
    FrameData frame = frameData();
    vec3 relative = world - frame.previousPosition.xyz;
    float depth = dot(relative, frame.previousForward.xyz);
    if (depth <= 0.0001) {
        return vec2(-1.0e20);
    }
    vec2 ndc = vec2(dot(relative, frame.previousRight.xyz) /
                        (depth * frame.projection.z * frame.projection.w),
                    dot(relative, frame.previousUp.xyz) / (depth * frame.projection.z));
    vec2 unjittered = vec2((ndc.x * 0.5 + 0.5) * float(frame.extentFrameFlags.x) - 0.5,
                           (0.5 - ndc.y * 0.5) * float(frame.extentFrameFlags.y) - 0.5);
    return unjittered - frame.jitter.zw;
}

bool guideCompatible(BranchResult current, BranchResult previous)
{
    if ((current.guideIds.w & GUIDE_VALID) == 0u || (previous.guideIds.w & GUIDE_VALID) == 0u) {
        return false;
    }
    if (current.guideIds.xyz != previous.guideIds.xyz) {
        return false;
    }
    float depthTolerance = 0.04 + 0.01 * max(current.guidePosition.w, previous.guidePosition.w);
    return distance(current.guidePosition.xyz, previous.guidePosition.xyz) <= depthTolerance &&
           dot(current.guideNormal.xyz, previous.guideNormal.xyz) >= 0.9 &&
           abs(current.guideNormal.w - previous.guideNormal.w) <= 0.12;
}

vec3 lightPoint(Light light, vec2 barycentric)
{
    float su = sqrt(clamp(barycentric.x, 0.0, 1.0));
    float b0 = 1.0 - su;
    float b1 = barycentric.y * su;
    float b2 = 1.0 - b0 - b1;
    return b0 * light.aArea.xyz + b1 * light.bCdf.xyz + b2 * light.cProbability.xyz;
}

vec3 lightNormal(Light light)
{
    return normalize(cross(light.bCdf.xyz - light.aArea.xyz,
                           light.cProbability.xyz - light.aArea.xyz));
}

uint selectLight(float xi, uint lightCount)
{
    uint low = 0u;
    uint high = lightCount;
    while (low + 1u < high) {
        uint middle = (low + high) >> 1u;
        if (xi <= lights.values[middle].bCdf.w) {
            high = middle;
        } else {
            low = middle;
        }
    }
    if (xi <= lights.values[0].bCdf.w) {
        return 0u;
    }
    return min(high, lightCount - 1u);
}

vec3 fresnelSchlick(vec3 f0, float cosine)
{
    float factor = pow(clamp(1.0 - cosine, 0.0, 1.0), 5.0);
    return f0 + (vec3(1.0) - f0) * factor;
}

float dielectricFresnel(float cosineI, float etaI, float etaT)
{
    cosineI = clamp(cosineI, 0.0, 1.0);
    float sineT2 = (etaI / etaT) * (etaI / etaT) * max(0.0, 1.0 - cosineI * cosineI);
    if (sineT2 >= 1.0) {
        return 1.0;
    }
    float cosineT = sqrt(max(0.0, 1.0 - sineT2));
    float rs = (etaI * cosineI - etaT * cosineT) / (etaI * cosineI + etaT * cosineT);
    float rp = (etaT * cosineI - etaI * cosineT) / (etaT * cosineI + etaI * cosineT);
    return 0.5 * (rs * rs + rp * rp);
}

float powerHeuristic(float a, float b)
{
    float aa = a * a;
    float bb = b * b;
    return aa / max(aa + bb, 1.0e-20);
}
