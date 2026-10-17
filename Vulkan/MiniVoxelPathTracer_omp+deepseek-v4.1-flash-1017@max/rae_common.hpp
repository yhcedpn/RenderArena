// 公共数学类型、GPU 缓冲布局与跨 C++/GLSL 的常量。
// 所有 GPU 结构都按 16 字节 vec4 对齐，确保 std430 布局在两端一致。
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

namespace rae {

// 路径队列与像素容量设计：
//   * 每个主样本最多 1 次首次玻璃拆分，因此活跃路径数 <= 2 * 像素数。
//   * 每个活跃路径每反弹最多生成 1 个 NEE shadow 任务，因此 shadow 任务数 <= 2 * 像素数。
//   * 每个像素使用 3 个独立信号槽：0=主路径，1=首次玻璃反射分支，2=首次玻璃透射分支。
constexpr uint32_t kMaxScatterEvents = 8;
constexpr uint32_t kSlotsPerPixel = 3;
constexpr uint32_t kMaxHistoryFrames = 32;
constexpr uint32_t kAtrousIterations = 5;
constexpr uint32_t kThreadsPerGroup = 64;
constexpr uint32_t kDomainNone = 0xFFFFFFFFu;

// 信号域只用于语义与历史验证；物理滤波按槽位独立进行。
enum Domain : uint32_t {
    kDomainPrimaryDiffuse = 0,
    kDomainPrimaryGlossy = 1,
    kDomainReflection = 2,
    kDomainTransmission = 3,
};

enum ChainType : uint32_t {
    kChainPrimary = 0,
    kChainMirror = 1,
    kChainGlassReflection = 2,
    kChainGlassTransmission = 3,
};

enum MotionMode : uint32_t {
    kMotionPrimary = 0,
    kMotionReflection = 1,
    kMotionTransmission = 2,
};

enum LobeType : uint32_t {
    kLobeDiffuse = 0,
    kLobeGlossy = 1,
    kLobeEmissive = 2,
};

// 材质类型与 JSON 中的 type 字符串一一对应。
enum MaterialType : uint32_t {
    kMatDiffuse = 0,
    kMatRoughMetal = 1,
    kMatMirror = 2,
    kMatDielectric = 3,
    kMatEmissive = 4,
};

enum PatternType : uint32_t {
    kPatternSolid = 0,
    kPatternStone = 1,
    kPatternWood = 2,
};

// 路径标志位（meta0.w）：
// bit0     介质为玻璃
// bit1     已发生首次玻璃拆分
// bit2     已到达光学链终端（guide 已写入）
// bit3..4  emitter 命中策略：0=ReSTIR 独占（丢弃），1=NEE MIS，2=delta 后权重 1
// bit5..6  分支编号：0=主分支，1=玻璃反射，2=玻璃透射
// bit7..9  光学链类型（ChainType）
// bit10..15 保留
constexpr uint32_t kPathFlagMedium = 1u << 0;
constexpr uint32_t kPathFlagSplit = 1u << 1;
constexpr uint32_t kPathFlagTerminal = 1u << 2;
constexpr uint32_t kPathFlagEmitterShift = 3;
constexpr uint32_t kPathFlagBranchShift = 5;
constexpr uint32_t kPathFlagChainShift = 7;
inline uint32_t pathPackedFlags(bool medium, bool split, bool terminal, uint32_t emitterPolicy,
                                uint32_t branch, uint32_t chainType) {
    return (medium ? kPathFlagMedium : 0u) | (split ? kPathFlagSplit : 0u) |
           (terminal ? kPathFlagTerminal : 0u) | ((emitterPolicy & 3u) << kPathFlagEmitterShift) |
           ((branch & 3u) << kPathFlagBranchShift) | ((chainType & 7u) << kPathFlagChainShift);
}
inline bool pathMedium(uint32_t flags) { return (flags & kPathFlagMedium) != 0u; }
inline bool pathSplit(uint32_t flags) { return (flags & kPathFlagSplit) != 0u; }
inline bool pathTerminal(uint32_t flags) { return (flags & kPathFlagTerminal) != 0u; }
inline uint32_t pathEmitterPolicy(uint32_t flags) {
    return (flags >> kPathFlagEmitterShift) & 3u;
}
inline uint32_t pathBranch(uint32_t flags) { return (flags >> kPathFlagBranchShift) & 3u; }
inline uint32_t pathChainType(uint32_t flags) { return (flags >> kPathFlagChainShift) & 7u; }

// guide 标志位（meta.z）：
// bit0     valid
// bit1..2  lobe
// bit3..4  motion
// bit5..7  chainType
// bit8..10 domain
// bit11..15 material
constexpr uint32_t kGuideFlagValid = 1u << 0;
inline uint32_t guideFlags(bool valid, uint32_t lobe, uint32_t motion, uint32_t chainType,
                           uint32_t domain, uint32_t material) {
    return (valid ? kGuideFlagValid : 0u) | ((lobe & 3u) << 1) | ((motion & 3u) << 3) |
           ((chainType & 7u) << 5) | ((domain & 7u) << 8) | ((material & 31u) << 11);
}
inline bool guideValid(uint32_t f) { return (f & kGuideFlagValid) != 0u; }
inline uint32_t guideLobe(uint32_t f) { return (f >> 1) & 3u; }
inline uint32_t guideMotion(uint32_t f) { return (f >> 3) & 3u; }
inline uint32_t guideChainType(uint32_t f) { return (f >> 5) & 7u; }
inline uint32_t guideDomain(uint32_t f) { return (f >> 8) & 7u; }
inline uint32_t guideMaterial(uint32_t f) { return (f >> 11) & 31u; }

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    constexpr Vec3() = default;
    constexpr Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
    explicit constexpr Vec3(float s) : x(s), y(s), z(s) {}

    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    Vec3 operator/(float s) const { return {x / s, y / s, z / s}; }
    Vec3& operator+=(const Vec3& o) {
        x += o.x;
        y += o.y;
        z += o.z;
        return *this;
    }
    Vec3& operator-=(const Vec3& o) {
        x -= o.x;
        y -= o.y;
        z -= o.z;
        return *this;
    }
};

