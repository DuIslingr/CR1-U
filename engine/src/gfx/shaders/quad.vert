#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 0) out vec2 vUV;
void main() {
    vUV = aUV;
    gl_Position = pc.matrix * vec4(aPos, 0.0, 1.0);
#ifdef SCENE
    gl_Position.xy += pc.motion.xy * gl_Position.w;
#endif
}
