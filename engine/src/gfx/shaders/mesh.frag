#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
// Unlit surface emulating GL ES 1.1 texture environment modes (as gl_device.cpp's)
layout(set = 0, binding = 0) uniform sampler2D uTex;
layout(location = 0) in vec2 vUV;
layout(location = 1) in vec4 vColor;
layout(location = 0) out vec4 fragColor;
#ifdef SCENE
layout(location = 2) in vec4 vCur;
layout(location = 3) in vec4 vPrev;
layout(location = 1) out vec2 fragMotion;  // UV offset to this point last frame
layout(location = 2) out float fragReactive;
#endif
void main() {
    vec4 base = pc.color * vColor;
    vec4 c = base;
    if (pc.texMode >= 0) {
        vec4 t = texture(uTex, vUV);
        if (pc.alphaTex != 0) c = vec4(base.rgb, pc.texMode == 0 ? t.a : base.a * t.a);
        else if (pc.texMode == 0) c = t;
        else if (pc.texMode == 1) c = vec4(base.rgb + t.rgb, base.a * t.a);
        else if (pc.texMode == 2) c = vec4(base.rgb * (vec3(1.0) - t.rgb), base.a * t.a);
        else if (pc.texMode == 3) c = vec4(mix(base.rgb, t.rgb, t.a), base.a);
        else c = base * t;
    }
    if (pc.alphaRef > 0.0 && c.a < pc.alphaRef) discard;
    fragColor = c;
#ifdef SCENE
    fragMotion = (vPrev.xy / vPrev.w - vCur.xy / vCur.w) * pc.motion.zw;
    fragReactive = pc.reactive > 0.0 ? pc.reactive * clamp(c.a, 0.0, 1.0)
                                     : -pc.reactive * clamp(max(c.r, max(c.g, c.b)), 0.0, 1.0);
#endif
}
