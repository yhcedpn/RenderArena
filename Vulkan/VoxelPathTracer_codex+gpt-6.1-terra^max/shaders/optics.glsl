uint opticalIndex(uint phase,uint branch,uint pixel) { return (phase*2u+branch)*pc.width*pc.height+pixel; }
Guide opticalGuide(uint phase,uint branch,uint pixel) {
    uint i=phase*pc.width*pc.height+pixel; return branch==0u ? opticalGuides0[i] : opticalGuides1[i];
}
void writeOpticalGuide(uint phase,uint branch,uint pixel,Guide guide) {
    uint i=phase*pc.width*pc.height+pixel; if (branch==0u) opticalGuides0[i]=guide; else opticalGuides1[i]=guide;
}
void resetOptical(uint slot) {
    opticalPaths[slot].reserved=0u; opticalPaths[slot].chainLength=0u;
    opticalPaths[slot].chainHash=0x811c9dc5u; opticalPaths[slot].flags=0u; opticalPaths[slot].distance=0.0;
    uint pixel=slot/2u,branch=slot&1u; opticalMetadata[opticalIndex(pc.frame&1u,branch,pixel)].valid=0u;
}
void opticalEvent(uint slot,uint triangle,uint eventType,bool front) {
    OpticalPath path=opticalPaths[slot];
    path.chainHash=hashValue(path.chainHash^hashValue(triangles[triangle].surface*8u+eventType*2u+uint(front)));
    path.events[path.chainLength]=triangle*8u+eventType*2u+uint(front); path.chainLength++;
    if (eventType!=0u) path.flags|=1u;
    if (eventType==2u) path.flags|=2u;
    opticalPaths[slot]=path;
}
void resumeOptical(uint slot) {
    OpticalPath path=opticalPaths[slot]; uint triangle=path.reserved>>1u,front=path.reserved&1u;
    uint eventCode=triangle*8u+6u+front;
    path.events[path.chainLength]=eventCode; path.chainLength++;
    path.chainHash=hashValue(path.chainHash^hashValue(triangles[triangle].surface*8u+6u+front));
    path.flags|=8u; opticalPaths[slot]=path;
    opticalMetadata[opticalIndex(pc.frame&1u,slot&1u,slot/2u)].valid=0u;
}
void latchOptical(Path path,Hit hit,Material material,Vertex a,Vertex b,Vertex c,vec3 point) {
    OpticalPath chain=opticalPaths[path.slot]; if ((chain.flags&4u)!=0u) return;
    float w=1.0-hit.bary.x-hit.bary.y; vec2 uv=a.uv*w+b.uv*hit.bary.x+c.uv*hit.bary.y;
    float footprint=path.footprint+path.cone*hit.distance;
    float lod=max(0.0,log2(max(1.0,footprint*float(textureSize(baseTexture,0).x))));
    Guide guide; guide.position=point; guide.surface=triangles[hit.triangle].surface; guide.normal=a.normal;
    guide.material=triangles[hit.triangle].material; guide.albedo=textureLod(baseTexture,vec3(uv,float(guide.material)),lod).rgb;
    guide.roughness=textureLod(roughTexture,vec3(uv,float(guide.material)),lod).r; guide.view=-path.direction; guide.type=material.type;
    if ((chain.flags&(2u|8u))==0u) {
        // 将终端点、法线和观察方向依照反射面的逆序映射到虚拟空间。
        for (int k=int(chain.chainLength)-1;k>=0;--k) {
            uint eventCode=chain.events[k], eventType=(eventCode>>1u)&3u;
            if (eventType>=2u) continue;
            Vertex plane=vertices[3u*(eventCode>>3u)]; vec3 n=plane.normal;
            guide.position-=2.0*n*dot(guide.position-plane.position,n);
            guide.normal-=2.0*n*dot(guide.normal,n); guide.view-=2.0*n*dot(guide.view,n);
        }
    }
    opticalPaths[path.slot].reserved=hit.triangle*2u+uint(dot(path.direction,a.normal)<0.0);
    uint branch=path.slot&1u,pixel=path.slot/2u,phase=pc.frame&1u;
    writeOpticalGuide(phase,branch,pixel,guide);
    uint kind=(chain.flags&8u)!=0u ? ((chain.flags&2u)!=0u ? 5u : 4u) :
        ((chain.flags&2u)!=0u ? 3u : ((chain.flags&1u)!=0u ? 2u : 1u));
    OpticalMeta meta=OpticalMeta(chain.chainHash,chain.chainLength,kind,1u,chain.distance);
    uint historyIndex=opticalIndex(phase,branch,pixel);
    opticalMetadata[historyIndex]=meta;
    for (uint k=0u;k<chain.chainLength;++k) opticalChains[8u*historyIndex+k]=chain.events[k];
}
void latchOpticalMiss(Path path) {
    OpticalPath chain=opticalPaths[path.slot]; if ((chain.flags&4u)!=0u) return;
    vec3 direction=path.direction;
    if ((chain.flags&(2u|8u))==0u) for (int k=int(chain.chainLength)-1;k>=0;--k) {
        uint eventCode=chain.events[k],eventType=(eventCode>>1u)&3u;
        if (eventType>=2u) continue;
        vec3 n=vertices[3u*(eventCode>>3u)].normal; direction-=2.0*n*dot(direction,n);
    }
    Guide guide=Guide(normalize(direction),0xffffffffu,normalize(direction),0xffffffffu,vec3(1),0.0,-normalize(direction),5u);
    uint branch=path.slot&1u,pixel=path.slot/2u,phase=pc.frame&1u,index=opticalIndex(phase,branch,pixel);
    writeOpticalGuide(phase,branch,pixel,guide);
    uint kind=(chain.flags&8u)!=0u ? ((chain.flags&2u)!=0u ? 5u : 4u) :
        ((chain.flags&2u)!=0u ? 3u : ((chain.flags&1u)!=0u ? 2u : 1u));
    opticalMetadata[index]=OpticalMeta(chain.chainHash,chain.chainLength,kind,1u,chain.distance);
    for (uint k=0u;k<chain.chainLength;++k) opticalChains[8u*index+k]=chain.events[k];
}
