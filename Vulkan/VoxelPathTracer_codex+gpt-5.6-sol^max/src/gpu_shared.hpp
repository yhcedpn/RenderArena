#pragma once

#include "math.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace arena {

constexpr std::uint32_t kMaxScatterEvents = 8;
constexpr std::uint32_t kRestirNewCandidates = 4;
constexpr std::uint32_t kRestirSpatialNeighbors = 4;
constexpr std::uint32_t kSvgfDomainCount = 6;
constexpr std::uint32_t kInvalidIndex = std::numeric_limits<std::uint32_t>::max();

enum class MaterialType : std::uint32_t {
    diffuse,
    roughMetal,
    mirror,
    dielectric,
    emissive,
};

enum class SignalDomain : std::uint32_t {
    unclassified = 0,
    analytic = 1,
    ordinaryDiffuse = 2,
    ordinarySpecular = 3,
    reflectionDiffuse = 4,
    reflectionSpecular = 5,
    glassReflection = 6,
    glassTransmission = 7,
};

enum DescriptorBinding : std::uint32_t {
    bindingTlas = 0,
    bindingVertices,
    bindingIndices,
    bindingTriangles,
    bindingMaterials,
    bindingLights,
    bindingBaseColorTexture,
    bindingRoughnessTexture,
    bindingTextureSampler,
    bindingPathQueueA,
    bindingPathQueueB,
    bindingHitStates,
    bindingCounters,
    bindingIndirectArgs,
    bindingShadowTasks,
    bindingShadowResults,
    bindingBranchRecords,
    bindingReservoirInitial,
    bindingReservoirTemporal,
    bindingReservoirHistory,
    bindingSvgfHistory,
    bindingAtrousA,
    bindingAtrousB,
    bindingCamera,
    bindingCount,
};

struct alignas(16) UVec4 {
    std::uint32_t x{};
    std::uint32_t y{};
    std::uint32_t z{};
    std::uint32_t w{};
};

struct alignas(16) VertexGpu {
    Vec4 positionU{};
    Vec4 normalV{};
    UVec4 metadata{};
};

struct alignas(16) TriangleGpu {
    UVec4 metadata{}; // surface id, material id, light id, BLAS group id
    Vec4 geometry{};  // triangle area, light area proposal PDF, unused, unused
};

struct alignas(16) MaterialGpu {
    Vec4 baseColorMetallic{};
    Vec4 emissionRoughness{};
    Vec4 absorptionIor{};
    UVec4 metadata{}; // type, texture layer, texture size, mip count
};

struct alignas(16) LightGpu {
    Vec4 v0Area{};
    Vec4 edge1SelectionPdf{};
    Vec4 edge2AreaPdf{};
    Vec4 normalCdf{};
    UVec4 metadata{}; // triangle id, material id, unused, unused
};

// std430-compatible. The three reflection rows encode the affine transform
// R0*R1*...*Rn, so applying it to a terminal point performs the required
// reverse traversal of the encountered mirror planes.
struct alignas(16) PathStateGpu {
    Vec4 originConeWidth{};
    Vec4 directionConeSpread{};
    Vec4 throughputLastPdf{};
    UVec4 identity{}; // pixel, branch, completed scatters, flags
    UVec4 optical{};  // chain hash, chain length, signal domain, reserved
    Vec4 reflectionRow0{};
    Vec4 reflectionRow1{};
    Vec4 reflectionRow2{};
};

struct alignas(16) HitStateGpu {
    Vec4 distanceBarycentric{}; // t, bary x, bary y, hit flag
    UVec4 identity{};           // global triangle, surface, material, front face
};

struct alignas(16) GuideGpu {
    Vec4 positionDistance{};
    Vec4 normalRoughness{};
    Vec4 albedoValidity{};
    UVec4 identity{}; // surface, material, optical hash, terminal/domain bits
};

struct alignas(16) BranchRecordGpu {
    Vec4 radiance{};
    GuideGpu guide{};
    UVec4 metadata{}; // domain, valid, optical flags, reserved
};

struct alignas(16) ReservoirGpu {
    Vec4 sample{};       // bary x, bary y, selected target, normalization W
    Vec4 statistics{};   // weight sum, z estimate, M, valid
    UVec4 identity{};    // light id, source pixel, surface id, material id
};

struct alignas(16) ShadowTaskGpu {
    Vec4 originTMax{};
    Vec4 direction{};
    Vec4 contribution{};
    UVec4 identity{}; // branch record, domain, kind, reserved
};

struct alignas(16) SvgfHistoryGpu {
    Vec4 radianceVariance{};
    Vec4 momentsLength{};
};

struct alignas(16) CameraGpu {
    Vec4 currentPositionTanHalfFov{};
    Vec4 currentForwardAspect{};
    Vec4 currentRight{};
    Vec4 currentUp{};
    Vec4 previousPositionTanHalfFov{};
    Vec4 previousForwardAspect{};
    Vec4 previousRight{};
    Vec4 previousUp{};
    Vec4 jitterPixels{}; // current xy, previous xy
    UVec4 frame{};       // width, height, rendered frame index, flags
};

struct alignas(16) PushConstantsGpu {
    UVec4 params{};
    UVec4 counts{};
    UVec4 extra{};
};

static_assert(sizeof(VertexGpu) == 48);
static_assert(sizeof(PathStateGpu) == 128);
static_assert(sizeof(GuideGpu) == 64);
static_assert(sizeof(BranchRecordGpu) == 96);
static_assert(sizeof(ReservoirGpu) == 48);
static_assert(sizeof(CameraGpu) == 160);

} // namespace arena
