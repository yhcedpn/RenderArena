#ifndef RAE_GLOBALS_GLSL
#define RAE_GLOBALS_GLSL

#include "common.glsl"

layout(std430, set = 0, binding = 0) readonly buffer GlobalsBuffer {
    Globals items[2];
} globals;

#endif
