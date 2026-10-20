// Shared by every Vulkan shader: one push-constant block per draw (128 bytes; 208 for the scene
// variants, built with SCENE: frames a temporal upscaler resolves)
layout(push_constant) uniform Draw {
    mat4 matrix;     // clip space (already Vulkan's: depth 0..1)
    vec4 color;
    int texMode;     // meshes: 0 replace, 1 add, 2 blend, 3 decal, 4 modulate, -1 untextured
    int alphaTex;    // GL_ALPHA texture: colour always comes from the fragment
    float alphaRef;  // alpha test (0 = off)
    float reactive;  // scene: > 0 alpha-blended (reactive = this x alpha), < 0 additive (x brightness)
    int texMode2;    // meshes: texture unit 1's environment over unit 0's result, -1 none
    int pad0, pad1, pad2;
    vec4 uv2;        // unit 1's coordinates: uv * uv2.xy + uv2.zw
#ifdef SCENE
    mat4 prevMatrix; // this draw's matrix last frame (unjittered, as `matrix`)
    vec4 motion;     // xy: the frame's jitter in clip units; zw: NDC -> render-target UV
#endif
} pc;
