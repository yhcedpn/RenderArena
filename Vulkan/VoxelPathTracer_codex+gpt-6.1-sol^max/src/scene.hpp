#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

struct V3 {
    float x{},y{},z{};
    V3 operator+(V3 b) const { return {x+b.x,y+b.y,z+b.z}; }
    V3 operator-(V3 b) const { return {x-b.x,y-b.y,z-b.z}; }
    V3 operator*(float b) const { return {x*b,y*b,z*b}; }
};
inline float dot(V3 a,V3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
inline V3 cross(V3 a,V3 b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
inline V3 unit(V3 a){return a*(1.0f/std::sqrt(dot(a,a)));}
struct F4 { float x{},y{},z{},w{}; };
struct U4 { uint32_t x{},y{},z{},w{}; };
inline F4 f4(V3 a,float w=0){return {a.x,a.y,a.z,w};}
struct MaterialGPU {F4 baseMetal, emissionType, absorptionIor, misc;};
struct Material { std::string id,pattern; MaterialGPU gpu; uint32_t seed; float colorVariation,roughVariation; };
struct Triangle {F4 p0,p1,p2,normal,uv01,uv2;};
struct Light { U4 ids; F4 distribution; };
struct MeshGroup {uint32_t first,count;};
struct TextureLevel {uint32_t size; std::vector<uint8_t> color,roughness;};
struct Scene {
    uint32_t textureSize{}, mipCount{}, rectangles{};
    std::vector<Material> materials;
    std::vector<MaterialGPU> gpuMaterials;
    std::vector<Triangle> triangles;
    std::vector<MeshGroup> groups;
    std::vector<Light> lights;
    std::vector<TextureLevel> textures;
    void load(const std::filesystem::path&);
};
struct Camera {
    V3 position{8,2.5f,20}; float yaw=0,pitch=std::atan2(-0.5f,14.0f);
    V3 forward() const {return {std::sin(yaw)*std::cos(pitch),std::sin(pitch),-std::cos(yaw)*std::cos(pitch)};}
    V3 right() const {return unit(cross(forward(),{0,1,0}));}
    V3 up() const {return cross(right(),forward());}
};
struct FrameData {F4 eye,right,up,forward,oldEye,oldRight,oldUp,oldForward,jitter; U4 extent; U4 control; F4 limits;};
// 与 shared.glsl 的 vec4 布局一致。路径最多两条，输出为拆分前、反射、透射三个 slot。
constexpr uint32_t PathBytes=160,HitBytes=64,GuideBytes=144,ReservoirBytes=32,HistoryBytes=32,ShadowBytes=64;
