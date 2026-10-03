#pragma once
#include <nlohmann/json.hpp>
#include <array>
#include <vector>
#include <map>
#include <string>
#include <fstream>
#include <filesystem>
#include <cmath>
#include <algorithm>
#include <stdexcept>
#include <cstdint>
#include <limits>
struct V3 { float x{},y{},z{};
 float& operator[](int i){return i==0?x:(i==1?y:z);
} float operator[](int i)const{return i==0?x:(i==1?y:z);
} };
inline V3 operator+(V3 a,V3 b){return {a.x+b.x,a.y+b.y,a.z+b.z};
}
inline V3 operator-(V3 a,V3 b){return {a.x-b.x,a.y-b.y,a.z-b.z};
}
inline V3 operator*(V3 a,float b){return {a.x*b,a.y*b,a.z*b};
}
inline float dot(V3 a,V3 b){return a.x*b.x+a.y*b.y+a.z*b.z;
}
inline V3 cross(V3 a,V3 b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};
}
inline float length(V3 a){return std::sqrt(dot(a,a));
}
inline V3 unit(V3 a){return a*(1/length(a));
}
struct F4{float x{},y{},z{},w{};
};
inline F4 f4(V3 a,float w=0){return {a.x,a.y,a.z,w};
}
struct U4{uint32_t x{},y{},z{},w{};
};
static_assert(sizeof(V3)==12 && sizeof(F4)==16 && sizeof(U4)==16);
struct MaterialGPU { F4 color, absorption, emission, params;
 };
struct Material { std::string id,pattern;
 MaterialGPU gpu;
 uint32_t seed;
 float cv,rv;
 };
struct Triangle { F4 a,b,c,n;
 U4 info;
 };
struct Light { uint32_t triangle;
 float area,pdf,cdf;
 };
