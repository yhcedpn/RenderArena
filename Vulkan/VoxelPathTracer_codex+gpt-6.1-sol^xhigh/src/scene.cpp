#include "scene.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <limits>

namespace arena {
V3 normalize(V3 a){float d=std::sqrt(dot(a,a));return a*(1.0f/d);}
namespace {
using J=nlohmann::json;
float scalar(const J& j,const char* key,float lo,float hi){
 const auto& v=j.at(key); if(!v.is_number())throw std::runtime_error(std::string(key)+": 必须为数值");
 double d=v.get<double>();if(!std::isfinite(d)||d<lo||d>hi)throw std::runtime_error(std::string(key)+": 数值越界");return static_cast<float>(d);
}
V3 vector3(const J& j,const char* key,float hi){const auto& a=j.at(key);if(!a.is_array()||a.size()!=3)throw std::runtime_error(std::string(key)+": 需要三个分量");V3 r;float* p=&r.x;for(size_t i=0;i<3;++i){if(!a[i].is_number())throw std::runtime_error(std::string(key)+": 分量类型错误");double v=a[i].get<double>();if(!std::isfinite(v)||v<0||v>hi)throw std::runtime_error(std::string(key)+": 分量越界");p[i]=static_cast<float>(v);}return r;}
uint32_t integer(const J& j,const char* key,uint32_t lo,uint32_t hi){const auto& v=j.at(key);if(!v.is_number_integer()||v.get<double>()<lo||v.get<double>()>hi)throw std::runtime_error(std::string(key)+": 整数越界");return v.get<uint32_t>();}
bool zero(V3 a){return a.x==0&&a.y==0&&a.z==0;}
uint32_t hash(uint32_t x){x^=x>>16;x*=0x7feb352du;x^=x>>15;x*=0x846ca68bu;return x^(x>>16);}
float noise(float x,float y,uint32_t seed){int ix=static_cast<int>(std::floor(x)),iy=static_cast<int>(std::floor(y));float u=x-std::floor(x),v=y-std::floor(y);u=u*u*(3-2*u);v=v*v*(3-2*v);auto n=[&](int a,int b){return static_cast<float>(hash(static_cast<uint32_t>(a)*0x9e3779b9u^static_cast<uint32_t>(b)*0x85ebca6bu^seed)>>8)*(1.0f/16777216.0f);};return std::lerp(std::lerp(n(ix,iy),n(ix+1,iy),u),std::lerp(n(ix,iy+1),n(ix+1,iy+1),u),v);}
float linear(float s){return s<=0.04045f?s/12.92f:std::pow((s+0.055f)/1.055f,2.4f);}
float srgb(float s){return s<=0.0031308f?12.92f*s:1.055f*std::pow(s,1.0f/2.4f)-0.055f;}
uint8_t quant(float x){return static_cast<uint8_t>(std::lround(std::clamp(x,0.0f,1.0f)*255));}
F4 f4(V3 p){return{p.x,p.y,p.z,0};}
}
Scene::Scene(const std::filesystem::path& path){
 std::ifstream input(path);if(!input)throw std::runtime_error("无法读取材质文件: "+path.string());J root;input>>root;
 if(integer(root,"schemaVersion",1,1)!=1)throw std::runtime_error("不支持的 schemaVersion");
 textureSize=integer(root,"textureSize",16,1024);if((textureSize&(textureSize-1))!=0)throw std::runtime_error("textureSize 必须为二的幂");
 const auto& list=root.at("materials");if(!list.is_array()||list.empty())throw std::runtime_error("materials 必须为非空数组");std::map<std::string,uint32_t> ids;
 for(const auto& j:list){Material m;m.id=j.at("id").get<std::string>();m.type=j.at("type").get<std::string>();m.pattern=j.at("pattern").get<std::string>();
  if(m.id.empty()||ids.contains(m.id))throw std::runtime_error("材质 ID 为空或重复: "+m.id);
  const std::array<std::string,5> types={"diffuse","rough_metal","mirror","dielectric","emissive"};auto it=std::find(types.begin(),types.end(),m.type);if(it==types.end())throw std::runtime_error("未知材质类型: "+m.type);
  if(m.pattern!="solid"&&m.pattern!="stone_noise"&&m.pattern!="wood_grain")throw std::runtime_error("未知 pattern");
  m.color=vector3(j,"baseColorSRGB",1);m.absorption=vector3(j,"absorption",std::numeric_limits<float>::max());m.emission=vector3(j,"emission",std::numeric_limits<float>::max());
  m.metallic=scalar(j,"metallic",0,1);m.roughness=scalar(j,"roughness",0,1);m.ior=scalar(j,"ior",1,3);m.colorVariation=scalar(j,"baseColorVariation",0,1);m.roughVariation=scalar(j,"roughnessVariation",0,1);m.seed=integer(j,"textureSeed",0,UINT32_MAX);
  if((m.type=="diffuse"&&m.metallic!=0)||(m.type=="rough_metal"&&(m.metallic!=1||m.roughness<=0))||(m.type=="mirror"&&(m.metallic!=1||m.roughness!=0))||(m.type=="dielectric"&&(m.metallic!=0||m.roughness!=0||m.ior<=1))||(m.type!="dielectric"&&(m.ior!=1||!zero(m.absorption)))||(m.type!="emissive"&&!zero(m.emission))||((m.type=="mirror"||m.type=="dielectric"||m.type=="emissive")&&(m.pattern!="solid"||m.colorVariation!=0||m.roughVariation!=0)))throw std::runtime_error("材质模型约束不满足: "+m.id);
  uint32_t type=static_cast<uint32_t>(it-types.begin());ids[m.id]=static_cast<uint32_t>(materials.size());materials.push_back(m);gpuMaterials.push_back({{m.absorption.x,m.absorption.y,m.absorption.z,m.ior},{m.emission.x,m.emission.y,m.emission.z,static_cast<float>(type)}});
 }
 auto id=[&](const char* s)->int{auto it=ids.find(s);if(it==ids.end())throw std::runtime_error(std::string("场景材质无法解析: ")+s);return static_cast<int>(it->second);};
 int stone=id("stone"),red=id("red"),blue=id("blue"),mirror=id("mirror"),wood=id("wood"),metal=id("rough_metal"),glass=id("glass"),glow=id("glow");glassMaterial=static_cast<uint32_t>(glass);
 // 稀疏整数格只参与不透明邻接；抬升后的玻璃独立提取。
 std::map<std::array<int,3>,int> cells;auto fill=[&](int x0,int x1,int y0,int y1,int z0,int z1,int mat){for(int z=z0;z<=z1;++z)for(int y=y0;y<=y1;++y)for(int x=x0;x<=x1;++x)cells[{x,y,z}]=mat;};
 fill(0,15,-1,-1,0,15,stone);fill(0,0,0,5,0,15,red);fill(15,15,0,5,0,15,blue);fill(1,14,0,5,0,0,stone);
 for(int z=0;z<16;++z)for(int x=0;x<16;++x)if(!(x>=6&&x<=9&&z>=6&&z<=9))cells[{x,6,z}]=stone;
 fill(5,10,1,4,1,1,mirror);fill(1,1,1,4,4,7,mirror);fill(3,3,0,2,5,5,red);fill(12,12,0,2,6,6,blue);fill(4,4,0,1,10,10,wood);fill(5,5,0,0,10,10,wood);fill(10,10,0,1,10,10,metal);
 for(auto x:{4,11})for(auto z:{4,11})cells[{x,5,z}]=glow;
 // 每个方向/平面建立掩码；先向 U，再向 V 扩展，确保当前矩形不能整边扩大。
 using Mask=std::map<std::array<int,2>,int>;std::map<std::array<int,3>,Mask> planes;
 for(const auto& [p,mat]:cells)for(int axis=0;axis<3;++axis)for(int sign:{-1,1}){auto q=p;q[axis]+=sign;if(cells.contains(q))continue;int u=(axis+1)%3,v=(axis+2)%3;planes[{axis,sign,p[axis]+(sign>0?1:0)}][{p[u],p[v]}]=mat;++exposedUnitFaces;}
 std::vector<std::vector<Triangle>> byMaterial(materials.size());uint32_t surface=1;
 auto rectangle=[&](int axis,int sign,float plane,float u0,float v0,float du,float dv,int mat){int u=(axis+1)%3,v=(axis+2)%3;V3 p[4];float a[3]{};a[axis]=plane;a[u]=u0;a[v]=v0;p[0]={a[0],a[1],a[2]};a[u]+=du;p[1]={a[0],a[1],a[2]};a[v]+=dv;p[2]={a[0],a[1],a[2]};a[u]-=du;p[3]={a[0],a[1],a[2]};V3 n{};(&n.x)[axis]=static_cast<float>(sign);if(sign<0)std::swap(p[1],p[3]);for(auto idx:std::array<std::array<int,3>,2>{{{0,1,2},{0,2,3}}})byMaterial[static_cast<size_t>(mat)].push_back({f4(p[idx[0]]),f4(p[idx[1]]),f4(p[idx[2]]),f4(n),{static_cast<uint32_t>(mat),surface,UINT32_MAX,0}});++surface;++rectangles;};
 for(auto& [key,mask]:planes)while(!mask.empty()){auto first=mask.begin();auto uv=first->first;int mat=first->second,w=1,h=1;auto match=[&](int u,int v){auto i=mask.find({u,v});return i!=mask.end()&&i->second==mat;};while(match(uv[0]+w,uv[1]))++w;for(;;){bool ok=true;for(int i=0;i<w;++i)if(!match(uv[0]+i,uv[1]+h)){ok=false;break;}if(!ok)break;++h;}rectangle(key[0],key[1],static_cast<float>(key[2]),static_cast<float>(uv[0]),static_cast<float>(uv[1]),static_cast<float>(w),static_cast<float>(h),mat);for(int j=0;j<h;++j)for(int i=0;i<w;++i)mask.erase({uv[0]+i,uv[1]+j});}
 const float low[3]={7,0.125f,7},high[3]={9,2.125f,9};for(int axis=0;axis<3;++axis)for(int sign:{-1,1}){int u=(axis+1)%3,v=(axis+2)%3;rectangle(axis,sign,sign>0?high[axis]:low[axis],low[u],low[v],high[u]-low[u],high[v]-low[v],glass);}
 for(const auto& group:byMaterial){groups.push_back({static_cast<uint32_t>(triangles.size()),static_cast<uint32_t>(group.size())});triangles.insert(triangles.end(),group.begin(),group.end());}
 double power=0;for(uint32_t i=0;i<triangles.size();++i){auto& t=triangles[i];for(auto p:{t.a,t.b,t.c})vertices.push_back({p.x,p.y,p.z});auto& m=materials[t.id.x];if(m.type!="emissive")continue;V3 a{t.a.x,t.a.y,t.a.z},b{t.b.x,t.b.y,t.b.z},c{t.c.x,t.c.y,t.c.z};V3 cr=cross(b-a,c-a);float area=std::sqrt(dot(cr,cr))*0.5f;double w=area*(0.2126*m.emission.x+0.7152*m.emission.y+0.0722*m.emission.z);if(w<=0)continue;t.id.z=static_cast<uint32_t>(lights.size());power+=w;lights.push_back({i,static_cast<float>(w),static_cast<float>(power),area});}
 for(auto& l:lights){l.pdf=static_cast<float>(l.pdf/power/l.area);l.cdf=static_cast<float>(l.cdf/power);}if(!lights.empty())lights.back().cdf=1;
 for(const auto& m:materials){std::vector<TextureLevel> levels;TextureLevel top{textureSize,{}, {}};top.color.resize(size_t(textureSize)*textureSize*4);top.rough.resize(size_t(textureSize)*textureSize);
  for(uint32_t y=0;y<textureSize;++y)for(uint32_t x=0;x<textureSize;++x){float u=(static_cast<float>(x)+0.5f)/static_cast<float>(textureSize),v=(static_cast<float>(y)+0.5f)/static_cast<float>(textureSize),n=0;
   if(m.pattern=="stone_noise")n=2*(0.57f*noise(u*4,v*4,m.seed)+0.29f*noise(u*11,v*11,m.seed+1)+0.14f*noise(u*29,v*29,m.seed+2))-1;
   if(m.pattern=="wood_grain")n=std::sin((v*16+2*noise(u*3,v*3,m.seed))*6.283185307f); // 条纹沿 U 延伸。
   size_t p=size_t(y)*textureSize+x;top.color[p*4]=quant(m.color.x+n*m.colorVariation);top.color[p*4+1]=quant(m.color.y+n*m.colorVariation);top.color[p*4+2]=quant(m.color.z+n*m.colorVariation);top.color[p*4+3]=255;float r=std::clamp(m.roughness+n*m.roughVariation,0.0f,1.0f);if(m.type=="rough_metal")r=std::max(r,1.0f/255);top.rough[p]=quant(r);
  }levels.push_back(std::move(top));while(levels.back().size>1){const auto& a=levels.back();uint32_t s=a.size/2;TextureLevel b{s,std::vector<uint8_t>(size_t(s)*s*4),std::vector<uint8_t>(size_t(s)*s)};for(uint32_t y=0;y<s;++y)for(uint32_t x=0;x<s;++x){float c[3]{},r=0;for(uint32_t j=0;j<2;++j)for(uint32_t i=0;i<2;++i){size_t p=size_t(y*2+j)*a.size+x*2+i;for(size_t k=0;k<3;++k)c[k]+=linear(a.color[p*4+k]/255.0f)*0.25f;r+=a.rough[p]/255.0f*0.25f;}size_t p=size_t(y)*s+x;for(size_t k=0;k<3;++k)b.color[p*4+k]=quant(srgb(c[k]));b.color[p*4+3]=255;b.rough[p]=quant(r);}levels.push_back(std::move(b));}textures.push_back(std::move(levels));
 }
 std::cout<<"Scene: exposed unit faces="<<exposedUnitFaces<<", greedy rectangles="<<rectangles<<", triangles="<<triangles.size()<<", light triangles="<<lights.size()<<", texture layers="<<materials.size()<<"\n";
}
}

