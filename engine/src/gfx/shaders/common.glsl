// Shared by every Vulkan shader: one push-constant block per draw (96 bytes; 176 for the scene
// variants, built with SCENE: frames a temporal upscaler resolves)
layout(push_constant) uniform Draw {
    mat4 matrix;     // clip space (already Vulkan's: depth 0..1)
    vec4 color;
    int texMode;     // meshes: 0 replace, 1 add, 2 blend, 3 decal, 4 modulate, -1 untextured
    int alphaTex;    // GL_ALPHA texture: colour always comes from the fragment
    float alphaRef;  // alpha test (0 = off)
    float reactive;  // scene: > 0 alpha-blended (reactive = this x alpha), < 0 additive (x brightness)
#ifdef SCENE
    mat4 prevMatrix; // this draw's matrix last frame (unjittered, as `matrix`)
    vec4 motion;     // xy: the frame's jitter in clip units; zw: NDC -> render-target UV
#endif
} pc;
