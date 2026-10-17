#pragma once

// 本文件是 C++ 与 GLSL (shaders/common.glsl) 的共享契约：
// 描述符绑定、push constant 布局、GPU 结构体大小与域/标志枚举必须同步修改。

#include <cstdint>

namespace vpt {

// 单一描述符集，全部资源通过 descriptor buffer 绑定。
namespace bind {
constexpr std::uint32_t BaseColor = 0;      // combined image sampler（2D array, sRGB）
constexpr std::uint32_t Roughness = 1;      // combined image sampler（2D array, R8）
constexpr std::uint32_t Materials = 2;      // SSBO Material[]
constexpr std::uint32_t Triangles = 3;      // SSBO Triangle[]
constexpr std::uint32_t Instances = 4;      // SSBO InstanceDesc[]
constexpr std::uint32_t Lights = 5;         // SSBO Light[]
constexpr std::uint32_t Camera = 6;         // UBO Camera
constexpr std::uint32_t PathsA = 7;         // SSBO Path[]
constexpr std::uint32_t PathsB = 8;         // SSBO Path[]
constexpr std::uint32_t Hits = 9;           // SSBO Hit[]
constexpr std::uint32_t Primary = 10;       // SSBO PrimarySurface[2 * pixels]
constexpr std::uint32_t BranchContrib = 11; // SSBO vec4[2 * pixels]
constexpr std::uint32_t BranchInfo = 12;    // SSBO BranchInfo[2 * pixels]
constexpr std::uint32_t RestirInit = 13;    // SSBO Reservoir[pixels]
constexpr std::uint32_t RestirTemporal = 14;// SSBO Reservoir[pixels]
constexpr std::uint32_t RestirSpatial = 15; // SSBO Reservoir[pixels]
constexpr std::uint32_t RestirHistory = 16; // SSBO Reservoir[2 * pixels]
constexpr std::uint32_t RestirOut = 17;     // SSBO vec4[pixels]
constexpr std::uint32_t ShadowQueue = 18;   // SSBO ShadowTask[3 * pixels]
constexpr std::uint32_t ShadowResults = 19; // SSBO uvec4[3 * pixels]
constexpr std::uint32_t Counters = 20;      // SSBO Counters
constexpr std::uint32_t Indirect = 21;      // SSBO IndirectArgs
constexpr std::uint32_t DomainRadiance = 22; // SSBO vec4[6 * pixels]
constexpr std::uint32_t DomainGuide = 23;   // SSBO Guide[2 * 6 * pixels]
constexpr std::uint32_t DomainM1 = 24;      // SSBO vec4[2 * 6 * pixels]
constexpr std::uint32_t DomainM2 = 25;      // SSBO vec4[2 * 6 * pixels]
constexpr std::uint32_t DomainHist = 26;    // SSBO uvec4[2 * 6 * pixels]
constexpr std::uint32_t DomainFiltered = 27; // SSBO vec4[6 * pixels]
constexpr std::uint32_t ScratchColorA = 28; // SSBO vec4[pixels]
constexpr std::uint32_t ScratchColorB = 29; // SSBO vec4[pixels]
constexpr std::uint32_t ScratchVarA = 30;   // SSBO vec4[pixels]
constexpr std::uint32_t ScratchVarB = 31;   // SSBO vec4[pixels]
constexpr std::uint32_t Analytic = 32;      // SSBO vec4[pixels]
constexpr std::uint32_t Planes = 33;        // SSBO Plane[]
constexpr std::uint32_t Tlas = 34;          // TLAS acceleration structure
constexpr std::uint32_t Composite = 35;     // SSBO vec4[pixels]
constexpr std::uint32_t Count = 36;
} // namespace bind

namespace material_type {
constexpr std::uint32_t Diffuse = 0;
constexpr std::uint32_t RoughMetal = 1;
constexpr std::uint32_t Mirror = 2;
constexpr std::uint32_t Dielectric = 3;
constexpr std::uint32_t Emissive = 4;
} // namespace material_type

namespace domain {
constexpr std::uint32_t Diffuse = 0;
constexpr std::uint32_t Specular = 1;
constexpr std::uint32_t MirrorDiffuse = 2;
constexpr std::uint32_t MirrorSpecular = 3;
constexpr std::uint32_t GlassReflect = 4;
constexpr std::uint32_t GlassTransmit = 5;
constexpr std::uint32_t Count = 6;
} // namespace domain

namespace pass {
constexpr std::uint32_t Setup = 0;
constexpr std::uint32_t Shade = 1;
constexpr std::uint32_t Prepare = 2;
constexpr std::uint32_t RestirInitial = 3;
constexpr std::uint32_t RestirTemporal = 4;
constexpr std::uint32_t RestirSpatial = 5;
constexpr std::uint32_t RestirShadowGen = 6;
constexpr std::uint32_t ShadowResolve = 7;
constexpr std::uint32_t SvgfTemporal = 8;
constexpr std::uint32_t SvgfVariance = 9;
constexpr std::uint32_t Atrous = 10;
constexpr std::uint32_t Finalize = 11;
constexpr std::uint32_t Resolve = 12;
constexpr std::uint32_t Composite = 13;
} // namespace pass

namespace pathflag {
constexpr std::uint32_t HasTransmission = 1u << 5;
constexpr std::uint32_t GlassSplitDone = 1u << 6;
constexpr std::uint32_t SuppressEmitter = 1u << 7;
constexpr std::uint32_t LastDelta = 1u << 8;
constexpr std::uint32_t ChainIncludesGlass = 1u << 12;
} // namespace pathflag

namespace pathclass {
constexpr std::uint32_t PrimaryDiffuse = 0;
constexpr std::uint32_t PrimarySpecular = 1;
constexpr std::uint32_t Mirror = 2;
constexpr std::uint32_t Glass = 3;
} // namespace pathclass

namespace shadowkind {
constexpr std::uint32_t Branch = 0;
constexpr std::uint32_t Restir = 1;
} // namespace shadowkind

constexpr std::uint32_t kMaxScatter = 8;
constexpr std::uint32_t kMaxChainPlanes = 8;
constexpr std::uint32_t kRestirCandidates = 4;
constexpr std::uint32_t kRestirSpatialNeighbors = 4;
constexpr std::uint32_t kRestirHistoryCap = 32;
constexpr std::uint32_t kSvgfHistoryCap = 32;
constexpr std::uint32_t kAtrousIterations = 5;
constexpr float kNearPlane = 0.05f;

// std430 对齐的 112 字节路径记录。
// terminal: 终端位置/粗糙度 half 打包、材质/有效位、instance/primitive。
struct PathGpu {
    float originTmin[4];
    float dirTmax[4];
    float throughput[4];
    std::uint32_t ids[4];
    std::uint32_t misc[4];
    std::uint32_t terminal[4];
    std::uint32_t meta[4];
};
static_assert(sizeof(PathGpu) == 112, "PathGpu 必须为 112 字节");

struct HitGpu {
    float t;
    std::uint32_t instance;
    std::uint32_t primitive;
    float baryX;
    float baryY;
    float pad[3];
};
static_assert(sizeof(HitGpu) == 32, "HitGpu 必须为 32 字节");

struct MaterialGpu {
    float baseColor[4];   // linear rgb, w 未用
    float absorption[4];  // sigma rgb, w ior
    float emission[4];    // Le rgb, w metallic
    float params[4];      // roughness, type, layer, 未用
};
static_assert(sizeof(MaterialGpu) == 64, "MaterialGpu 必须为 64 字节");

struct TriangleGpu {
    float p0[4];
    float p1[4];
    float p2[4];
    float n[4];       // xyz 面法线，w 未用
    std::uint32_t extra[4]; // material, planeIndex, lightIndex, instance
};
static_assert(sizeof(TriangleGpu) == 80, "TriangleGpu 必须为 80 字节");

struct InstanceDescGpu {
    std::uint32_t triangleOffset;
    std::uint32_t triangleCount;
    std::uint32_t material;
    std::uint32_t pad;
};
static_assert(sizeof(InstanceDescGpu) == 16, "InstanceDescGpu 必须为 16 字节");

struct LightGpu {
    std::uint32_t ids[4];  // instance, primitive, triangleIndex, lightIndex
    float emissionArea[4]; // rgb, area
    float cdf[4];          // cdf, pad
};
static_assert(sizeof(LightGpu) == 48, "LightGpu 必须为 48 字节");

struct PlaneGpu {
    float n[4]; // xyz 单位法线, w 偏移 d (dot(n,p)=d)
};
static_assert(sizeof(PlaneGpu) == 16, "PlaneGpu 必须为 16 字节");

struct PrimarySurfaceGpu {
    float posDepth[4];    // xyz 世界位置, w 视深
    float normalRough[4]; // xyz 法线, w roughness
    float albedo[4];      // linear rgb, w 未用
    float uv[4];          // uv.xy, 未用
    float ids[4];         // instance, primitive, material, flags
    float tag[4];         // frame, pathClass, 未用
};
static_assert(sizeof(PrimarySurfaceGpu) == 96, "PrimarySurfaceGpu 必须为 96 字节");

// 32 字节 Guide：位置/深度与法线/粗糙度使用 half 打包，重投影时由相机矩阵现算。
struct GuideGpu {
    std::uint32_t posDepth[2];   // half4(pos.xyz, viewDepth)
    std::uint32_t normalRough[2]; // half4(normal.xyz, roughness)
    std::uint32_t chainHash;
    std::uint32_t terminalId;    // instance << 16 | primitive
    std::uint32_t materialFlags; // material | flags << 16
    std::uint32_t frameTag;
};
static_assert(sizeof(GuideGpu) == 32, "GuideGpu 必须为 32 字节");

struct BranchInfoGpu {
    GuideGpu guide;
    std::uint32_t domain;
    std::uint32_t frame;
    std::uint32_t valid;
    std::uint32_t pad;
};
static_assert(sizeof(BranchInfoGpu) == 48, "BranchInfoGpu 必须为 48 字节");

struct ReservoirGpu {
    std::uint32_t sample[4]; // lightIndex, valid, baryX bits, baryY bits
    float stats[4];          // W, M, targetSrc, pad
};
static_assert(sizeof(ReservoirGpu) == 32, "ReservoirGpu 必须为 32 字节");

struct ShadowTaskGpu {
    float originTmax[4]; // xyz origin, w tmax
    float target[4];     // xyz 目标点, w 未用
    std::uint32_t ids[4];// instance, primitive, targetIndex, kind
    float weight[4];     // rgb 预乘贡献, w 未用
};
static_assert(sizeof(ShadowTaskGpu) == 64, "ShadowTaskGpu 必须为 64 字节");

struct CountersGpu {
    std::uint32_t pathCount[2];
    std::uint32_t shadowCount;
    std::uint32_t shadowTaskCount;
    std::uint32_t overflow;
    std::uint32_t pad[3];
};
static_assert(sizeof(CountersGpu) == 32, "CountersGpu 必须为 32 字节");

struct IndirectArgsGpu {
    std::uint32_t queueTrace[4];
    std::uint32_t queueDispatch[4];
    std::uint32_t shadowTrace[4];
    std::uint32_t shadowDispatch[4];
};
static_assert(sizeof(IndirectArgsGpu) == 64, "IndirectArgsGpu 必须为 64 字节");

struct CameraGpu {
    float viewProjCurr[16];
    float viewProjPrev[16];
    float position[4];   // xyz, tanHalfFov
    float right[4];      // xyz, aspect
    float up[4];         // xyz, frame parity
    float forward[4];    // xyz, frame index (uint bits)
    float extent[4];     // width, height, 1/width, 1/height
    float flags[4];      // cameraMoved, reset, 未用
    float glassMin[4];   // 玻璃体 AABB 最小值（用于相机在玻璃内的介质初始化）
    float glassMax[4];   // 玻璃体 AABB 最大值
};
static_assert(sizeof(CameraGpu) == 256, "CameraGpu 必须为 256 字节");

struct PushConstantsGpu {
    std::uint32_t pass;
    std::uint32_t frame;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t domainIndex;
    std::uint32_t iteration;
    std::uint32_t queue;
    std::uint32_t flags;
    std::uint32_t pad[8];
};
static_assert(sizeof(PushConstantsGpu) == 64, "PushConstantsGpu 必须为 64 字节");

// 与场景相关的常量。
constexpr int kGridX = 16;
constexpr int kGridZ = 16;
constexpr int kGridYMin = -1;
constexpr int kGridYMax = 6;
constexpr int kGridYCount = kGridYMax - kGridYMin + 1;

} // namespace vpt