struct Scene {
 std::vector<Material> materials;
 std::vector<Triangle> triangles;
 std::vector<V3> vertices;
 std::vector<Light> lights;
 uint32_t textureSize{};
 std::vector<std::vector<uint8_t>> colors,roughness;
 std::map<std::string,int> ids;
 static void need(bool ok,const std::string& why){if(!ok)throw std::runtime_error("materials.json: "+why);
 }
 void load(const std::filesystem::path& file){
  std::ifstream in(file);
   need(bool(in),"无法读取配置");
  nlohmann::json root;
   in>>root;
  need(root.at("schemaVersion").is_number_integer()&&root.at("schemaVersion")==1,"schemaVersion 必须为 1");
  const auto& ts=root.at("textureSize");
  need(ts.is_number_integer(),"textureSize 类型错误");
  need(ts.get<double>()>=16&&ts.get<double>()<=1024,"textureSize 范围错误");
  int size=ts.get<int>();
  need((size&(size-1))==0,"textureSize 必须为二次幂");
  textureSize=uint32_t(size);
  need(root.at("materials").is_array(),"materials 必须为数组");
  auto scalar=[](const nlohmann::json& j,const char* key,float lo,float hi){
   const auto& v=j.at(key);
   need(v.is_number(),std::string(key)+" 类型错误");
   float f=v.get<float>();
   need(std::isfinite(f)&&f>=lo&&f<=hi,std::string(key)+" 范围错误");
   return f;
  };
  auto vector=[&](const nlohmann::json& j,const char* key,float hi){
   const auto& a=j.at(key);
   need(a.is_array()&&a.size()==3,std::string(key)+" 必须为三分量");
   V3 v;
   for(int k=0;k<3;++k){need(a[k].is_number(),std::string(key)+" 类型错误");
   v[k]=a[k].get<float>();
   need(std::isfinite(v[k])&&v[k]>=0&&v[k]<=hi,std::string(key)+" 范围错误");
   }return v;
  };
  for(const auto& j:root.at("materials")){
   Material m{};
   need(j.at("id").is_string(),"id 类型错误");
   m.id=j.at("id").get<std::string>();
   need(!m.id.empty()&&!ids.contains(m.id),"id 空或重复");
   const std::string type=j.at("type").get<std::string>();
   const std::array<std::string,5> types={"diffuse","rough_metal","mirror","dielectric","emissive"};
   auto it=std::find(types.begin(),types.end(),type);
   need(it!=types.end(),"未知 type");
   int t=int(it-types.begin());
   m.gpu.color=f4(vector(j,"baseColorSRGB",1));
   m.gpu.absorption=f4(vector(j,"absorption",std::numeric_limits<float>::max()));
   m.gpu.emission=f4(vector(j,"emission",std::numeric_limits<float>::max()));
   float metallic=scalar(j,"metallic",0,1),r=scalar(j,"roughness",0,1),ior=scalar(j,"ior",1,3);
   m.gpu.params={float(t),r,ior,metallic};
   need(t==1||t==2?metallic==1:metallic==0,"metallic 与 type 不兼容");
   need(t!=1||r>0,"rough_metal 的 roughness 必须为正");
   need((t!=2&&t!=3)||r==0,"delta 材质 roughness 必须为零");
   need(t==3?ior>1:ior==1,"ior 与 type 不兼容");
   need(t==3||dot({m.gpu.absorption.x,m.gpu.absorption.y,m.gpu.absorption.z},{1,1,1})==0,"非玻璃 absorption 必须为零");
   need(t==4||dot({m.gpu.emission.x,m.gpu.emission.y,m.gpu.emission.z},{1,1,1})==0,"非发光 emission 必须为零");
   m.pattern=j.at("pattern").get<std::string>();
   need(m.pattern=="solid"||m.pattern=="stone_noise"||m.pattern=="wood_grain","未知 pattern");
   const auto& seed=j.at("textureSeed");
   need(seed.is_number_integer()&&seed.get<double>()>=0&&seed.get<double>()<=4294967295.0,"textureSeed 范围错误");
   m.seed=seed.get<uint32_t>();
   m.cv=scalar(j,"baseColorVariation",0,1);
   m.rv=scalar(j,"roughnessVariation",0,1);
   need(t<2||(m.pattern=="solid"&&m.cv==0&&m.rv==0),"delta/emissive 必须为 solid 且 variation=0");
   ids[m.id]=int(materials.size());
   materials.push_back(m);
  }
  for(const auto* id:{"stone","wood","red","blue","rough_metal","mirror","glass","glow"})need(ids.contains(id),std::string("场景缺少 ")+id);
 }
 static uint32_t hash(uint32_t x){x^=x>>16;
 x*=0x7feb352du;
 x^=x>>15;
 x*=0x846ca68bu;
 return x^(x>>16);
 }
 static float noise(float x,float y,uint32_t seed){
  int ix=int(std::floor(x)),iy=int(std::floor(y));
  float fx=x-float(ix),fy=y-float(iy);
  fx=fx*fx*(3-2*fx);
  fy=fy*fy*(3-2*fy);
  auto h=[seed](int a,int b){return float(hash(uint32_t(a)*73856093u^uint32_t(b)*19349663u^seed)&0xffffffu)/16777215.0f;
  };
  return std::lerp(std::lerp(h(ix,iy),h(ix+1,iy),fx),std::lerp(h(ix,iy+1),h(ix+1,iy+1),fx),fy);
 }
 static float decode(float s){return s<=0.04045f?s/12.92f:std::pow((s+0.055f)/1.055f,2.4f);
 }
 static float encode(float l){return l<=0.0031308f?12.92f*l:1.055f*std::pow(l,1/2.4f)-0.055f;
 }
 void textures(){
  colors.resize(materials.size());
  roughness.resize(materials.size());
  for(size_t k=0;k<materials.size();++k){
   const auto& m=materials[k];
   uint32_t sz=textureSize;
   std::vector<float> rgb(size_t(sz)*sz*3),rr(size_t(sz)*sz);
   for(uint32_t y=0;y<sz;++y)for(uint32_t x=0;x<sz;++x){
    float u=float(x)/float(sz),v=float(y)/float(sz),q=0;
    if(m.pattern=="stone_noise")q=(noise(u*4,v*4,m.seed)*0.5f+noise(u*16,v*16,m.seed+1)*0.3f+noise(u*64,v*64,m.seed+2)*0.2f)*2-1;
    if(m.pattern=="wood_grain")q=std::sin((v*16+noise(u*3,v*3,m.seed)*2)*6.2831853f);
    size_t p=size_t(y)*sz+x;
    const std::array<float,3> base={m.gpu.color.x,m.gpu.color.y,m.gpu.color.z};
    for(size_t c=0;c<3;++c)rgb[p*3+c]=decode(std::clamp(base[c]+q*m.cv,0.0f,1.0f));
    rr[p]=std::clamp(m.gpu.params.y+q*m.rv,m.gpu.params.x==1?1.0f/255:0.0f,1.0f);
   }
   while(true){
    for(size_t p=0;p<size_t(sz)*sz;++p){for(size_t c=0;c<3;++c)colors[k].push_back(uint8_t(std::lround(std::clamp(encode(rgb[p*3+c]),0.0f,1.0f)*255)));
    colors[k].push_back(255);
    roughness[k].push_back(uint8_t(std::lround(rr[p]*255)));
    }
    if(sz==1)break;
    uint32_t ns=sz/2;
    std::vector<float> nr(size_t(ns)*ns*3),ng(size_t(ns)*ns);
    for(uint32_t y=0;y<ns;++y)for(uint32_t x=0;x<ns;++x)for(uint32_t dy=0;dy<2;++dy)for(uint32_t dx=0;dx<2;++dx){
     size_t a=size_t(y*2+dy)*sz+x*2+dx,b=size_t(y)*ns+x;
     ng[b]+=rr[a]*0.25f;
     for(size_t c=0;c<3;++c)nr[b*3+c]+=rgb[a*3+c]*0.25f;
    }rgb=std::move(nr);
    rr=std::move(ng);
    sz=ns;
   }
  }
 }
 void mesh(){
  std::map<std::array<int,3>,int> grid;
  auto fill=[&](int x0,int x1,int y0,int y1,int z0,int z1,const char* id){for(int z=z0;z<=z1;++z)for(int y=y0;y<=y1;++y)for(int x=x0;x<=x1;++x)grid[{x,y,z}]=ids.at(id);
  };
  fill(0,15,-1,-1,0,15,"stone");
  fill(0,0,0,5,0,15,"red");
  fill(15,15,0,5,0,15,"blue");
  fill(1,14,0,5,0,0,"stone");
  fill(0,15,6,6,0,15,"stone");
  for(int z=6;z<=9;++z)for(int x=6;x<=9;++x)grid.erase({x,6,z});
  fill(5,10,1,4,1,1,"mirror");
  fill(1,1,1,4,4,7,"mirror");
  fill(3,3,0,2,5,5,"red");
  fill(12,12,0,2,6,6,"blue");
  fill(4,4,0,1,10,10,"wood");
  fill(5,5,0,0,10,10,"wood");
  fill(10,10,0,1,10,10,"rough_metal");
  for(auto p:std::array<std::array<int,3>,4>{{{4,5,4},{11,5,4},{4,5,11},{11,5,11}}})grid[p]=ids.at("glow");
  using Key=std::array<int,4>;
  std::map<Key,std::map<std::array<int,2>,int>> planes;
  for(auto [p,mat]:grid)for(int axis=0;axis<3;++axis)for(int s:{-1,1}){
   auto q=p;
   q[axis]+=s;
   if(grid.contains(q))continue;
   int u=(axis+1)%3,v=(axis+2)%3;
   planes[{axis,s,p[axis]+(s==1?1:0),mat}][{p[u],p[v]}]=mat;
  }
  uint32_t surface=1;
  auto rect=[&](int axis,int sign,float plane,float u0,float v0,float uw,float vh,int mat){
   int u=(axis+1)%3,v=(axis+2)%3;
   V3 a{};
   a[axis]=plane;
   a[u]=u0;
   a[v]=v0;
   V3 du{},dv{},n{};
   du[u]=uw;
   dv[v]=vh;
   n[axis]=float(sign);
   V3 b=a+du,c=a+du+dv,d=a+dv;
   if(sign<0)std::swap(b,d);
   for(auto vv:std::array<std::array<V3,3>,2>{{{a,b,c},{a,c,d}}}){
    triangles.push_back({f4(vv[0]),f4(vv[1]),f4(vv[2]),f4(n),{uint32_t(mat),surface,uint32_t(axis),0}});
    vertices.insert(vertices.end(),vv.begin(),vv.end());
   }++surface;
  };
  for(auto& [key,cells]:planes)while(!cells.empty()){
   auto p=cells.begin()->first;
   int w=1,h=1;
   while(cells.contains({p[0]+w,p[1]}))++w;
   while(true){bool full=true;
   for(int x=0;x<w;++x)if(!cells.contains({p[0]+x,p[1]+h}))full=false;
   if(!full)break;
   ++h;
   }
   for(int y=0;y<h;++y)for(int x=0;x<w;++x)cells.erase({p[0]+x,p[1]+y});
   rect(key[0],key[1],float(key[2]),float(p[0]),float(p[1]),float(w),float(h),key[3]);
  }
  // 玻璃按实际平移后的闭合边界单独提取，不能用格子邻接删除其底面。
  V3 lo{7,0.125f,7},hi{9,2.125f,9};
  for(int axis=0;axis<3;++axis)for(int s:{-1,1})rect(axis,s,s<0?lo[axis]:hi[axis],lo[(axis+1)%3],lo[(axis+2)%3],2,2,ids.at("glass"));
  float total=0;
  for(uint32_t i=0;i<triangles.size();++i){auto& t=triangles[i];
  const auto& m=materials[t.info.x].gpu;
  if(m.params.x!=4)continue;
   V3 a{t.a.x,t.a.y,t.a.z},b{t.b.x,t.b.y,t.b.z},c{t.c.x,t.c.y,t.c.z};
   float area=length(cross(b-a,c-a))*0.5f,power=area*(m.emission.x*0.2126f+m.emission.y*0.7152f+m.emission.z*0.0722f);
   if(power>0){total+=power;
   lights.push_back({i,area,power,total});
   t.info.w=uint32_t(lights.size());
   }
  }
  for(auto& l:lights){l.pdf/=total;
  l.cdf/=total;
  }
 }
};
