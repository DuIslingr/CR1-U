#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;
layout(location = 0) out vec2 vUV;
layout(location = 1) out vec4 vColor;
#ifdef SCENE
layout(location = 3) in vec3 aPrevPos; // last frame's vertices (re-skinned meshes)
layout(location = 2) out vec4 vCur;
layout(location = 3) out vec4 vPrev;
#endif
void main() {
    vUV = aUV;
    vColor = aColor;
    gl_Position = pc.matrix * vec4(aPos, 1.0);
#ifdef SCENE
    vCur = gl_Position;
    vPrev = pc.prevMatrix * vec4(aPrevPos, 1.0);
    gl_Position.xy += pc.motion.xy * gl_Position.w;
#endif
}
