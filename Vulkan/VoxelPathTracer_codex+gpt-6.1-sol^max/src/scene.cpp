#include "scene.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <fstream>
#include <map>
#include <stdexcept>

namespace {
using J=nlohmann::json;
void demand(bool b,const std::string& s){if(!b)throw std::runtime_error("materials.json: "+s);}
float number(const J& j,const char* key,float low,float high){
    const auto& v=j.at(key); demand(v.is_number(),std::string(key)+" 必须为数值");
    float x=v.get<float>(); demand(std::isfinite(x)&&x>=low&&x<=high,std::string(key)+" 超出范围"); return x;
}
V3 triple(const J& j,const char* key,float high){
    const auto& v=j.at(key);demand(v.is_array()&&v.size()==3,std::string(key)+" 必须是三个数");
    V3 r;float* out=&r.x;
    for(size_t i=0;i<3;++i){demand(v[i].is_number(),std::string(key)+" 分量必须为数值");out[i]=v[i].get<float>();demand(std::isfinite(out[i])&&out[i]>=0&&out[i]<=high,std::string(key)+" 分量超出范围");}return r;
}
uint32_t hash(uint32_t x){x^=x>>16;x*=0x7feb352du;x^=x>>15;x*=0x846ca68bu;return x^(x>>16);}
float noise(float x,float y,uint32_t seed){
    int ix=int(std::floor(x)),iy=int(std::floor(y));float u=x-float(ix),v=y-float(iy);u=u*u*(3-2*u);v=v*v*(3-2*v);
    auto h=[&](int a,int b){return float(hash(uint32_t(a)*73856093u^uint32_t(b)*19349663u^seed)&0xffffffu)/16777215.0f;};
    return std::lerp(std::lerp(h(ix,iy),h(ix+1,iy),u),std::lerp(h(ix,iy+1),h(ix+1,iy+1),u),v);
}
float linear(float x){return x<=0.04045f?x/12.92f:std::pow((x+0.055f)/1.055f,2.4f);}
float srgb(float x){return x<=0.0031308f?12.92f*x:1.055f*std::pow(x,1/2.4f)-0.055f;}
uint8_t byte(float x){return uint8_t(std::lround(std::clamp(x,0.0f,1.0f)*255));}
}
void Scene::load(const std::filesystem::path& path){
    std::ifstream f(path);if(!f)throw std::runtime_error("无法读取材质文件: "+path.string());
    J j;f>>j;demand(j.at("schemaVersion").is_number_integer()&&j.at("schemaVersion")==1,"不支持 schemaVersion");
    demand(j.at("textureSize").is_number_unsigned()||j.at("textureSize").is_number_integer(),"textureSize 必须为整数");
    int n=j.at("textureSize").get<int>();demand(n>=16&&n<=1024&&(n&(n-1))==0,"textureSize 必须为 16..1024 的二次幂");textureSize=uint32_t(n);
    const auto& arr=j.at("materials");demand(arr.is_array()&&!arr.empty(),"materials 必须为非空数组");
    std::map<std::string,int> ids; const std::array<std::string,5> types={"diffuse","rough_metal","mirror","dielectric","emissive"};
    for(const auto& m:arr){
        Material a{};a.id=m.at("id").get<std::string>();demand(!a.id.empty()&&!ids.contains(a.id),"材质 ID 为空或重复: "+a.id);
        std::string type=m.at("type").get<std::string>();auto it=std::find(types.begin(),types.end(),type);demand(it!=types.end(),"未知 type: "+type);int t=int(it-types.begin());
        V3 base=triple(m,"baseColorSRGB",1),em=triple(m,"emission",3.4e38f),abs=triple(m,"absorption",3.4e38f);
        float metal=number(m,"metallic",0,1),rough=number(m,"roughness",0,1),ior=number(m,"ior",1,3);
        a.colorVariation=number(m,"baseColorVariation",0,1);a.roughVariation=number(m,"roughnessVariation",0,1);
        a.pattern=m.at("pattern").get<std::string>();demand(a.pattern=="solid"||a.pattern=="stone_noise"||a.pattern=="wood_grain","未知 pattern");
        auto seed=m.at("textureSeed");demand(seed.is_number_integer()&&seed.get<double>()>=0&&seed.get<double>()<=4294967295.0,"textureSeed 必须为 uint32");a.seed=seed.get<uint32_t>();
        demand((t==0&&metal==0)||(t==1&&metal==1&&rough>0)||(t==2&&metal==1&&rough==0)||(t==3&&metal==0&&rough==0)||(t==4),"metallic/roughness 与 type 不符: "+a.id);
        demand(t==3?ior>1:ior==1,"ior 与 type 不符: "+a.id);
        demand(t==3||(abs.x==0&&abs.y==0&&abs.z==0),"非 dielectric 的 absorption 必须为零");
        demand(t==4||(em.x==0&&em.y==0&&em.z==0),"非 emissive 的 emission 必须为零");
        demand(t<2||(a.pattern=="solid"&&a.colorVariation==0&&a.roughVariation==0),"delta/emissive 必须是无 variation 的 solid");
        a.gpu={f4(base,metal),f4(em,float(t)),f4(abs,ior),{rough,0,0,0}};
        ids[a.id]=int(materials.size());materials.push_back(a);gpuMaterials.push_back(a.gpu);
    }
    auto id=[&](const char* s){demand(ids.contains(s),std::string("场景无法解析 ID: ")+s);return ids.at(s);};
    int stone=id("stone"),red=id("red"),blue=id("blue"),mirror=id("mirror"),wood=id("wood"),metal=id("rough_metal"),glass=id("glass"),glow=id("glow");
    constexpr int sx=16,sy=8,sz=16; std::array<int,sx*sy*sz> cells;cells.fill(-1);
    auto at=[&](int x,int y,int z)->int& {return cells[size_t(((y+1)*sz+z)*sx+x)];};
    auto box=[&](int x0,int x1,int y0,int y1,int z0,int z1,int m){for(int y=y0;y<=y1;++y)for(int z=z0;z<=z1;++z)for(int x=x0;x<=x1;++x)at(x,y,z)=m;};
    box(0,15,-1,-1,0,15,stone);box(0,0,0,5,0,15,red);box(15,15,0,5,0,15,blue);box(1,14,0,5,0,0,stone);
    for(int z=0;z<16;++z)for(int x=0;x<16;++x)if(!(x>=6&&x<=9&&z>=6&&z<=9))at(x,6,z)=stone;
    box(5,10,1,4,1,1,mirror);box(1,1,1,4,4,7,mirror);box(3,3,0,2,5,5,red);box(12,12,0,2,6,6,blue);
    box(4,4,0,1,10,10,wood);at(5,0,10)=wood;box(10,10,0,1,10,10,metal);
    for(int x:{4,11})for(int z:{4,11})at(x,5,z)=glow;
    auto get=[&](const std::array<int,3>& p){return p[0]<0||p[0]>=sx||p[1]<-1||p[1]>6||p[2]<0||p[2]>=sz?-1:at(p[0],p[1],p[2]);};
    std::vector<std::vector<Triangle>> byMat(materials.size());uint32_t surface=1;
    auto quad=[&](int axis,int sign,float plane,float u0,float v0,float u1,float v1,int mat){
        int u=(axis+1)%3,v=(axis+2)%3;V3 norm{};(&norm.x)[axis]=float(sign);
        auto p=[&](float a,float b){V3 r{};(&r.x)[axis]=plane;(&r.x)[u]=a;(&r.x)[v]=b;return r;};
        std::array<V3,4> q={p(u0,v0),p(u1,v0),p(u1,v1),p(u0,v1)};
        auto tri=[&](int a,int b,int c){Triangle t{};t.p0=f4(q[size_t(a)]);t.p1=f4(q[size_t(b)]);t.p2=f4(q[size_t(c)]);t.normal=f4(norm,float(mat));
            auto uv=[&](int i){return std::array<float,2>{(&q[size_t(i)].x)[u],(&q[size_t(i)].x)[v]};};auto aa=uv(a),bb=uv(b),cc=uv(c);
            t.uv01={aa[0],aa[1],bb[0],bb[1]};t.uv2={cc[0],cc[1],float(surface),0};byMat[size_t(mat)].push_back(t);};
        if(sign>0){tri(0,1,2);tri(0,2,3);}else{tri(0,2,1);tri(0,3,2);}++surface;++rectangles;
    };
    // 每个 axis/sign/plane 独立的暴露面掩码；宽优先再沿完整宽度扩高。
    const std::array<int,3> lo={0,-1,0},hi={16,7,16};
    for(int axis=0;axis<3;++axis)for(int sign:{-1,1})for(int d=lo[size_t(axis)];d<hi[size_t(axis)];++d){
        int u=(axis+1)%3,v=(axis+2)%3,w=hi[size_t(u)]-lo[size_t(u)],h=hi[size_t(v)]-lo[size_t(v)];std::vector<int> mask(size_t(w*h),-1);
        for(int y=0;y<h;++y)for(int x=0;x<w;++x){std::array<int,3> p{};p[size_t(axis)]=d;p[size_t(u)]=x+lo[size_t(u)];p[size_t(v)]=y+lo[size_t(v)];auto neighbor=p;neighbor[size_t(axis)]+=sign;
            int m=get(p);if(m>=0&&get(neighbor)<0)mask[size_t(y*w+x)]=m;}
        for(int y=0;y<h;++y)for(int x=0;x<w;++x){int m=mask[size_t(y*w+x)];if(m<0)continue;int rw=1,rh=1;
            while(x+rw<w&&mask[size_t(y*w+x+rw)]==m)++rw;
            for(;y+rh<h;++rh){bool match=true;for(int k=0;k<rw;++k)if(mask[size_t((y+rh)*w+x+k)]!=m)match=false;if(!match)break;}
            quad(axis,sign,float(d+(sign>0)),float(x+lo[size_t(u)]),float(y+lo[size_t(v)]),float(x+rw+lo[size_t(u)]),float(y+rh+lo[size_t(v)]),m);
            for(int yy=0;yy<rh;++yy)for(int xx=0;xx<rw;++xx)mask[size_t((y+yy)*w+x+xx)]=-1;
        }
    }
    // 玻璃独立提取整体边界，因此偏移后的底面和地板顶面均保留。
    const std::array<float,3> gl={7,0.125f,7},gh={9,2.125f,9};
    for(int a=0;a<3;++a)for(int s:{-1,1})quad(a,s,s<0?gl[size_t(a)]:gh[size_t(a)],gl[size_t((a+1)%3)],gl[size_t((a+2)%3)],gh[size_t((a+1)%3)],gh[size_t((a+2)%3)],glass);
    for(const auto& list:byMat)if(!list.empty()){groups.push_back({uint32_t(triangles.size()),uint32_t(list.size())});triangles.insert(triangles.end(),list.begin(),list.end());}
    float power=0;
    for(uint32_t i=0;i<triangles.size();++i){const auto& t=triangles[i];const auto& m=materials[size_t(t.normal.w)].gpu;float lum=0.2126f*m.emissionType.x+0.7152f*m.emissionType.y+0.0722f*m.emissionType.z;
        if(lum>0){V3 a{t.p0.x,t.p0.y,t.p0.z},b{t.p1.x,t.p1.y,t.p1.z},c{t.p2.x,t.p2.y,t.p2.z};V3 cr=cross(b-a,c-a);float area=std::sqrt(dot(cr,cr))*0.5f;float p=area*lum;power+=p;lights.push_back({{i,0,0,0},{area,p,power,0}});}}
    if(power>0){for(auto& l:lights){l.distribution.y/=power;l.distribution.z/=power;}lights.back().distribution.z=1;}
    TextureLevel first{textureSize,{}, {}};size_t texels=size_t(textureSize)*textureSize*materials.size();first.color.resize(texels*4);first.roughness.resize(texels);
    for(size_t layer=0;layer<materials.size();++layer){const auto& m=materials[layer];for(uint32_t y=0;y<textureSize;++y)for(uint32_t x=0;x<textureSize;++x){float u=float(x)/float(textureSize),v=float(y)/float(textureSize),q=0;
        if(m.pattern=="stone_noise")q=2*(0.55f*noise(u*5,v*5,m.seed)+0.3f*noise(u*13,v*13,m.seed+1)+0.15f*noise(u*31,v*31,m.seed+2))-1;
        if(m.pattern=="wood_grain")q=std::sin((v*12+0.5f*noise(u*3,v*3,m.seed))*6.2831853f);
        size_t k=(layer*textureSize+y)*textureSize+x;const float* base=&m.gpu.baseMetal.x;for(size_t c=0;c<3;++c)first.color[k*4+c]=byte(base[c]+q*m.colorVariation);first.color[k*4+3]=255;
        float r=std::clamp(m.gpu.misc.x+q*m.roughVariation,0.0f,1.0f);if(m.gpu.emissionType.w==1)r=std::max(r,1.0f/255);first.roughness[k]=byte(r);
    }}textures.push_back(std::move(first));
    while(textures.back().size>1){const auto& prev=textures.back();uint32_t s=prev.size/2;TextureLevel next{s,{}, {}};next.color.resize(size_t(s)*s*materials.size()*4);next.roughness.resize(size_t(s)*s*materials.size());
        for(size_t layer=0;layer<materials.size();++layer)for(uint32_t y=0;y<s;++y)for(uint32_t x=0;x<s;++x){size_t k=(layer*s+y)*s+x;float rgb[3]={},r=0;
            for(uint32_t yy=0;yy<2;++yy)for(uint32_t xx=0;xx<2;++xx){size_t z=(layer*prev.size+y*2+yy)*prev.size+x*2+xx;for(size_t c=0;c<3;++c)rgb[c]+=linear(float(prev.color[z*4+c])/255)*0.25f;r+=float(prev.roughness[z])*0.25f;}
            for(size_t c=0;c<3;++c)next.color[k*4+c]=byte(srgb(rgb[c]));next.color[k*4+3]=255;next.roughness[k]=uint8_t(std::lround(r));}textures.push_back(std::move(next));
    }mipCount=uint32_t(textures.size());
}
