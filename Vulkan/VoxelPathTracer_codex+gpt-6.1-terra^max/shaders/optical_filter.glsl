bool sameOpticalChain(OpticalMeta a,uint aIndex,OpticalMeta b,uint bIndex) {
    if (a.valid==0u || b.valid==0u || a.kind!=b.kind || a.chainHash!=b.chainHash || a.chainLength!=b.chainLength) return false;
    if (a.kind>=4u) {
        uint n=pc.width*pc.height, phaseA=(aIndex/n)/2u, phaseB=(bIndex/n)/2u;
        Guide originA=guides[phaseA*n+aIndex%n],originB=guides[phaseB*n+bIndex%n];
        if ((originA.type<=1u || originB.type<=1u) && !compatible(originA,originB)) return false;
    }
    for (uint k=0u;k<a.chainLength;++k) {
        uint x=opticalChains[8u*aIndex+k],y=opticalChains[8u*bIndex+k];
        if ((x&7u)!=(y&7u) || triangles[x>>3u].surface!=triangles[y>>3u].surface) return false;
    }
    return true;
}
bool opticalCompatible(Guide a,OpticalMeta ma,uint aIndex,Guide b,OpticalMeta mb,uint bIndex) {
    if (!sameOpticalChain(ma,aIndex,mb,bIndex) || a.type!=b.type || a.material!=b.material || a.surface!=b.surface) return false;
    if (a.type==5u) return dot(a.position,b.position)>0.999 && abs(ma.distance-mb.distance)<0.2;
    if (dot(a.normal,b.normal)<0.98 || (dot(a.normal,a.view)>=0.0)!=(dot(b.normal,b.view)>=0.0)) return false;
    float tolerance=max(0.04,4.0*length(a.position-pc.position)/float(pc.height));
    return length(a.position-b.position)<=tolerance && abs(ma.distance-mb.distance)<=2.0*tolerance;
}
bool previousOptical(Guide current,OpticalMeta meta,uint i,uint branch,out uint previousIndex) {
    CameraPair pair=cameras[pc.cameraSlot]; if (pair.valid==0u || meta.valid==0u) return false;
    ivec2 pixel=ivec2(int(i%pc.width),int(i/pc.width)); vec2 currentJitter=jitterFor(i,pc.frame); vec2 samplePosition;
    if (meta.kind>=3u) {
        // 折射映射不在移动相机间复用；静止时补偿两帧子像素采样位置。
        if (any(notEqual(pair.current.position,pair.previous.position)) || any(notEqual(pair.current.forward,pair.previous.forward)) ||
            pair.current.tanHalfFov!=pair.previous.tanHalfFov || pair.current.width!=pair.previous.width || pair.current.height!=pair.previous.height) return false;
        samplePosition=vec2(pixel)+currentJitter;
    } else {
        vec3 d=current.type==5u ? current.position : current.position-pair.previous.position; float z=dot(d,pair.previous.forward);
        if (z<=0.05) return false;
        float aspect=float(pair.previous.width)/float(pair.previous.height);
        vec2 screen=vec2(dot(d,pair.previous.right)/(z*pair.previous.tanHalfFov*aspect),
                         dot(d,pair.previous.up)/(z*pair.previous.tanHalfFov));
        vec2 projected=vec2((screen.x+1.0)*0.5*float(pc.width),(1.0-screen.y)*0.5*float(pc.height));
        vec2 motion=projected-(vec2(pixel)+currentJitter); samplePosition=vec2(pixel)+currentJitter+motion;
    }
    ivec2 estimate=ivec2(floor(samplePosition));
    if (any(lessThan(estimate,ivec2(0))) || any(greaterThanEqual(estimate,ivec2(pc.width,pc.height)))) return false;
    uint guess=uint(estimate.y)*pc.width+uint(estimate.x);
    ivec2 q=ivec2(round(samplePosition-jitterFor(guess,pair.previous.frame)));
    if (any(lessThan(q,ivec2(0))) || any(greaterThanEqual(q,ivec2(pc.width,pc.height)))) return false;
    previousIndex=uint(q.y)*pc.width+uint(q.x); uint previousPhase=(pc.frame^1u)&1u;
    Guide oldGuide=opticalGuide(previousPhase,branch,previousIndex);
    OpticalMeta oldMeta=opticalMetadata[opticalIndex(previousPhase,branch,previousIndex)];
    return opticalCompatible(current,meta,opticalIndex(pc.frame&1u,branch,i),oldGuide,oldMeta,opticalIndex(previousPhase,branch,previousIndex));
}