inline float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float length(const Vec3& v) { return std::sqrt(dot(v, v)); }
inline Vec3 normalize(const Vec3& v) {
    const float len = length(v);
    return len > 1e-12f ? v / len : Vec3(0.0f, 0.0f, 0.0f);
}
inline Vec3 vmin(const Vec3& a, const Vec3& b) {
    return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)};
}
inline Vec3 vmax(const Vec3& a, const Vec3& b) {
    return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)};
}

struct Camera {
    Vec3 pos;
    Vec3 forward{0.0f, 0.0f, -1.0f};
    Vec3 right{1.0f, 0.0f, 0.0f};
    Vec3 up{0.0f, 1.0f, 0.0f};
    float tanHalfFovY = 0.577350269f;
    float aspect = 16.0f / 9.0f;
};

inline Camera makeCamera(const Vec3& pos, const Vec3& target, float fovYDegrees, float aspect) {
    Camera cam;
    cam.pos = pos;
    cam.forward = normalize(target - pos);
    const Vec3 worldUp(0.0f, 1.0f, 0.0f);
    cam.right = normalize(cross(cam.forward, worldUp));
    if (length(cam.right) < 1e-6f) {
        cam.right = Vec3(1.0f, 0.0f, 0.0f);
    }
    cam.up = normalize(cross(cam.right, cam.forward));
    cam.tanHalfFovY = std::tan(fovYDegrees * 0.5f * 3.14159265358979323846f / 180.0f);
    cam.aspect = aspect;
    return cam;
}

inline bool sameCamera(const Camera& a, const Camera& b) {
    constexpr float eps = 1e-6f;
    auto close = [](const Vec3& x, const Vec3& y) {
        return std::abs(x.x - y.x) <= eps && std::abs(x.y - y.y) <= eps &&
               std::abs(x.z - y.z) <= eps;
    };
    return close(a.pos, b.pos) && close(a.forward, b.forward) && close(a.right, b.right) &&
           close(a.up, b.up) && std::abs(a.tanHalfFovY - b.tanHalfFovY) <= eps &&
           std::abs(a.aspect - b.aspect) <= eps;
}

// 与 shaders/common.glsl 中 Globals 完全一致。
struct GpuGlobals {
    float camPos[4]{};
    float camFwd[4]{};
    float camRight[4]{};
    float camUp[4]{};
    float prevCamPos[4]{};
    float prevCamFwd[4]{};
    float prevCamRight[4]{};
    float prevCamUp[4]{};
    float extent[4]{};
    float glassMin[4]{};
    float glassMax[4]{};
    float glassAbsorption[4]{};
    uint32_t flags[4]{};
    uint32_t misc[4]{};
};
static_assert(sizeof(GpuGlobals) == 14 * 16, "Globals layout mismatch");

// 路径记录：128 字节，字段布局与 shaders/common.glsl Path 一致。
struct GpuPath {
    float origin[4]{};      // xyz, tmin
    float dir[4]{};         // xyz, coneAngle
    float throughput[4]{};  // rgb, prevBsdfPdf
    float prevPos[4]{};     // xyz, coneWidth
    float prevNormal[4]{};  // xyz, 保留
    uint32_t virtualT[4]{}; // 反射空间累计变换：映射位 + 平移 float 位
    uint32_t meta0[4]{};    // pixel, rngState, depth, packedFlags
    uint32_t meta1[4]{};    // slot, domain, chainHash, terminalSurfaceId
};
static_assert(sizeof(GpuPath) == 8 * 16, "Path layout mismatch");

