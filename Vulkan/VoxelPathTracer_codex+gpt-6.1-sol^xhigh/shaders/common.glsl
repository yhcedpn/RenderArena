#ifndef ARENA_COMMON
#define ARENA_COMMON
#ifdef DISPLAY_READONLY
#define RW readonly
#else
#define RW
#endif
#extension GL_EXT_scalar_block_layout : require
const float PI=3.14159265358979323846;
const uint INVALID=0xffffffffu;
struct Triangle {vec4 a,b,c,n;uvec4 id;};
struct Material {vec4 absorptionIor,emissionType;};
struct Light {uint triangle;float pdf,cdf,area;};
struct Path {vec4 o,d,beta,previous,signs,offset;uvec4 info,chain0,chain1;};
struct Hit {vec4 data;uvec4 id;};
struct Shadow {vec4 o,d,value;uvec4 id;};
struct Guide {vec4 p,n,albedo,real;uvec4 id,chain0,chain1;};
struct Reservoir {vec4 chosen;vec4 stats;};
struct History {vec4 value,moments;};
layout(set=0,binding=1,scalar) readonly buffer Triangles {Triangle tri[];};
layout(set=0,binding=2,scalar) readonly buffer Materials {Material mat[];};
layout(set=0,binding=3,scalar) readonly buffer Lights {Light light[];};
layout(set=0,binding=4) uniform sampler2DArray colorTex;
layout(set=0,binding=5) uniform sampler2DArray roughTex;
layout(set=0,binding=6,scalar) readonly buffer Parameters {
 vec4 eye,forward,right,up,oldEye,oldForward,oldRight,oldUp,jitter;
 uvec4 size,config;vec4 lens;
} params;
layout(set=0,binding=7,scalar) RW buffer Paths {Path paths[];};
layout(set=0,binding=8,scalar) RW buffer Hits {Hit hits[];};
layout(set=0,binding=9,scalar) RW buffer Queues {uint queues[];};
layout(set=0,binding=10,scalar) RW buffer Counters {uint counts[];};
layout(set=0,binding=11,scalar) RW buffer Shadows {Shadow shadows[];};
layout(set=0,binding=12,scalar) RW buffer Radiance {vec4 radiance[];};
layout(set=0,binding=13,scalar) RW buffer Guides {Guide guides[];};
layout(set=0,binding=14,scalar) RW buffer Reservoirs {Reservoir reservoirs[];};
layout(set=0,binding=15,scalar) RW buffer Histories {History histories[];};
layout(set=0,binding=16,scalar) RW buffer Filters {vec4 filtered[];};
layout(push_constant) uniform Push {uint queue,stage,phase,step;} pc;
uint pixels(){return params.size.x*params.size.y;}
uint capacity(){return 2u*pixels();}
uint signals(){return 3u*pixels();}
uint currentGuide(uint slot){return (params.size.z&1u)*signals()+slot;}
uint oldGuide(uint slot){return ((params.size.z&1u)^1u)*signals()+slot;}
uint hash(uint x){x^=x>>16;x*=0x7feb352du;x^=x>>15;x*=0x846ca68bu;return x^(x>>16);}
uint seed(uint pixel,uint dimension){return hash(pixel^hash(params.size.z+0x9e3779b9u)^hash(dimension+0x87654321u));}
float random(inout uint s){s=hash(s+0x9e3779b9u);return float(s>>8)*(1.0/16777216.0);}
float luminance(vec3 c){return dot(c,vec3(0.2126,0.7152,0.0722));}
vec3 unit(vec3 v){return normalize(v);}
vec3 offsetRay(vec3 p,vec3 n,vec3 d){return p+n*(dot(n,d)>=0?0.0002:-0.0002);}// 世界单位；比玻璃抬升量小三个数量级。
vec2 uvOf(vec3 p,vec3 n){if(abs(n.x)>0.5)return p.yz;if(abs(n.y)>0.5)return p.zx;return p.xy;}
vec3 colorAt(uint material,vec3 p,vec3 n,float lod){return textureLod(colorTex,vec3(uvOf(p,n),float(material)),lod).rgb;}
float roughAt(uint material,vec3 p,vec3 n,float lod){return textureLod(roughTex,vec3(uvOf(p,n),float(material)),lod).r;}
void basis(vec3 n,out vec3 t,out vec3 b){t=normalize(cross(abs(n.y)<0.99?vec3(0,1,0):vec3(1,0,0),n));b=cross(n,t);}
vec3 local(vec3 x,vec3 t,vec3 b,vec3 n){return vec3(dot(x,t),dot(x,b),dot(x,n));}
float smithLambda(float nz,float a){float z2=nz*nz;return 0.5*(sqrt(1.0+a*a*max(0.0,1.0-z2)/max(z2,1e-12))-1.0);}
vec3 bsdf(vec3 n,vec3 wo,vec3 wi,vec3 albedo,float rough,uint type,out float pdf){
 float ni=dot(n,wi),no=dot(n,wo);pdf=0;if(ni<=0||no<=0)return vec3(0);
 if(type==0u){pdf=ni/PI;return albedo/PI;}
 if(type!=1u)return vec3(0);
 float a=max(rough*rough,1e-5);vec3 h=normalize(wo+wi);float nh=clamp(dot(n,h),0,1),oh=max(dot(wo,h),0);if(oh<=0)return vec3(0);
 float denominator=(1-nh*nh)+a*a*nh*nh;float d=a*a/(PI*denominator*denominator);float lv=smithLambda(no,a),li=smithLambda(ni,a);float g2=1/(1+lv+li),g1=1/(1+lv);
 vec3 f=albedo+(1-albedo)*pow(1-oh,5);pdf=d*g1/(4*no);return f*(d*g2/(4*ni*no));
}
vec3 sampleBsdf(vec3 n,vec3 wo,float rough,uint type,inout uint rng){
 vec3 t,b;basis(n,t,b);float u=random(rng),v=random(rng);
 if(type==0u){float r=sqrt(u),a=2*PI*v;return t*(r*cos(a))+b*(r*sin(a))+n*sqrt(1-u);}
 // Heitz 的 GGX 可见法线分布采样；PDF 使用 D*G1/(4*n.wo)。
 float a=max(rough*rough,1e-5);vec3 vl=local(wo,t,b,n),vh=normalize(vec3(a*vl.xy,vl.z));float l=dot(vh.xy,vh.xy);vec3 t1=l>0?vec3(-vh.y,vh.x,0)/sqrt(l):vec3(1,0,0),t2=cross(vh,t1);float r=sqrt(u),phi=2*PI*v;float x=r*cos(phi),y=r*sin(phi),s=0.5*(1+vh.z);y=(1-s)*sqrt(max(0,1-x*x))+s*y;vec3 nh=x*t1+y*t2+sqrt(max(0,1-x*x-y*y))*vh;vec3 h=normalize(vec3(a*nh.xy,max(0,nh.z)));return reflect(-wo,t*h.x+b*h.y+n*h.z);
}
float dielectricF(float cosI,float etaI,float etaT,out float cosT){float sin2=pow(etaI/etaT,2)*max(0,1-cosI*cosI);if(sin2>=1){cosT=0;return 1;}cosT=sqrt(1-sin2);float rs=(etaI*cosI-etaT*cosT)/(etaI*cosI+etaT*cosT),rp=(etaT*cosI-etaI*cosT)/(etaT*cosI+etaI*cosT);return 0.5*(rs*rs+rp*rp);}
float powerMis(float a,float b){return a*a/(a*a+b*b);}
vec4 sampleLight(inout uint rng){if(params.config.x==0u)return vec4(0);float u=random(rng);uint lo=0,hi=params.config.x;while(lo<hi){uint m=(lo+hi)/2;if(light[m].cdf<u)lo=m+1;else hi=m;}uint id=min(lo,params.config.x-1);float s=sqrt(random(rng)),t=random(rng);return vec4(float(id),1-s,s*t,1);}
vec3 lightPoint(vec4 chosen){Triangle t=tri[light[uint(chosen.x)].triangle];return t.a.xyz*chosen.y+t.b.xyz*chosen.z+t.c.xyz*(1-chosen.y-chosen.z);}
vec3 directValue(Guide g,vec4 chosen,vec3 camera,out vec3 wi,out float dist,out float directionalPdf){
 wi=vec3(0);dist=0;directionalPdf=0;if(chosen.w==0||g.p.w==0)return vec3(0);Light l=light[uint(chosen.x)];Triangle t=tri[l.triangle];vec3 delta=lightPoint(chosen)-g.real.xyz;float d2=dot(delta,delta);if(d2<1e-8)return vec3(0);dist=sqrt(d2);wi=delta/dist;float nl=max(0,dot(t.n.xyz,-wi)),ns=max(0,dot(g.n.xyz,wi));if(nl<=0||ns<=0)return vec3(0);float pdf;vec3 f=bsdf(g.n.xyz,normalize(camera-g.real.xyz),wi,g.albedo.rgb,g.n.w,uint(mat[g.id.y].emissionType.w),pdf);directionalPdf=l.pdf*d2/nl;return mat[t.id.x].emissionType.rgb*f*(ns*nl/d2);
}
float target(Guide g,vec4 chosen,vec3 camera){vec3 wi;float d,p;return luminance(directValue(g,chosen,camera,wi,d,p));}
Guide emptyGuide(){Guide g;g.p=g.n=g.albedo=g.real=vec4(0);g.id=g.chain0=g.chain1=uvec4(0);return g;}
bool sameChain(Guide a,Guide b){return a.id.x==b.id.x&&a.id.y==b.id.y&&a.id.z==b.id.z&&a.id.w==b.id.w&&all(equal(a.chain0,b.chain0))&&all(equal(a.chain1,b.chain1));}
bool compatible(Guide a,Guide b,bool temporal){
 if(a.p.w==0||b.p.w==0||!sameChain(a,b)||dot(a.n.xyz,b.n.xyz)<0.95||abs(a.n.w-b.n.w)>0.12)return false;
 float footprint=2*params.lens.x*max(length(a.p.xyz-params.eye.xyz),1)/float(params.size.y);
 // 位置/深度容差以世界单位表示；随屏幕像素 footprint 缩放，不依赖材质 ID。
 if(temporal)return length(a.p.xyz-b.p.xyz)<0.04+3*footprint&&abs(a.real.w-b.real.w)<0.06+4*footprint;
 return abs(dot(a.p.xyz-b.p.xyz,a.n.xyz))<0.02+footprint&&abs(a.real.w-b.real.w)<0.1+32*footprint;
}
vec2 project(vec3 p,bool old){vec3 delta=p-(old?params.oldEye.xyz:params.eye.xyz);vec3 f=old?params.oldForward.xyz:params.forward.xyz,r=old?params.oldRight.xyz:params.right.xyz,u=old?params.oldUp.xyz:params.up.xyz;float z=dot(delta,f);if(z<=0)return vec2(-100000);vec2 ndc=vec2(dot(delta,r)/(z*params.lens.x*params.lens.y),-dot(delta,u)/(z*params.lens.x));return (ndc*0.5+0.5)*vec2(params.size.xy);}
ivec2 historyPixel(uint pixel,Guide g){vec2 xy=vec2(pixel%params.size.x,pixel/params.size.x)+0.5;vec2 h;
 if((g.id.w&4u)!=0u)h=xy+params.jitter.xy-params.jitter.zw;
 else {vec2 unjitteredCurrent=project(g.p.xyz,false)-params.jitter.xy;vec2 motion=(project(g.p.xyz,true)-params.jitter.zw)-unjitteredCurrent;h=xy+motion;}
 return ivec2(floor(h));
}
bool inside(ivec2 p){return all(greaterThanEqual(p,ivec2(0)))&&all(lessThan(p,ivec2(params.size.xy)));}
vec3 demod(vec3 c,Guide g){if((g.id.w&1u)==0u)return c;return vec3(g.albedo.r>0?c.r/g.albedo.r:0,g.albedo.g>0?c.g/g.albedo.g:0,g.albedo.b>0?c.b/g.albedo.b:0);}
#endif





