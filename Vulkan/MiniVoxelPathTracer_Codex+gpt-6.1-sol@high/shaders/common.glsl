#extension GL_EXT_scalar_block_layout : require


#ifdef RT_STAGE
#extension GL_EXT_ray_tracing : require
#endif
struct Triangle { vec4 a,b,c,n; uvec4 info; };
struct Material { vec4 color,absorption,emission,params; };
struct Light { uint tri; float area,pdf,cdf; };
struct Path { vec4 o,d,beta,r0,r1,r2; uvec4 id,flags,chain0,chain1; vec4 data,prev; };
struct Hit { float t; uint tri; };
struct Guide { vec4 p,n,albedo,wo,realp; uvec4 id,opt,chain0,chain1; };
struct Reservoir { uint tri; float u,v,sum; float M,W,target,pad; };
struct Shadow { vec4 o,d,value; uvec4 id; };
struct History { vec4 color,moments; };
#ifdef RT_STAGE
layout(set=0,binding=0) uniform accelerationStructureEXT world;
#endif
layout(set=0,binding=1,scalar) readonly buffer Geometry { Triangle triangles[]; };
layout(set=0,binding=2,scalar) readonly buffer Materials { Material materials[]; };
layout(set=0,binding=3,scalar) readonly buffer Lights { Light lights[]; };
layout(set=0,binding=4) uniform sampler2DArray baseTex;
layout(set=0,binding=5) uniform sampler2DArray roughTex;
layout(set=0,binding=6,scalar) readonly buffer Frame {

 vec4 cam[8];
 uvec4 extent;
 vec4 jitter;
 uvec4 settings;
} f;
layout(push_constant,scalar) uniform Push {uint q,bounce,pass,step;} pc;
layout(set=0,binding=7,scalar) buffer QueueA {Path v[];} paths0;
layout(set=0,binding=8,scalar) buffer QueueB {Path v[];} paths1;
layout(set=0,binding=9,scalar) buffer HitData {Hit v[];} hitBuffer;
layout(set=0,binding=10,scalar) buffer CountData {uint v[];} countBuffer;
layout(set=0,binding=11,scalar) buffer ShadowData {Shadow v[];} shadowBuffer;
layout(set=0,binding=12,scalar) buffer RadianceData {vec4 v[];} radianceBuffer;
layout(set=0,binding=13,scalar) buffer GuideData {Guide v[];} guideBuffer;
layout(set=0,binding=14,scalar) buffer OldGuideData {Guide v[];} oldGuideBuffer;
layout(set=0,binding=15,scalar) buffer InitialReservoirData {Reservoir v[];} initialReservoir;
layout(set=0,binding=16,scalar) buffer TemporalReservoirData {Reservoir v[];} temporalReservoir;
layout(set=0,binding=17,scalar) buffer FinalReservoirData {Reservoir v[];} finalReservoir;
layout(set=0,binding=18,scalar) buffer OldReservoirData {Reservoir v[];} oldReservoir;
layout(set=0,binding=19,scalar) buffer TemporalHistoryData {History v[];} temporalHistory;
layout(set=0,binding=20,scalar) buffer OldHistoryData {History v[];} oldHistory;
layout(set=0,binding=21,scalar) buffer FilterA {vec4 v[];} filter0;
layout(set=0,binding=22,scalar) buffer FilterB {vec4 v[];} filter1;
layout(set=0,binding=23,scalar) buffer OutputData {vec4 v[];} outputBuffer;
layout(set=0,binding=24,scalar) buffer ShadowResultData {vec4 v[];} shadowResultBuffer;
layout(set=0,binding=25,scalar) buffer IndirectData {uint v[];} indirectBuffer;
#define hits() hitBuffer
#define counts() countBuffer
#define shadows() shadowBuffer
#define radiance() radianceBuffer
#define guides() guideBuffer
#define oldGuides() oldGuideBuffer
#define initialR() initialReservoir
#define temporalR() temporalReservoir
#define finalR() finalReservoir
#define oldR() oldReservoir
#define temporalH() temporalHistory
#define oldH() oldHistory
#define outputImage() outputBuffer
#define shadowResults() shadowResultBuffer
#define indirect() indirectBuffer
Path loadPath(uint i){return pc.q==0u?paths0.v[i]:paths1.v[i];}
void storeInput(uint i,Path p){if(pc.q==0u)paths0.v[i]=p;else paths1.v[i]=p;}
void storeOutput(uint i,Path p){if(pc.q==0u)paths1.v[i]=p;else paths0.v[i]=p;}
vec4 loadFilter(uint i){return pc.q==0u?filter0.v[i]:filter1.v[i];}
void storeFilter(uint i,vec4 value){if(pc.q==0u)filter1.v[i]=value;else filter0.v[i]=value;}
uint pixels(){return f.extent.x*f.extent.y;}
uint hash(uint x){x^=x>>16;x*=0x7feb352du;x^=x>>15;x*=0x846ca68bu;return x^(x>>16);}
float random(inout uint s){s=hash(s+0x9e3779b9u);return float(s>>8)*(1.0/16777216.0);}
uint seed(uint pixel,uint branch,uint dim){return hash(pixel^hash(f.extent.z+1)^hash(branch*173u+dim*7919u));}
float lum(vec3 x){return dot(x,vec3(.2126,.7152,.0722));}
vec3 offsetRay(vec3 p,vec3 n,vec3 d){return p+n*(dot(n,d)>0?0.0002:-0.0002);}
vec3 local(vec3 v,vec3 n){vec3 t=normalize(cross(abs(n.z)<.99?vec3(0,0,1):vec3(0,1,0),n));return vec3(dot(v,t),dot(v,cross(n,t)),dot(v,n));}
vec3 global(vec3 v,vec3 n){vec3 t=normalize(cross(abs(n.z)<.99?vec3(0,0,1):vec3(0,1,0),n));return t*v.x+cross(n,t)*v.y+n*v.z;}
float Dggx(float nz,float a){float d=nz*nz*(a*a-1)+1;return a*a/(3.14159265359*d*d);}
float lambda(float nz,float a){return .5*(sqrt(1+a*a*max(0,1-nz*nz)/(nz*nz))-1);}
vec3 bsdf(vec3 wo,vec3 wi,vec3 n,vec3 albedo,float rough,uint type,out float pdf){
 float nv=dot(n,wo),nl=dot(n,wi);pdf=0;if(nv<=0||nl<=0)return vec3(0);
 if(type==0u){pdf=nl/3.14159265359;return albedo/3.14159265359;}
 vec3 h=normalize(wo+wi);float nh=max(dot(n,h),0),vh=max(dot(wo,h),0);float a=max(rough*rough,1e-5);
 float d=Dggx(nh,a),lv=lambda(nv,a),ll=lambda(nl,a);
 vec3 fr=albedo+(1-albedo)*pow(1-vh,5);
 pdf=d/(4*nv*(1+lv));return fr*d/(4*nv*nl*(1+lv+ll));
}
vec3 sampleBSDF(vec3 wo,vec3 n,float rough,uint type,inout uint s){
 float u=random(s),v=random(s);
 if(type==0u){float r=sqrt(u),a=6.28318530718*v;return global(vec3(r*cos(a),r*sin(a),sqrt(1-u)),n);}
 // 可见法线采样在拉伸空间内执行；与 bsdf 中的 VNDF 方向 PDF 一致。
 float a=max(rough*rough,1e-5);vec3 V=normalize(local(wo,n)*vec3(a,a,1));
 vec3 T1=V.z<.99999?normalize(cross(vec3(0,0,1),V)):vec3(1,0,0),T2=cross(V,T1);
 float r=sqrt(u),phi=6.28318530718*v,t1=r*cos(phi),t2=r*sin(phi),k=.5*(1+V.z);
 t2=(1-k)*sqrt(max(0,1-t1*t1))+k*t2;
 vec3 Nh=T1*t1+T2*t2+V*sqrt(max(0,1-t1*t1-t2*t2));
 vec3 h=global(normalize(vec3(a*Nh.xy,max(0,Nh.z))),n);return reflect(-wo,h);
}
void textureAt(Triangle t,vec3 p,float footprint,out vec3 a,out float r){
 uint axis=t.info.z;vec2 uv=axis==0u?p.yz:(axis==1u?p.zx:p.xy);
 float lod=clamp(log2(max(footprint*float(f.settings.z & 65535u),1.0)),0.0,log2(float(f.settings.z & 65535u)));
 a=textureLod(baseTex,vec3(uv,t.info.x),lod).rgb;r=textureLod(roughTex,vec3(uv,t.info.x),lod).r;
}
vec3 lightPoint(Reservoir r){Triangle t=triangles[r.tri];return t.a.xyz*(1-r.u-r.v)+t.b.xyz*r.u+t.c.xyz*r.v;}
Reservoir fresh(inout uint s){
 float x=random(s);uint lo=0,hi=f.settings.x-1;while(lo<hi){uint m=(lo+hi)/2;if(lights[m].cdf<x)lo=m+1;else hi=m;}
 float u=sqrt(random(s)),v=random(s);Reservoir r; r.tri=lights[lo].tri;r.u=1-u;r.v=u*v;r.sum=0;r.M=0;r.W=0;r.target=0;r.pad=0;return r;
}
float areaPDF(uint tri){uint i=triangles[tri].info.w;return i==0u?0:lights[i-1].pdf/lights[i-1].area;}
vec3 direct(Guide g,Reservoir r,out float dirPdf,out float bp){
 vec3 delta=lightPoint(r)-g.realp.xyz;float d2=dot(delta,delta);dirPdf=0;bp=0;if(d2<=1e-10)return vec3(0);
 vec3 wi=delta*inversesqrt(d2);Triangle t=triangles[r.tri];float cl=max(dot(t.n.xyz,-wi),0),cs=max(dot(g.n.xyz,wi),0);
 // 普通主表面 n 与 realp 同域；光学终端的 NEE 在局部临时 guide 中求值。
 if(cl<=0||cs<=0)return vec3(0);
 dirPdf=areaPDF(r.tri)*d2/cl;
 return bsdf(g.wo.xyz,wi,g.n.xyz,g.albedo.rgb,g.n.w,g.id.y==0xffffffffu?0u:uint(materials[g.id.y].params.x),bp)*materials[t.info.x].emission.rgb*(cs*cl/d2);
}
float target(Guide g,Reservoir r){float dp,bp;return lum(direct(g,r,dp,bp));}
Reservoir emptyR(){Reservoir r;r.tri=0;r.u=0;r.v=0;r.sum=0;r.M=0;r.W=0;r.target=0;r.pad=0;return r;}
void offer(inout Reservoir dst,Reservoir candidate,float weight,float m,inout uint s){
 dst.M+=m;if(weight<=0)return;dst.sum+=weight;if(random(s)*dst.sum<weight){dst.tri=candidate.tri;dst.u=candidate.u;dst.v=candidate.v;}
}
bool compatible(Guide a,Guide b){
 if(a.id.w==0u||b.id.w==0u||any(notEqual(a.id.xyz,b.id.xyz))||any(notEqual(a.opt,b.opt))||any(notEqual(a.chain0,b.chain0))||any(notEqual(a.chain1,b.chain1)))return false;
 // 位置阈值以世界单位计，允许同一像素足迹内的 jitter；法线限制阻止跨面渗色。
 return dot(a.n.xyz,b.n.xyz)>.98&&abs(a.n.w-b.n.w)<.15&&length(a.p.xyz-b.p.xyz)<max(.08,.015*length(a.p.xyz-f.cam[0].xyz)) && (a.id.z!=3u || abs(a.realp.w-b.realp.w)<max(.08,.02*a.realp.w));
}
ivec2 projectOld(Guide g,uint pixel){
 if(g.id.z==3u){vec2 xy=vec2(pixel%f.extent.x,pixel/f.extent.x)+f.jitter.xy-f.jitter.zw;return ivec2(floor(xy+.5));}
 vec3 d=g.p.xyz-f.cam[4].xyz;float z=dot(d,f.cam[5].xyz);if(z<=.05)return ivec2(-1);
 vec2 ndc=vec2(dot(d,f.cam[6].xyz)/f.cam[6].w,dot(d,f.cam[7].xyz)/f.cam[7].w)/z;
 return ivec2(floor((ndc*vec2(.5,-.5)+.5)*vec2(f.extent.xy)-f.jitter.zw));
}
int previous(Guide g,uint slot){
 if(f.extent.w!=0u||(g.id.z==3u&&f.settings.w!=0u))return -1;
 ivec2 p=projectOld(g,slot/3);if(any(lessThan(p,ivec2(0)))||any(greaterThanEqual(p,ivec2(f.extent.xy))))return -1;
 uint i=(uint(p.y)*f.extent.x+uint(p.x))*3+slot%3;return compatible(g,oldGuides().v[i])?int(i):-1;
}
void addRadiance(uint slot,vec3 c){radiance().v[slot].rgb+=c;}
void shadowTask(vec3 p,vec3 n,Reservoir r,vec3 value,uint slot){
 vec3 delta=lightPoint(r)-p;float dist=length(delta);uint i=atomicAdd(counts().v[2],1);
 Shadow t;t.o=vec4(offsetRay(p,n,delta),0);t.d=vec4(delta/dist,max(0,dist-.0006));t.value=vec4(value,0);t.id=uvec4(slot,0,0,0);shadows().v[i]=t;
}
