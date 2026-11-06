#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

const uint MAX_REUSE_SOURCES = 5u;

Reservoir combineReservoirs(Guide targetGuide, Reservoir sources[5], Guide sourceGuides[5],
                            uint sourceCount, uint pixel, uint randomStage) {
    float candidateWeights[5];
    float candidateTargets[5];
    float weightSum = 0.0;
    float totalMultiplicity = 0.0;
    uint selected = INVALID_INDEX;

    for (uint i = 0u; i < sourceCount; ++i) {
        candidateWeights[i] = 0.0;
        candidateTargets[i] = 0.0;
        Reservoir source = sources[i];
        if (source.statistics.w < 0.5 || source.identity.x >= lightCount() ||
            source.statistics.z <= 0.0 || source.selected.w <= 0.0 || source.statistics.y <= 0.0) continue;
        float targetAtDestination;
        float proposalArea;
        evaluateDirectAtGuide(targetGuide, source.identity.x, source.selected.xy,
                              targetAtDestination, proposalArea);
        if (targetAtDestination <= 0.0) continue;

        // Generalized balance heuristic over the actual source support. Each
        // source density is represented by p_hat_s(y)/Z_s; sources where the
        // selected light has zero target contribute zero support.
        float supportDenominator = 0.0;
        float sourceNumerator = 0.0;
        for (uint j = 0u; j < sourceCount; ++j) {
            Reservoir other = sources[j];
            if (other.statistics.w < 0.5 || other.statistics.z <= 0.0 || other.statistics.y <= 0.0) continue;
            float targetAtSource;
            float ignoredProposal;
            evaluateDirectAtGuide(sourceGuides[j], source.identity.x, source.selected.xy,
                                  targetAtSource, ignoredProposal);
            float representedDensity = other.statistics.z * targetAtSource / max(other.statistics.y, 1e-20);
            supportDenominator += representedDensity;
            if (j == i) sourceNumerator = representedDensity;
        }
        if (sourceNumerator <= 0.0 || supportDenominator <= 0.0) continue;
        float misWeight = sourceNumerator / supportDenominator;
        float candidateWeight = targetAtDestination * source.selected.w * misWeight;
        if (!(candidateWeight > 0.0) || isnan(candidateWeight) || isinf(candidateWeight)) continue;
        candidateWeights[i] = candidateWeight;
        candidateTargets[i] = targetAtDestination;
        weightSum += candidateWeight;
        totalMultiplicity += source.statistics.z;
        float choice = randomDimension(pixel, 0u, camera.frame.z, randomStage + i);
        if (choice * weightSum < candidateWeight) selected = i;
    }

    if (selected == INVALID_INDEX || weightSum <= 0.0 || candidateTargets[selected] <= 0.0) {
        return invalidReservoir();
    }
    Reservoir result = sources[selected];
    float normalization = weightSum / candidateTargets[selected];
    float multiplicity = min(totalMultiplicity, 32.0);
    result.selected.z = candidateTargets[selected];
    result.selected.w = normalization;
    result.statistics = vec4(normalization * multiplicity * candidateTargets[selected],
                             normalization * candidateTargets[selected], multiplicity, 1.0);
    result.identity.y = pixel;
    result.identity.z = targetGuide.identity.x;
    result.identity.w = targetGuide.identity.y;
    return result;
}

