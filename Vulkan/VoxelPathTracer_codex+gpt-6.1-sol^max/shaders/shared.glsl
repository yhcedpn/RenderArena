#ifndef SHARED_GLSL
#define SHARED_GLSL
#ifndef RW
#define RW
#endif
#extension GL_EXT_scalar_block_layout : require
const float PI=3.141592653589793;
struct Triangle {vec4 p0,p1,p2,normal,uv01,uv2;};
struct Material {vec4 baseMetal,emissionType,absorptionIor,misc;};
struct Light {uvec4 ids;vec4 distribution;};
struct Path {vec4 origin,direction,beta,previous;uvec4 id,flags;vec4 a0,a1,a2,translation;};
struct Hit {vec4 position,normal;uvec4 ids;vec4 uv;};
struct Guide {vec4 position,normal,albedo,actual,geometry,view;uvec4 ids,chain;vec4 aux;};
struct Reservoir {uvec4 meta;vec4 value;};
struct History {vec4 color,moments;};
struct Shadow {vec4 origin,direction,contribution;uvec4 info;};
layout(set=0,binding=1,scalar) readonly buffer Triangles {Triangle triangles[];};
layout(set=0,binding=2,scalar) readonly buffer Materials {Material materials[];};
layout(set=0,binding=3,scalar) readonly buffer Lights {Light lights[];};
layout(set=0,binding=4,scalar) uniform Frame {
    vec4 eye,right,up,forward,oldEye,oldRight,oldUp,oldForward,jitter;
    uvec4 extent,control;vec4 limits;
} frame;
layout(set=0,binding=5,scalar) RW buffer QueueA {Path qa[];};
layout(set=0,binding=6,scalar) RW buffer QueueB {Path qb[];};
layout(set=0,binding=7,scalar) RW buffer Hits {Hit hits[];};
layout(set=0,binding=8,scalar) RW buffer Counters {uint counters[];};
layout(set=0,binding=9,scalar) RW buffer Signals {vec4 signals[];};
layout(set=0,binding=10,scalar) RW buffer Guides {Guide guides[];};
layout(set=0,binding=11,scalar) readonly buffer OldGuides {Guide oldGuides[];};
layout(set=0,binding=12,scalar) RW buffer InitialReservoirs {Reservoir initialR[];};
layout(set=0,binding=13,scalar) RW buffer TemporalReservoirs {Reservoir temporalR[];};
layout(set=0,binding=14,scalar) RW buffer SpatialReservoirs {Reservoir spatialR[];};
layout(set=0,binding=15,scalar) readonly buffer OldReservoirs {Reservoir oldR[];};
layout(set=0,binding=16,scalar) RW buffer TemporalHistory {History history[];};
layout(set=0,binding=17,scalar) readonly buffer OldHistory {History oldHistory[];};
layout(set=0,binding=18,scalar) RW buffer FilterA {vec4 filterA[];};
layout(set=0,binding=19,scalar) RW buffer FilterB {vec4 filterB[];};
layout(set=0,binding=20,scalar) RW buffer Shadows {Shadow shadows[];};
layout(set=0,binding=21,scalar) RW buffer Visibility {uint visibility[];};
layout(set=0,binding=22) uniform sampler2DArray colorTexture;
layout(set=0,binding=23) uniform sampler2DArray roughTexture;
layout(push_constant,scalar) uniform Push {uint mode,queue,iteration,pad;} pc;
uint pixels(){return frame.extent.x*frame.extent.y;}
uint hash32(uint x){x^=x>>16;x*=0x7feb352du;x^=x>>15;x*=0x846ca68bu;return x^(x>>16);}
float rnd(inout uint state){state=hash32(state+0x9e3779b9u);return float(state>>8)*(1.0/16777216.0);}
float luminance(vec3 v){return dot(v,vec3(0.2126,0.7152,0.0722));}
float max3(vec3 v){return max(v.x,max(v.y,v.z));}
vec3 mapVector(Path p,vec3 v){return vec3(dot(p.a0.xyz,v),dot(p.a1.xyz,v),dot(p.a2.xyz,v));}
void appendReflection(inout Path p,vec3 n,vec3 point,uint surface,uint event){
    if((p.flags.y&4u)!=0u&&(p.flags.y&16u)==0u)return;
    vec3 an=mapVector(p,n);float d=dot(n,point);
    p.a0.xyz-=2.0*an.x*n;p.a1.xyz-=2.0*an.y*n;p.a2.xyz-=2.0*an.z*n;
    p.translation.xyz+=2.0*d*an;
    p.flags.z++;p.flags.w=hash32(p.flags.w^hash32(surface*3u+event));
}
void appendTransmission(inout Path p,uint surface,uint event){
    p.flags.y|=8u;
    if((p.flags.y&4u)==0u||(p.flags.y&16u)!=0u){p.flags.z++;p.flags.w=hash32(p.flags.w^hash32(surface*3u+event));}
}
vec3 offsetRay(vec3 p,vec3 n,vec3 d){return p+n*(dot(n,d)>=0.0?0.0002:-0.0002);}
vec3 basisSample(vec3 n,vec3 local){vec3 t=normalize(cross(abs(n.y)<0.99?vec3(0,1,0):vec3(1,0,0),n));return t*local.x+cross(n,t)*local.y+n*local.z;}
float smithLambda(vec3 n,vec3 v,float a){float c=abs(dot(n,v));return 0.5*(sqrt(1.0+a*a*max(0.0,1.0-c*c)/max(c*c,1e-14))-1.0);}
float ggxD(float nh,float a){float den=(1.0-nh*nh)+a*a*nh*nh;return a*a/(PI*den*den);}
vec3 bsdf(Guide g,vec3 wi,out float pdf){
    vec3 n=g.geometry.xyz,wo=g.view.xyz;float ni=dot(n,wi),no=dot(n,wo);pdf=0.0;
    if(ni<=0.0||no<=0.0)return vec3(0);
    if(g.albedo.w==1.0){pdf=ni/PI;return g.albedo.xyz/PI;}
    vec3 h=normalize(wi+wo);float a=max(g.normal.w*g.normal.w,1e-6);float d=ggxD(max(dot(n,h),0.0),a);
    float lo=smithLambda(n,wo,a),li=smithLambda(n,wi,a);vec3 f=g.albedo.xyz+(vec3(1)-g.albedo.xyz)*pow(1.0-max(dot(wo,h),0.0),5.0);
    pdf=d/(4.0*no*(1.0+lo));return f*d/(4.0*no*ni*(1.0+lo+li));
}
vec3 sampleBSDF(Guide g,inout uint rng,out float pdf,out vec3 f){
    float u=rnd(rng),v=rnd(rng);vec3 n=g.geometry.xyz,wi;
    if(g.albedo.w==1.0){float r=sqrt(u),a=2.0*PI*v;wi=basisSample(n,vec3(r*cos(a),r*sin(a),sqrt(1.0-u)));}
    else {
        // Heitz isotropic GGX VNDF：拉伸视向，圆盘采样，再逆拉伸法线。
        vec3 t=normalize(cross(abs(n.y)<0.99?vec3(0,1,0):vec3(1,0,0),n)),b=cross(n,t),wo=g.view.xyz;
        float alpha=max(g.normal.w*g.normal.w,1e-6);vec3 vh=normalize(vec3(alpha*dot(wo,t),alpha*dot(wo,b),dot(wo,n)));
        float lensq=dot(vh.xy,vh.xy);vec3 t1=lensq>0.0?vec3(-vh.y,vh.x,0)/sqrt(lensq):vec3(1,0,0),t2=cross(vh,t1);
        float r=sqrt(u),a=2.0*PI*v,x=r*cos(a),y=r*sin(a),s=0.5*(1.0+vh.z);y=(1.0-s)*sqrt(max(0.0,1.0-x*x))+s*y;
        vec3 h=x*t1+y*t2+sqrt(max(0.0,1.0-x*x-y*y))*vh;h=normalize(vec3(alpha*h.xy,max(0.0,h.z)));
        h=t*h.x+b*h.y+n*h.z;wi=reflect(-wo,h);
    }
    f=bsdf(g,wi,pdf);return wi;
}
vec2 sampleLight(inout uint rng,out uint lightID){
    float u=rnd(rng);uint lo=0u,hi=frame.control.w;
    while(lo<hi){uint mid=(lo+hi)/2u;if(lights[mid].distribution.z<u)lo=mid+1u;else hi=mid;}
    lightID=min(lo,frame.control.w-1u);float a=sqrt(rnd(rng)),b=rnd(rng);return vec2(1.0-a,a*b);
}
vec3 lightPoint(uint id,vec2 uv){Triangle t=triangles[lights[id].ids.x];return t.p0.xyz*(1.0-uv.x-uv.y)+t.p1.xyz*uv.x+t.p2.xyz*uv.y;}
vec3 directValue(Guide g,uint id,vec2 uv,out float bsdfPdf,out float solidPdf){
    Light l=lights[id];Triangle t=triangles[l.ids.x];vec3 delta=lightPoint(id,uv)-g.actual.xyz;float d2=dot(delta,delta);bsdfPdf=0;solidPdf=0;if(d2<=1e-10)return vec3(0);
    vec3 wi=delta/sqrt(d2);float cl=max(dot(t.normal.xyz,-wi),0.0),cs=max(dot(g.geometry.xyz,wi),0.0);if(cl<=0.0||cs<=0.0)return vec3(0);
    solidPdf=(l.distribution.y/l.distribution.x)*d2/cl;
    return bsdf(g,wi,bsdfPdf)*materials[uint(t.normal.w)].emissionType.xyz*(cl*cs/d2);
}
float target(Guide g,uint id,vec2 uv){float a,b;return luminance(directValue(g,id,uv,a,b));}
float lightSolidPdf(uint tri,vec3 from,vec3 to){
    for(uint i=0u;i<frame.control.w;++i)if(lights[i].ids.x==tri){vec3 d=to-from;float d2=dot(d,d),c=max(dot(triangles[tri].normal.xyz,-normalize(d)),0.0);return c>0.0?lights[i].distribution.y/lights[i].distribution.x*d2/c:0.0;}return 0.0;
}
float powerMIS(float a,float b){return a*a/(a*a+b*b);}
int reproject(Guide g){
    vec3 d=g.position.xyz-frame.oldEye.xyz;float z=dot(d,frame.oldForward.xyz);if(z<=0.0)return -1;
    vec2 ndc=vec2(dot(d,frame.oldRight.xyz)/frame.oldRight.w,dot(d,frame.oldUp.xyz)/frame.oldUp.w)/z;
    // position 由本帧 jitter 光线得到；旧采样中心还需减去上一帧 jitter。
    vec2 uv=(ndc*vec2(0.5,-0.5)+0.5)*vec2(frame.extent.xy)-0.5-frame.jitter.zw;
    ivec2 p=ivec2(floor(uv+0.5));if(any(lessThan(p,ivec2(0)))||any(greaterThanEqual(p,ivec2(frame.extent.xy))))return -1;return p.y*int(frame.extent.x)+p.x;
}
bool compatible(Guide a,Guide b,bool spatial){
    if(a.ids.x==0u||b.ids.x==0u||a.albedo.w!=b.albedo.w||a.ids.y!=b.ids.y||a.ids.z!=b.ids.z||a.ids.w!=b.ids.w||any(notEqual(a.chain,b.chain)))return false;
    if(dot(a.normal.xyz,b.normal.xyz)<0.98||abs(a.normal.w-b.normal.w)>0.12)return false;
    if(a.chain.w==2u){
        if(a.aux.z!=b.aux.z||a.geometry.w!=b.geometry.w||dot(a.geometry.xyz,b.geometry.xyz)<0.98||abs(a.aux.w-b.aux.w)>0.12)return false;
        float terminalTolerance=max(0.025,a.aux.x*3.0);
        if(spatial){if(abs(dot(a.actual.xyz-b.actual.xyz,a.geometry.xyz))>0.035)return false;}
        else if(length(a.actual.xyz-b.actual.xyz)>terminalTolerance||abs(a.actual.w-b.actual.w)>terminalTolerance)return false;
    }
    // 世界单位：时间容差覆盖一个像素 footprint；空间仅比较离面距离，保留同平面的纹理。
    float tol=spatial?0.035:max(0.015,a.aux.x*2.5);
    if(spatial)return abs(dot(a.position.xyz-b.position.xyz,a.normal.xyz))<tol;
    float previousDistance=a.chain.z!=0u?a.aux.y:length(a.position.xyz-frame.oldEye.xyz);
    return length(a.position.xyz-b.position.xyz)<tol&&abs(previousDistance-b.aux.y)<max(0.05,a.aux.x*4.0);
}
#endif
