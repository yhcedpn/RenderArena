#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace arena {
struct V3 { float x{},y{},z{}; V3 operator+(V3 b) const{return{x+b.x,y+b.y,z+b.z};} V3 operator-(V3 b) const{return{x-b.x,y-b.y,z-b.z};} V3 operator*(float s)const{return{x*s,y*s,z*s};} };
inline float dot(V3 a,V3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
inline V3 cross(V3 a,V3 b){return{a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
V3 normalize(V3 a);
struct F4 { float x{},y{},z{},w{}; };
struct U4 { uint32_t x{},y{},z{},w{}; };
struct GpuMaterial { F4 absorptionIor,emissionType; };
struct Triangle { F4 a,b,c,normal; U4 id; };
struct Light { uint32_t triangle; float pdf,cdf,area; };
struct Group { uint32_t first,count; };
struct TextureLevel { uint32_t size; std::vector<uint8_t> color,rough; };
struct Material {
 std::string id,type,pattern; V3 color,absorption,emission; float metallic,roughness,ior,colorVariation,roughVariation; uint32_t seed;
};
struct Scene {
 uint32_t textureSize{}; std::vector<Material> materials; std::vector<GpuMaterial> gpuMaterials;
 std::vector<Triangle> triangles; std::vector<V3> vertices; std::vector<Group> groups; std::vector<Light> lights;
 std::vector<std::vector<TextureLevel>> textures;
 uint32_t glassMaterial{}; uint32_t exposedUnitFaces{},rectangles{};
 explicit Scene(const std::filesystem::path& path);
};
}