// RT 命中结果：48 字节。
struct GpuHit {
    float hitPos[4]{};   // xyz, t
    float normal[4]{};   // xyz, hit
    uint32_t geom[4]{};  // instance, primitive, bary0 bits, bary1 bits
};
static_assert(sizeof(GpuHit) == 3 * 16, "Hit layout mismatch");

// shadow 任务：64 字节。
struct GpuShadowTask {
    float origin[4]{};  // xyz, tmax
    float dir[4]{};     // xyz, 保留
    float weight[4]{};  // rgb, 保留
    uint32_t meta[4]{}; // slotIndex, 保留...
};
static_assert(sizeof(GpuShadowTask) == 4 * 16, "ShadowTask layout mismatch");

// guide：64 字节。
struct GpuGuide {
    float posRough[4]{}; // xyz, roughness
    float nrm[4]{};      // xyz, hitDistance
    float albedo[4]{};   // rgb 线性 albedo, 保留
    uint32_t meta[4]{};  // chainHash, surfaceId, packedFlags, guideFrame
};
static_assert(sizeof(GpuGuide) == 4 * 16, "Guide layout mismatch");

// ReSTIR 主要素信息：64 字节。
struct GpuPrimaryInfo {
    float pos[4]{};     // xyz, 1=valid
    float nrm[4]{};     // xyz, viewDepth
    float albedo[4]{};  // 线性基础色
    float params[4]{};  // roughness, lobe, material, domain
};
static_assert(sizeof(GpuPrimaryInfo) == 4 * 16, "PrimaryInfo layout mismatch");

// reservoir：32 字节。
struct GpuReservoir {
    uint32_t selected[4]{}; // triangle, M, flags, 保留
    float weights[4]{};     // bary0, bary1, weightSum, targetLum
};
static_assert(sizeof(GpuReservoir) == 2 * 16, "Reservoir layout mismatch");

// 材质：64 字节。
struct GpuMaterial {
    float baseColor[4]{};   // 线性 rgb, w=type
    float params[4]{};      // metallic, roughness, ior, baseTexLayer
    float absorption[4]{};  // rgb, w=roughTexLayer
    float emission[4]{};    // rgb, w=pattern
};
static_assert(sizeof(GpuMaterial) == 4 * 16, "Material layout mismatch");

// 三角形元数据：80 字节。
struct GpuTriangle {
    float p0[4]{};
    float p1[4]{};
    float p2[4]{};
    float nrm[4]{};    // xyz, area
    uint32_t aux[4]{}; // material, group, 保留, 保留
};
static_assert(sizeof(GpuTriangle) == 5 * 16, "Triangle layout mismatch");

struct GpuInstanceGroup {
    uint32_t data[4]{}; // triOffset, material, isGlass, 保留
};
static_assert(sizeof(GpuInstanceGroup) == 16, "InstanceGroup layout mismatch");

struct GpuLightEntry {
    float cumulative = 0.0f;
    float area = 0.0f;
    float power = 0.0f;
    uint32_t triangle = 0;
};
static_assert(sizeof(GpuLightEntry) == 16, "LightEntry layout mismatch");

struct GpuCounts {
    uint32_t pathCount = 0;       // 当前活跃路径数
    uint32_t shadowCount = 0;     // 当前 shadow 任务数
    uint32_t nextPathCount = 0;   // shade 追加的下一队列路径数
    uint32_t nextShadowCount = 0; // shade 追加的 shadow 任务数
    uint32_t pathCapacity = 0;
    uint32_t shadowCapacity = 0;
    uint32_t overflowPath = 0;
    uint32_t overflowShadow = 0;
};
static_assert(sizeof(GpuCounts) == 32, "Counts layout mismatch");

// push constants：48 字节。
struct PushConstants {
    uint32_t slot = 0;       // 帧槽 0/1
    uint32_t queueCur = 0;   // 当前路径队列
    uint32_t queueNext = 1;  // 下一路径队列
    uint32_t count = 0;      // 平坦任务数
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t step = 0;       // à-trous 步长或 pass 标识
    uint32_t flags = 0;
    uint32_t frameIndex = 0;
    uint32_t pad0 = 0;
    uint32_t pad1 = 0;
    uint32_t pad2 = 0;
};
static_assert(sizeof(PushConstants) == 48, "PushConstants layout mismatch");

inline float luminance(const Vec3& c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; }

} // namespace rae
