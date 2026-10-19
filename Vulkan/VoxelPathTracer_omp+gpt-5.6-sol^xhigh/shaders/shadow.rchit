#version 460
#extension GL_EXT_ray_tracing : require

hitAttributeEXT vec2 attributes;
layout(location = 1) rayPayloadInEXT uint visible;

void main() {
    visible = 0u;
}
