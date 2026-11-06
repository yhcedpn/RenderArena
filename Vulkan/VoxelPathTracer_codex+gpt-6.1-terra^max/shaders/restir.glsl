float luminance(vec3 v) { return dot(v,vec3(0.2126,0.7152,0.0722)); }
vec2 jitterFor(uint pixel,uint frameNumber) {
    uint state=hashValue(pixel^hashValue(frameNumber+0x91e10da5u));
    float x=randomValue(state),y=randomValue(state); return vec2(x,y);
}
vec3 lightPoint(uint lightIndex,vec2 bary) {
    uint tri=lights[lightIndex].triangle;
    return vertices[3u*tri].position*(1.0-bary.x-bary.y)+vertices[3u*tri+1u].position*bary.x+vertices[3u*tri+2u].position*bary.y;
}
vec3 targetColor(Guide guide,uint lightIndex,vec2 bary) {
    if (guide.type>1u || lightIndex>=pc.lightCount || pc.lightPower<=0.0) return vec3(0);
    uint tri=lights[lightIndex].triangle; Vertex a=vertices[3u*tri];
    vec3 delta=lightPoint(lightIndex,bary)-guide.position; float d2=dot(delta,delta);
    if (d2<=0.000001) return vec3(0);
    vec3 wi=delta*inversesqrt(d2), n=dot(guide.normal,guide.view)>=0.0 ? guide.normal : -guide.normal;
    float noL=max(0.0,dot(n,wi)), noV=max(0.0,dot(n,guide.view)), lightCos=max(0.0,dot(a.normal,-wi));
    if (noL<=0.0 || noV<=0.0 || lightCos<=0.0) return vec3(0);
    vec3 f;
    if (guide.type==0u) f=guide.albedo/PI;
    else {
        float alpha=max(guide.roughness*guide.roughness,0.0001),a2=alpha*alpha;
        vec3 h=normalize(guide.view+wi); float noH=max(0.0,dot(n,h)),voH=max(0.0,dot(guide.view,h));
        vec3 F=guide.albedo+(1.0-guide.albedo)*pow(1.0-voH,5.0);
        f=F*ggxD(noH,a2)*smithG2(noV,noL,a2)/(4.0*noV*noL);
    }
    return f*materials[triangles[tri].material].emission*(noL*lightCos/d2);
}
Reservoir emptyReservoir() { return Reservoir(0u,vec2(0),0.0,0.0,0u,0u,0u); }
void consider(inout Reservoir combined,Reservoir source,Guide target,inout uint rng) {
    combined.M+=source.M;
    if (source.valid==0u || source.M==0u || source.W<=0.0) return;
    float weight=luminance(targetColor(target,source.light,source.bary))*source.W*float(source.M);
    if (weight<=0.0) return;
    combined.weightSum+=weight;
    if (randomValue(rng)*combined.weightSum<weight) { combined.light=source.light; combined.bary=source.bary; combined.valid=1u; }
}
bool compatible(Guide a,Guide b) {
    if (a.type>1u || b.type>1u || a.material!=b.material || a.surface!=b.surface || dot(a.normal,b.normal)<0.98 ||
        (dot(a.normal,a.view)>=0.0)!=(dot(b.normal,b.view)>=0.0)) return false;
    // 世界单位的容差随像素在该深度的投影足迹变化。
    float tolerance=max(0.04,4.0*length(a.position-pc.position)/float(pc.height));
    return length(a.position-b.position)<=tolerance;
}
bool previousPixel(Guide current,uint i,out uint previousIndex) {
    CameraPair pair=cameras[pc.cameraSlot];
    if (current.type>1u || pair.valid==0u) return false;
    vec3 d=current.position-pair.previous.position; float z=dot(d,pair.previous.forward);
    if (z<=0.05) return false;
    float aspect=float(pair.previous.width)/float(pair.previous.height);
    vec2 screen=vec2(dot(d,pair.previous.right)/(z*pair.previous.tanHalfFov*aspect),
                     dot(d,pair.previous.up)/(z*pair.previous.tanHalfFov));
    vec2 projected=vec2((screen.x+1.0)*0.5*float(pc.width),(1.0-screen.y)*0.5*float(pc.height));
    ivec2 pixel=ivec2(int(i%pc.width),int(i/pc.width));
    vec2 currentJitter=jitterFor(i,pc.frame); vec2 motion=projected-(vec2(pixel)+currentJitter);
    vec2 samplePosition=vec2(pixel)+currentJitter+motion; ivec2 estimate=ivec2(floor(samplePosition));
    if (any(lessThan(estimate,ivec2(0))) || any(greaterThanEqual(estimate,ivec2(pc.width,pc.height)))) return false;
    uint candidate=uint(estimate.y)*pc.width+uint(estimate.x);
    ivec2 reprojected=ivec2(round(samplePosition-jitterFor(candidate,pair.previous.frame)));
    if (any(lessThan(reprojected,ivec2(0))) || any(greaterThanEqual(reprojected,ivec2(pc.width,pc.height)))) return false;
    previousIndex=uint(reprojected.y)*pc.width+uint(reprojected.x);
    Guide oldGuide=guides[((pc.frame^1u)&1u)*pc.width*pc.height+previousIndex];
    return compatible(current,oldGuide);
}
