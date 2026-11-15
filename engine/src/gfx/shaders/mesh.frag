#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
// Unlit surface emulating GL ES 1.1 texture environment modes (as gl_device.cpp's)
layout(set = 0, binding = 0) uniform sampler2D uTex;
layout(set = 1, binding = 0) uniform sampler2D uTex2; // texture unit 1 (white when none)
layout(location = 0) in vec2 vUV;
layout(location = 1) in vec4 vColor;
layout(location = 0) out vec4 fragColor;
#ifdef SCENE
layout(location = 2) in vec4 vCur;
layout(location = 3) in vec4 vPrev;
layout(location = 1) out vec2 fragMotion;  // UV offset to this point last frame
layout(location = 2) out float fragReactive;
#endif
vec4 texEnv(int mode, vec4 base, vec4 t) {
    if (mode == 0) return t;
    if (mode == 1) return vec4(base.rgb + t.rgb, base.a * t.a);
    if (mode == 2) return vec4(base.rgb * (vec3(1.0) - t.rgb), base.a * t.a);
    if (mode == 3) return vec4(mix(base.rgb, t.rgb, t.a), base.a);
    return base * t;
}
void main() {
    vec4 base = pc.noVertexColor != 0 ? pc.color : pc.color * vColor;
    vec4 c = base;
    if (pc.texMode >= 0) {
        vec4 t = texture(uTex, vUV);
        if (pc.alphaTex == 1) c = vec4(base.rgb, pc.texMode == 0 ? t.a : base.a * t.a);
        else c = texEnv(pc.texMode, base, t);
        if (pc.alphaTex == 2 && pc.texMode == 0) c.a = base.a; // (GL_RGB: replace takes the colour alone)
    }
    if (pc.texMode2 >= 0) c = texEnv(pc.texMode2, c, texture(uTex2, vUV * pc.uv2.xy + pc.uv2.zw));
    // GL ES 1's alpha test compares in 8-bit fixed point: a fragment within half a step of the reference passes. Exact
    // float made "alpha >= 1.0" (the 255 tests of a battle model's blended copy) fail at scattered pixels where a
    // GPU's interpolation lands a hair under 1.0 - Vieg's skin speckled with the sky through it (CRO-U #5)
    if (pc.alphaRef > 0.0 && c.a < pc.alphaRef - 0.5 / 255.0) discard;
    fragColor = c;
#ifdef SCENE
    fragMotion = (vPrev.xy / vPrev.w - vCur.xy / vCur.w) * pc.motion.zw;
    fragReactive = pc.reactive > 0.0 ? pc.reactive * clamp(c.a, 0.0, 1.0)
                                     : -pc.reactive * clamp(max(c.r, max(c.g, c.b)), 0.0, 1.0);
#endif
}
