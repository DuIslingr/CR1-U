// The model bridge's libraries, built once: cgltf 1.13 (MIT, Johannes Kuhlmann) reads glTF 2.0
// (.glb / .gltf) replacements; stb_image 2.30 (PNG / JPEG only) decodes their pictures and
// stb_image_write 1.16 encodes the exports' (both public domain / MIT, Sean Barrett)
#define CGLTF_IMPLEMENTATION
#include "cgltf/cgltf.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb/stb_image_write.h"
