#extension GL_EXT_scalar_block_layout : require

struct Vertex { vec3 position; vec3 normal; vec2 uv; };
struct TriangleInfo { uint material; uint surface; };
struct Material { vec3 absorption; float ior; vec3 emission; uint type; };
struct Path {
    vec3 origin; float tMin;
    vec3 direction; float cone;
    vec3 throughput; uint slot;
    uint depth; uint flags; uint rng; float footprint;
    float lastBsdfPdf;
};
struct Hit { float distance; uint triangle; vec2 bary; };
struct Light { uint triangle; float area; float cumulativePower; };
struct Shadow { vec3 origin; float distance; vec3 direction; uint slot; vec3 contribution; uint valid; uint visible; };
struct Guide { vec3 position; uint surface; vec3 normal; uint material; vec3 albedo; float roughness; vec3 view; uint type; };
struct Reservoir { uint light; vec2 bary; float weightSum; float W; uint M; uint valid; uint reserved; };
struct Camera { vec3 position; float tanHalfFov; vec3 forward; uint width; vec3 right; uint height; vec3 up; uint frame; };
struct CameraPair { Camera current; Camera previous; uint valid; uint reserved0; uint reserved1; uint reserved2; };
struct History { vec3 color; float moment1; float moment2; uint historyLength; uint valid; uint reserved; };
struct FilterValue { vec3 color; float variance; };
struct OpticalPath { uint events[8]; uint reserved; uint chainLength; uint chainHash; uint flags; float distance; };
struct OpticalMeta { uint chainHash; uint chainLength; uint kind; uint valid; float distance; };
layout(set=0, binding=1, scalar) readonly buffer Vertices { Vertex vertices[]; };
layout(set=0, binding=2, scalar) readonly buffer Triangles { TriangleInfo triangles[]; };
layout(set=0, binding=3, scalar) readonly buffer Materials { Material materials[]; };
layout(set=0, binding=4) uniform sampler2DArray baseTexture;
layout(set=0, binding=5) uniform sampler2DArray roughTexture;
layout(set=0, binding=6, scalar) buffer Paths0 { Path paths0[]; };
layout(set=0, binding=10, scalar) buffer Paths1 { Path paths1[]; };
layout(set=0, binding=11, scalar) readonly buffer Lights { Light lights[]; };
layout(set=0, binding=12, scalar) buffer Shadows { Shadow shadows[]; };
layout(set=0, binding=13, scalar) buffer Reservoirs { Reservoir reservoirs[]; };
#ifdef DISPLAY_SHADER
layout(set=0, binding=14, scalar) readonly buffer Guides { Guide guides[]; };
#else
layout(set=0, binding=14, scalar) buffer Guides { Guide guides[]; };
#endif
layout(set=0, binding=15, scalar) readonly buffer Cameras { CameraPair cameras[]; };
#ifdef DISPLAY_SHADER
layout(set=0, binding=16, scalar) readonly buffer SignalFlags { uint signalFlags[]; };
layout(set=0, binding=18, scalar) readonly buffer Filters { FilterValue filters[]; };
#else
layout(set=0, binding=16, scalar) buffer SignalFlags { uint signalFlags[]; };
layout(set=0, binding=18, scalar) buffer Filters { FilterValue filters[]; };
#endif
layout(set=0, binding=17, scalar) buffer Histories { History histories[]; };
layout(set=0, binding=19, scalar) buffer OpticalPaths { OpticalPath opticalPaths[]; };
#ifdef DISPLAY_SHADER
layout(set=0, binding=20, scalar) readonly buffer OpticalGuides0 { Guide opticalGuides0[]; };
layout(set=0, binding=21, scalar) readonly buffer OpticalGuides1 { Guide opticalGuides1[]; };
layout(set=0, binding=22, scalar) readonly buffer OpticalMetadata { OpticalMeta opticalMetadata[]; };
layout(set=0, binding=24, scalar) readonly buffer OpticalFilters { FilterValue opticalFilters[]; };
#else
layout(set=0, binding=20, scalar) buffer OpticalGuides0 { Guide opticalGuides0[]; };
layout(set=0, binding=21, scalar) buffer OpticalGuides1 { Guide opticalGuides1[]; };
layout(set=0, binding=22, scalar) buffer OpticalMetadata { OpticalMeta opticalMetadata[]; };
layout(set=0, binding=24, scalar) buffer OpticalFilters { FilterValue opticalFilters[]; };
#endif
layout(set=0, binding=23, scalar) buffer OpticalHistories { History opticalHistories[]; };
layout(set=0, binding=25, scalar) buffer OpticalChains { uint opticalChains[]; };
#ifdef DISPLAY_SHADER
layout(set=0, binding=26, scalar) readonly buffer PrefixRadiance { vec4 prefixRadiance[]; };
layout(set=0, binding=27, scalar) readonly buffer PrefixOptical { vec4 prefixOptical[]; };
#else
layout(set=0, binding=26, scalar) buffer PrefixRadiance { vec4 prefixRadiance[]; };
layout(set=0, binding=27, scalar) buffer PrefixOptical { vec4 prefixOptical[]; };
#endif
layout(set=0, binding=7, scalar) buffer Hits { Hit hits[]; };
layout(set=0, binding=8, scalar) buffer Counters {
    uint count[2]; uint traceWidth; uint traceHeight; uint traceDepth;
    uint dispatchX; uint dispatchY; uint dispatchZ;
} counters;
#ifdef DISPLAY_SHADER
layout(set=0, binding=9, scalar) readonly buffer Radiance { vec4 radiance[]; };
#else
layout(set=0, binding=9, scalar) buffer Radiance { vec4 radiance[]; };
#endif
layout(push_constant, scalar) uniform Push {
    vec3 position; float tanHalfFov;
    vec3 forward; uint width;
    vec3 right; uint height;
    vec3 up; uint frame;
    uint mode; uint inputQueue; uint maxTraceWidth; uint maxGroupWidth;
    float lightPower; uint lightCount; uint cameraSlot; uint reserved1;
} pc;
uint hashValue(uint x) {
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; return x ^ (x >> 16);
}
float randomValue(inout uint state) {
    state = hashValue(state + 0x9e3779b9u);
    return float(state >> 8) * (1.0 / 16777216.0);
}
uint capacity() { return 2u * pc.width * pc.height; }
Path readPath(uint queue, uint index) { return queue == 0u ? paths0[index] : paths1[index]; }
void writePath(uint queue, uint index, Path path) { if (queue == 0u) paths0[index] = path; else paths1[index] = path; }
const float PI = 3.14159265358979323846;
float smithG1(float nDotV,float a2) { return 2.0*nDotV/(nDotV+sqrt(a2+(1.0-a2)*nDotV*nDotV)); }
float smithG2(float nDotV,float nDotL,float a2) {
    return 2.0*nDotV*nDotL/(nDotL*sqrt(a2+(1.0-a2)*nDotV*nDotV)+nDotV*sqrt(a2+(1.0-a2)*nDotL*nDotL));
}
float ggxD(float nDotH,float a2) { float d=nDotH*nDotH*(a2-1.0)+1.0; return a2/(PI*d*d); }
float powerWeight(float a,float b) { float aa=a*a,bb=b*b; return aa+bb>0.0 ? aa/(aa+bb) : 0.0; }
float lightAreaPdf(uint index) {
    float previous=index==0u ? 0.0 : lights[index-1u].cumulativePower;
    return (lights[index].cumulativePower-previous)/(pc.lightPower*lights[index].area);
}