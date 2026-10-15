#pragma once
// Model packages (.bin) and MCM_ model blocks — see docs/FORMATS.md.

#include <cstdint>
#include <string>
#include <vector>

#include "assets/animation.hpp"
#include "assets/texture.hpp"
#include "core/math.hpp"

namespace cr {

struct SkeletonNode {
    int parent = -1;
    float axes[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}; // unscaled basis vectors (columns)
    float translation[3] = {0, 0, 0};
    float scale[3] = {1, 1, 1};
};

struct Skeleton {
    std::vector<SkeletonNode> nodes;
    std::vector<Mat4> restWorld, restWorldInverse;
};

// Appearance record (see docs/FORMATS.md, "Appearance"). Codes are the original
// engine's; the renderer maps them to GL (GL ES 1.1 enum order).
struct Material {
    int id = -1;
    float diffuse[4] = {1, 1, 1, 1};   // colour 0: GL_DIFFUSE and the unlit glColor
    float ambient[4] = {1, 1, 1, 1};   // colour 1
    float emission[4] = {0, 0, 0, 0};  // colour 2
    float specular[4] = {0, 0, 0, 0};  // colour 3
    uint8_t shininess = 0;
    uint8_t alphaThreshold = 0;        // alpha test GEQUAL threshold/255, off when 0
    uint8_t blendSrc = 1, blendDst = 0;// ES1 factor order; (1,0) = opaque, blending off
    uint8_t shading = 1;
    uint8_t culling = 2;               // 0 none, 1 front, 2 back (verified: meshes are CCW-outward)
    int16_t drawOrder = 0;
    uint32_t properties = 0;           // bit3 depth test, bit4 depth write, bit7 polygon offset
    int textureRef = -1;               // MCT texture id, -1 = untextured
    uint8_t texBlend = 4;              // 0 replace, 1 add, 2 blend, 3 decal, 4 modulate
    uint8_t wrapS = 0, wrapT = 0;      // 0 repeat, 1 clamp
    uint8_t minFilter = 0, magFilter = 0; // 0 nearest, 1 linear, 2.. mipmap variants
    float uvTransform[15] = {};        // unit 0: UV transform (4 x vec3) + UV scale (vec3), as stored
    int textureRef2 = -1;              // unit 1 texture id (unused by most shipped materials)
    uint8_t texBlend2 = 4;
    uint32_t refGid = 0, tail = 0, unitWord[2] = {0, 0}; // the record's unknown words (animation binding?)

    bool blended() const { return !(blendSrc == 1 && blendDst == 0); }
    bool depthTest() const { return (properties & 8) != 0; }
    bool depthWrite() const { return (properties & 16) != 0; }
};

struct Mesh {
    std::vector<float> positions;    // xyz per render vertex, rest pose in skeleton space
    std::vector<float> uvs;          // uv per render vertex (zeros if the buffer has none)
    std::vector<uint8_t> colors;     // RGBA per render vertex (white if the buffer has none)
    std::vector<uint32_t> indices;   // triangle list
    int appearance = -1;             // index into Model::materials
    int texture = -1;                // index into Model::textures, or -1

    // Deformation inputs (see scene/pose.hpp)
    std::vector<uint32_t> posOf;      // render vertex -> position index
    std::vector<float> bindPositions; // xyz per position, in the attach node's local space
    int attachNode = -1;              // skeleton node this mesh's figure is attached to
    bool skinned = false;
    bool visible = true;             // initial visibility from the NodeDeformer (false: hidden helper)
    std::vector<int> boneNodes;               // skeleton node of each bone
    std::vector<uint8_t> influenceCount;      // per position, 0..4
    std::vector<uint8_t> influenceBone;       // 4 per position
    std::vector<float> influenceWeight;       // 4 per position
};

struct ModelStats {
    int vertexBuffers = 0, indexBuffers = 0, boneDeformers = 0, sections = 0;
    int triangles = 0, unsupportedPrims = 0, unpairedSkins = 0, figureBound = 0;
    int texById = 0, texByIndex = 0, texMissing = 0, untextured = 0;
    int recordCounts[12] = {};
};

struct Model {
    std::vector<Mesh> meshes;
    std::vector<Image> textures;
    std::vector<int> textureIds;     // MCT header id of each texture (what materials reference)
    std::vector<int> appearanceIds;
    std::vector<Material> materials; // parallel to appearanceIds
    Skeleton skeleton;
    std::vector<Action> actions;     // animations stored in the package
    std::vector<std::pair<int, int>> objectNodes; // NodeDeformer: object id -> skeleton node (btlcamera_getpos)
    struct RecordTag { int section, id, user; };
    std::vector<RecordTag> records;  // every record's section, id and user word (mceObject3D_findObject3D)
    std::vector<std::vector<uint8_t>> lightRecords; // section 3 records as stored (header included)
    float boundsMin[3] = {0, 0, 0}, boundsMax[3] = {0, 0, 0};
    ModelStats stats;
};

// True if `file` is a model package (offset table whose first entry is an MCM_ block).
bool isModelPackage(const std::vector<uint8_t>& file);

// Parses a .bin package: the MCM_ model plus its MCT_ textures and MCA_ animations.
// On failure returns false and sets *error.
bool loadModelPackage(const std::vector<uint8_t>& file, Model& out, std::string* error = nullptr,
                      bool decodeTextures = true);

} // namespace cr
