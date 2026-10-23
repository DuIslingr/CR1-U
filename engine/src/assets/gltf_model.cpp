#include "assets/gltf_model.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

#include <SDL3/SDL.h>

#include "cgltf/cgltf.h"
#include "scene/pose.hpp"
#include "stb/stb_image.h"
#include "stb/stb_image_write.h"

namespace cr {

namespace {

constexpr float kFps = 20.0f; // the game's frames (core/timing.hpp's kFramesPerSecond)

bool fail(std::string* error, const std::string& what) {
    if (error) *error = what;
    return false;
}

// ---- Writing ----

std::string num(float v) {
    if (!std::isfinite(v)) v = 0;
    char b[32];
    std::snprintf(b, sizeof b, "%.9g", double(v));
    return b;
}

std::string nums(const float* v, int n) {
    std::string s = "[";
    for (int i = 0; i < n; ++i) s += (i ? "," : "") + num(v[i]);
    return s + "]";
}

std::string quoted(const std::string& s) {
    std::string o = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') o += '\\', o += c;
        else if (static_cast<unsigned char>(c) < 0x20) o += ' ';
        else o += c;
    }
    return o + "\"";
}

std::string joined(const std::vector<std::string>& v) {
    std::string s;
    for (size_t i = 0; i < v.size(); ++i) s += (i ? "," : "") + v[i];
    return s;
}

// The binary chunk and the views / accessors over it
struct Writer {
    std::vector<uint8_t> bin;
    std::vector<std::string> views, accessors;

    int view(const void* data, size_t size, int target) {
        while (bin.size() % 4) bin.push_back(0);
        const size_t at = bin.size();
        bin.insert(bin.end(), static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
        std::string v = "{\"buffer\":0,\"byteOffset\":" + std::to_string(at) + ",\"byteLength\":" + std::to_string(size);
        if (target) v += ",\"target\":" + std::to_string(target);
        views.push_back(v + "}");
        return int(views.size()) - 1;
    }
    // componentType 5126 float, 5125 uint, 5123 ushort, 5121 ubyte; type SCALAR / VEC2..4 / MAT4
    int accessor(const void* data, size_t count, int comps, int componentType, const char* type, int target,
                 bool normalized = false, const float* mn = nullptr, const float* mx = nullptr) {
        const size_t size = componentType == 5126 || componentType == 5125 ? 4 : componentType == 5123 ? 2 : 1;
        const int v = view(data, count * size_t(comps) * size, target);
        std::string a = "{\"bufferView\":" + std::to_string(v) + ",\"componentType\":" + std::to_string(componentType) +
                        ",\"count\":" + std::to_string(count) + ",\"type\":\"" + type + "\"";
        if (normalized) a += ",\"normalized\":true";
        if (mn && mx) a += ",\"min\":" + nums(mn, comps) + ",\"max\":" + nums(mx, comps);
        accessors.push_back(a + "}");
        return int(accessors.size()) - 1;
    }
};

// A rotation's basis columns as a unit quaternion (x, y, z, w)
void quaternion(const float c[3][3], float q[4]) {
    const float m00 = c[0][0], m11 = c[1][1], m22 = c[2][2]; // R[r][k] = c[k][r]
    const float tr = m00 + m11 + m22;
    if (tr > 0) {
        const float s = std::sqrt(tr + 1) * 2;
        q[3] = 0.25f * s, q[0] = (c[1][2] - c[2][1]) / s, q[1] = (c[2][0] - c[0][2]) / s, q[2] = (c[0][1] - c[1][0]) / s;
    } else if (m00 > m11 && m00 > m22) {
        const float s = std::sqrt(1 + m00 - m11 - m22) * 2;
        q[3] = (c[1][2] - c[2][1]) / s, q[0] = 0.25f * s, q[1] = (c[1][0] + c[0][1]) / s, q[2] = (c[2][0] + c[0][2]) / s;
    } else if (m11 > m22) {
        const float s = std::sqrt(1 + m11 - m00 - m22) * 2;
        q[3] = (c[2][0] - c[0][2]) / s, q[0] = (c[1][0] + c[0][1]) / s, q[1] = 0.25f * s, q[2] = (c[2][1] + c[1][2]) / s;
    } else {
        const float s = std::sqrt(1 + m22 - m00 - m11) * 2;
        q[3] = (c[0][1] - c[1][0]) / s, q[0] = (c[2][0] + c[0][2]) / s, q[1] = (c[2][1] + c[1][2]) / s, q[2] = 0.25f * s;
    }
    const float l = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    for (int k = 0; k < 4; ++k) q[k] = l > 0 ? q[k] / l : (k == 3 ? 1.0f : 0.0f);
}

// A pose as glTF's TRS: a mirrored basis (determinant < 0) flips its first column into the scale
struct Trs {
    float t[3], r[4], s[3];
};
Trs trsOf(const NodePose& p) {
    float c[3][3];
    std::memcpy(c, p.cols, sizeof c);
    Trs o;
    std::memcpy(o.t, p.t, sizeof o.t);
    std::memcpy(o.s, p.s, sizeof o.s);
    const float det = c[0][0] * (c[1][1] * c[2][2] - c[2][1] * c[1][2]) - c[1][0] * (c[0][1] * c[2][2] - c[2][1] * c[0][2]) +
                      c[2][0] * (c[0][1] * c[1][2] - c[1][1] * c[0][2]);
    if (det < 0) {
        for (int k = 0; k < 3; ++k) c[0][k] = -c[0][k];
        o.s[0] = -o.s[0];
    }
    quaternion(c, o.r);
    return o;
}

void pngOut(void* ctx, void* data, int size) {
    auto* v = static_cast<std::vector<uint8_t>*>(ctx);
    v->insert(v->end(), static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size);
}

// ---- Reading ----

// The skeleton node a glTF node is ("node_<i>"), else -1
int skeletonIndex(const cgltf_node* n, size_t nodes) {
    int i = -1, used = 0;
    if (n && n->name && std::sscanf(n->name, "node_%d%n", &i, &used) == 1 && n->name[used] == 0 && i >= 0 && size_t(i) < nodes) return i;
    return -1;
}

// Node `n`'s matching skeleton node, else its nearest ancestor's; -1 none. With `rel`, the
// transform from that node's space to n's own (the locals between, n's included)
int skeletonNodeOf(const cgltf_node* n, size_t nodes, Mat4* rel = nullptr) {
    Mat4 m;
    for (const cgltf_node* k = n; k; k = k->parent) {
        if (const int i = skeletonIndex(k, nodes); i >= 0) {
            if (rel) *rel = m;
            return i;
        }
        float l[16];
        cgltf_node_transform_local(k, l);
        Mat4 local;
        std::memcpy(local.m, l, sizeof l);
        m = local * m;
    }
    return -1;
}

// A rest pose that can be undone (an effect's node can rest at scale 0)
bool invertible(const Mat4& m) {
    const float* a = m.m;
    const float det = a[0] * (a[5] * a[10] - a[9] * a[6]) - a[4] * (a[1] * a[10] - a[9] * a[2]) + a[8] * (a[1] * a[6] - a[5] * a[2]);
    return std::fabs(det) > 1e-12f;
}

Mat4 matrixOf(const float m[16]) {
    Mat4 r;
    std::memcpy(r.m, m, sizeof r.m);
    return r;
}

// "cr_mat_<i>" -> i, else -1
int appearanceOf(const cgltf_material* m) {
    int i = -1;
    if (m && m->name && std::sscanf(m->name, "cr_mat_%d", &i) == 1) return i;
    return -1;
}

// An image's bytes: a buffer view's, a data URI's or a file beside the glTF
bool imageBytes(const cgltf_image* img, const std::string& dir, std::vector<uint8_t>& out) {
    if (img->buffer_view && img->buffer_view->buffer && img->buffer_view->buffer->data) {
        const uint8_t* p = static_cast<const uint8_t*>(img->buffer_view->buffer->data) + img->buffer_view->offset;
        out.assign(p, p + img->buffer_view->size);
        return true;
    }
    if (!img->uri) return false;
    if (std::strncmp(img->uri, "data:", 5) == 0) {
        const char* comma = std::strchr(img->uri, ',');
        if (!comma) return false;
        size_t len = std::strlen(comma + 1), pad = 0;
        while (len > 0 && comma[len] == '=') --len, ++pad; // (comma[len] is the text's last character)
        const size_t size = (len + pad) / 4 * 3 - pad;
        void* data = nullptr;
        cgltf_options opt{};
        if (cgltf_load_buffer_base64(&opt, size, comma + 1, &data) != cgltf_result_success) return false;
        out.assign(static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size);
        std::free(data);
        return true;
    }
    std::string uri = img->uri;
    cgltf_decode_uri(uri.data());
    uri.resize(std::strlen(uri.c_str()));
    size_t size = 0;
    void* data = SDL_LoadFile((dir + uri).c_str(), &size);
    if (!data) return false;
    out.assign(static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size);
    SDL_free(data);
    return true;
}

} // namespace

bool exportGlb(const Model& model, const std::vector<Action>& actions, const std::string& path, std::string* error,
               GltfExportStats* stats) {
    GltfExportStats st;
    Writer w;
    const Skeleton& sk = model.skeleton;
    const size_t n = sk.nodes.size();
    const bool skinned = n > 0 && sk.restWorld.size() == n && sk.restWorldInverse.size() == n;

    // The skeleton's nodes at rest
    std::vector<NodePose> rest;
    if (skinned) computeLocalPoses(sk, nullptr, 0, rest);
    std::vector<std::vector<int>> children(n);
    std::vector<int> roots;
    for (size_t i = 0; i < n; ++i) {
        const int p = sk.nodes[i].parent;
        if (p >= 0 && size_t(p) < n && size_t(p) != i) children[size_t(p)].push_back(int(i));
        else roots.push_back(int(i));
    }
    std::vector<std::string> nodes(skinned ? n : 0); // (the skeleton's first: filled in once the meshes are)

    // The pictures
    std::vector<std::string> images, textures;
    for (size_t t = 0; t < model.textures.size(); ++t) {
        const Image& img = model.textures[t];
        std::vector<uint8_t> png;
        if (img.width > 0 && img.height > 0 && img.rgba.size() >= size_t(img.width) * img.height * 4)
            stbi_write_png_to_func(pngOut, &png, img.width, img.height, 4, img.rgba.data(), img.width * 4);
        if (png.empty()) { // (a placeholder keeps the indices)
            const uint8_t white[4] = {255, 255, 255, 255};
            stbi_write_png_to_func(pngOut, &png, 1, 1, 4, white, 4);
        }
        const int v = w.view(png.data(), png.size(), 0);
        images.push_back("{\"name\":\"tex_" + std::to_string(t) + "\",\"bufferView\":" + std::to_string(v) + ",\"mimeType\":\"image/png\"}");
        textures.push_back("{\"source\":" + std::to_string(t) + ",\"sampler\":0}");
        ++st.images;
    }

    // One material per appearance (and texture, should two meshes differ in it)
    std::vector<std::string> materials;
    std::map<std::pair<int, int>, int> materialOf;
    auto material = [&](int appearance, int texture) {
        const auto key = std::make_pair(appearance, texture);
        if (auto it = materialOf.find(key); it != materialOf.end()) return it->second;
        const Material def;
        const Material& m = appearance >= 0 && size_t(appearance) < model.materials.size() ? model.materials[size_t(appearance)] : def;
        std::string name = appearance >= 0 ? "cr_mat_" + std::to_string(appearance) : "cr_none";
        bool again = false;
        for (const auto& [k, v] : materialOf) again |= k.first == appearance;
        if (again) name += "_t" + std::to_string(texture); // (still "cr_mat_<i>...": the same appearance)
        const float colour[4] = {1, 1, 1, m.diffuse[3] > 0.004f ? m.diffuse[3] : 1.0f};
        std::string s = "{\"name\":" + quoted(name) + ",\"pbrMetallicRoughness\":{\"baseColorFactor\":" + nums(colour, 4);
        if (texture >= 0 && size_t(texture) < model.textures.size()) s += ",\"baseColorTexture\":{\"index\":" + std::to_string(texture) + "}";
        s += ",\"metallicFactor\":0,\"roughnessFactor\":1}";
        if (m.blended()) s += ",\"alphaMode\":\"BLEND\"";
        else if (m.alphaThreshold > 0) s += ",\"alphaMode\":\"MASK\",\"alphaCutoff\":" + num(float(m.alphaThreshold) / 255.0f);
        if (m.culling == 0) s += ",\"doubleSided\":true";
        s += ",\"extensions\":{\"KHR_materials_unlit\":{}}}"; // (the game lights nothing: its colours are as painted)
        materials.push_back(s);
        materialOf[key] = int(materials.size()) - 1;
        return int(materials.size()) - 1;
    };

    // The meshes: skinned ones in the rest pose, skinned to every node (glTF: weights summing to
    // 1); rigid ones as their node's child in its space, as the game keeps them (an effect's node
    // can rest at scale 0)
    std::vector<std::string> meshes, sceneNodes;
    for (int r : roots) sceneNodes.push_back(std::to_string(r));
    const std::vector<Mat4> restWorld = skinned ? sk.restWorld : std::vector<Mat4>();
    for (size_t mi = 0; mi < model.meshes.size(); ++mi) {
        const Mesh& mesh = model.meshes[mi];
        const size_t nv = mesh.posOf.size();
        if (!nv || mesh.indices.empty()) continue;
        const bool rigid = skinned && !mesh.skinned && mesh.attachNode >= 0 && size_t(mesh.attachNode) < n;
        std::vector<float> pos;
        if (rigid) {
            pos.resize(nv * 3);
            for (size_t v = 0; v < nv; ++v)
                if (mesh.posOf[v] * 3 + 2 < mesh.bindPositions.size()) std::memcpy(&pos[v * 3], &mesh.bindPositions[mesh.posOf[v] * 3], 12);
        } else {
            skinMesh(model, mesh, restWorld, pos);
        }
        float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
        for (size_t v = 0; v < nv; ++v)
            for (int c = 0; c < 3; ++c) mn[c] = std::min(mn[c], pos[v * 3 + c]), mx[c] = std::max(mx[c], pos[v * 3 + c]);
        std::string attrs = "\"POSITION\":" + std::to_string(w.accessor(pos.data(), nv, 3, 5126, "VEC3", 34962, false, mn, mx));
        if (mesh.uvs.size() >= nv * 2) attrs += ",\"TEXCOORD_0\":" + std::to_string(w.accessor(mesh.uvs.data(), nv, 2, 5126, "VEC2", 34962));
        if (mesh.colors.size() >= nv * 4)
            attrs += ",\"COLOR_0\":" + std::to_string(w.accessor(mesh.colors.data(), nv, 4, 5121, "VEC4", 34962, true));
        if (skinned && !rigid) {
            std::vector<uint16_t> joints(nv * 4, 0);
            std::vector<float> weights(nv * 4, 0);
            const int attach = mesh.attachNode >= 0 && size_t(mesh.attachNode) < n ? mesh.attachNode : 0;
            for (size_t v = 0; v < nv; ++v) {
                const uint32_t p = mesh.posOf[v];
                const int k = mesh.skinned && p < mesh.influenceCount.size() ? mesh.influenceCount[p] : 0;
                float sum = 0;
                for (int i = 0; i < k && i < 4; ++i) {
                    const int b = mesh.influenceBone[p * 4 + size_t(i)];
                    const int node = b < int(mesh.boneNodes.size()) ? mesh.boneNodes[size_t(b)] : attach;
                    joints[v * 4 + size_t(i)] = uint16_t(node >= 0 && size_t(node) < n ? node : attach);
                    weights[v * 4 + size_t(i)] = std::max(0.0f, mesh.influenceWeight[p * 4 + size_t(i)]);
                    sum += weights[v * 4 + size_t(i)];
                }
                if (sum <= 1e-6f) {
                    joints[v * 4] = uint16_t(attach), weights[v * 4] = 1;
                    for (int i = 1; i < 4; ++i) joints[v * 4 + size_t(i)] = 0, weights[v * 4 + size_t(i)] = 0;
                } else {
                    for (int i = 0; i < 4; ++i) weights[v * 4 + size_t(i)] /= sum;
                }
            }
            attrs += ",\"JOINTS_0\":" + std::to_string(w.accessor(joints.data(), nv, 4, 5123, "VEC4", 34962));
            attrs += ",\"WEIGHTS_0\":" + std::to_string(w.accessor(weights.data(), nv, 4, 5126, "VEC4", 34962));
        }
        const int idx = w.accessor(mesh.indices.data(), mesh.indices.size(), 1, 5125, "SCALAR", 34963);
        const int mat = material(mesh.appearance, mesh.texture);
        const std::string name = "mesh_" + std::to_string(mi) + (mesh.visible ? "" : "_hidden");
        meshes.push_back("{\"name\":\"" + name + "\",\"primitives\":[{\"attributes\":{" + attrs + "},\"indices\":" + std::to_string(idx) +
                         ",\"material\":" + std::to_string(mat) + "}]}");
        nodes.push_back("{\"name\":\"" + name + "\",\"mesh\":" + std::to_string(meshes.size() - 1) + (skinned && !rigid ? ",\"skin\":0" : "") + "}");
        if (rigid) children[size_t(mesh.attachNode)].push_back(int(nodes.size()) - 1);
        else sceneNodes.push_back(std::to_string(nodes.size() - 1));
        ++st.meshes;
        st.vertices += int(nv);
        st.triangles += int(mesh.indices.size() / 3);
    }
    if (meshes.empty()) return fail(error, "no meshes");
    for (size_t i = 0; i < n && skinned; ++i) {
        const Trs t = trsOf(rest[i]);
        std::string s = "{\"name\":\"node_" + std::to_string(i) + "\",\"translation\":" + nums(t.t, 3) + ",\"rotation\":" + nums(t.r, 4) +
                        ",\"scale\":" + nums(t.s, 3);
        if (!children[i].empty()) {
            std::vector<std::string> c;
            for (int k : children[i]) c.push_back(std::to_string(k));
            s += ",\"children\":[" + joined(c) + "]";
        }
        nodes[i] = s + "}";
    }

    std::string skins;
    if (skinned) {
        std::vector<float> ibm(n * 16);
        for (size_t i = 0; i < n; ++i) std::memcpy(&ibm[i * 16], sk.restWorldInverse[i].m, 16 * sizeof(float));
        std::vector<std::string> j;
        for (size_t i = 0; i < n; ++i) j.push_back(std::to_string(i));
        skins = "[{\"joints\":[" + joined(j) + "],\"inverseBindMatrices\":" + std::to_string(w.accessor(ibm.data(), n, 16, 5126, "MAT4", 0)) +
                (roots.size() == 1 ? ",\"skeleton\":" + std::to_string(roots[0]) : std::string()) + "}]";
    }

    // The motions: each frame's local transforms; only channels that leave the rest pose
    std::vector<std::string> animations;
    for (size_t ai = 0; ai < actions.size() && skinned; ++ai) {
        const Action& a = actions[ai];
        if (a.segments.empty() || a.segments.size() > n) continue;
        const int frames = std::max(2, int(std::ceil(a.duration)) + 1);
        std::vector<std::vector<Trs>> poses(static_cast<size_t>(frames));
        std::vector<NodePose> p;
        for (int f = 0; f < frames; ++f) {
            computeLocalPoses(sk, &a, std::min(float(f), a.duration), p);
            for (size_t i = 0; i < n; ++i) poses[size_t(f)].push_back(trsOf(p[i]));
        }
        std::vector<float> times(static_cast<size_t>(frames));
        for (int f = 0; f < frames; ++f) times[size_t(f)] = float(f) / kFps;
        int input = -1;
        std::vector<std::string> samplers, channels;
        std::vector<Trs> restT(n);
        for (size_t i = 0; i < n; ++i) restT[i] = trsOf(rest[i]);
        const Trs* restTrs = restT.data();
        for (size_t i = 0; i < a.segments.size(); ++i) {
            for (int kind = 0; kind < 3; ++kind) { // translation, rotation, scale
                const int comps = kind == 1 ? 4 : 3;
                std::vector<float> out(size_t(frames) * size_t(comps));
                bool moves = false;
                for (int f = 0; f < frames; ++f) {
                    Trs& t = poses[size_t(f)][i];
                    if (kind == 1 && f > 0) { // (the short way round from the frame before)
                        const float* q0 = &out[size_t(f - 1) * 4];
                        if (q0[0] * t.r[0] + q0[1] * t.r[1] + q0[2] * t.r[2] + q0[3] * t.r[3] < 0)
                            for (float& c : t.r) c = -c;
                    }
                    const float* v = kind == 0 ? t.t : kind == 1 ? t.r : t.s;
                    const float* rv = kind == 0 ? restTrs[i].t : kind == 1 ? restTrs[i].r : restTrs[i].s;
                    for (int c = 0; c < comps; ++c) {
                        out[size_t(f) * size_t(comps) + size_t(c)] = v[c];
                        if (std::fabs(v[c] - rv[c]) > 1e-4f && !(kind == 1 && std::fabs(std::fabs(v[c]) - std::fabs(rv[c])) < 1e-4f))
                            moves = true;
                    }
                }
                if (!moves) continue;
                if (input < 0) {
                    const float mn = 0, mx = times.back();
                    input = w.accessor(times.data(), times.size(), 1, 5126, "SCALAR", 0, false, &mn, &mx);
                }
                const int output = w.accessor(out.data(), size_t(frames), comps, 5126, kind == 1 ? "VEC4" : "VEC3", 0);
                samplers.push_back("{\"input\":" + std::to_string(input) + ",\"output\":" + std::to_string(output) + ",\"interpolation\":\"LINEAR\"}");
                channels.push_back("{\"sampler\":" + std::to_string(samplers.size() - 1) + ",\"target\":{\"node\":" + std::to_string(i) +
                                   ",\"path\":\"" + (kind == 0 ? "translation" : kind == 1 ? "rotation" : "scale") + "\"}}");
            }
        }
        if (channels.empty()) continue;
        st.channels += int(channels.size());
        animations.push_back("{\"name\":" + quoted(a.name.empty() ? "action " + std::to_string(ai + 1) : a.name) + ",\"samplers\":[" +
                             joined(samplers) + "],\"channels\":[" + joined(channels) + "]}");
        ++st.animations;
    }

    while (w.bin.size() % 4) w.bin.push_back(0);
    std::string json = "{\"asset\":{\"version\":\"2.0\",\"generator\":\"Chaos Rings port model bridge\"}";
    json += ",\"extensionsUsed\":[\"KHR_materials_unlit\"],\"scene\":0,\"scenes\":[{\"nodes\":[" + joined(sceneNodes) + "]}]";
    json += ",\"nodes\":[" + joined(nodes) + "],\"meshes\":[" + joined(meshes) + "]";
    if (!materials.empty()) json += ",\"materials\":[" + joined(materials) + "]";
    if (!images.empty())
        json += ",\"images\":[" + joined(images) + "],\"textures\":[" + joined(textures) +
                "],\"samplers\":[{\"magFilter\":9729,\"minFilter\":9987}]";
    if (!skins.empty()) json += ",\"skins\":" + skins;
    if (!animations.empty()) json += ",\"animations\":[" + joined(animations) + "]";
    json += ",\"accessors\":[" + joined(w.accessors) + "],\"bufferViews\":[" + joined(w.views) + "]";
    json += ",\"buffers\":[{\"byteLength\":" + std::to_string(w.bin.size()) + "}]}";
    while (json.size() % 4) json += ' ';

    std::vector<uint8_t> glb;
    auto u32 = [&](uint32_t v) { for (int k = 0; k < 4; ++k) glb.push_back(uint8_t(v >> (8 * k))); };
    u32(0x46546C67), u32(2), u32(uint32_t(12 + 8 + json.size() + 8 + w.bin.size()));
    u32(uint32_t(json.size())), u32(0x4E4F534A);
    glb.insert(glb.end(), json.begin(), json.end());
    u32(uint32_t(w.bin.size())), u32(0x004E4942);
    glb.insert(glb.end(), w.bin.begin(), w.bin.end());
    if (!SDL_SaveFile(path.c_str(), glb.data(), glb.size())) return fail(error, std::string("can't write ") + path + ": " + SDL_GetError());
    st.bytes = glb.size();
    if (stats) *stats = st;
    return true;
}

bool applyGltfReplacement(Model& model, const std::string& path, std::string* error, GltfImportStats* stats) {
    cgltf_options opt{};
    cgltf_data* data = nullptr;
    if (cgltf_parse_file(&opt, path.c_str(), &data) != cgltf_result_success) return fail(error, "not a glTF file");
    struct Free {
        cgltf_data* d;
        ~Free() { cgltf_free(d); }
    } freeIt{data};
    if (cgltf_load_buffers(&opt, data, path.c_str()) != cgltf_result_success) return fail(error, "its buffers can't be read");
    const std::string dir = path.substr(0, path.find_last_of("/\\") + 1);

    GltfImportStats st;
    const Skeleton& sk = model.skeleton;
    const size_t n = sk.nodes.size();
    const bool skeleton = n > 0 && sk.restWorld.size() == n && sk.restWorldInverse.size() == n;
    // The pictures the replacement brings (any image but the export's "tex_<k>")
    std::map<const cgltf_image*, int> picture;
    std::vector<Image> newTextures;
    auto textureFor = [&](const cgltf_material* m, int appearance) {
        const cgltf_texture* t = m && m->has_pbr_metallic_roughness ? m->pbr_metallic_roughness.base_color_texture.texture : nullptr;
        const cgltf_image* img = t ? t->image : nullptr;
        if (img) {
            int k = -1, used = 0;
            if (img->name && std::sscanf(img->name, "tex_%d%n", &k, &used) == 1 && img->name[used] == 0 && k >= 0 &&
                size_t(k) < model.textures.size())
                return k;
            if (auto it = picture.find(img); it != picture.end()) return it->second;
            std::vector<uint8_t> bytes;
            int wdt = 0, hgt = 0, ch = 0;
            uint8_t* px = imageBytes(img, dir, bytes) ? stbi_load_from_memory(bytes.data(), int(bytes.size()), &wdt, &hgt, &ch, 4) : nullptr;
            if (px) {
                Image im;
                im.width = wdt, im.height = hgt, im.mipmaps = true;
                im.rgba.assign(px, px + size_t(wdt) * size_t(hgt) * 4);
                stbi_image_free(px);
                newTextures.push_back(std::move(im));
                const int index = int(model.textures.size() + newTextures.size()) - 1;
                picture[img] = index;
                return index;
            }
            picture[img] = -1;
        }
        // the appearance's own (as the package's meshes found it)
        for (const Mesh& om : model.meshes)
            if (om.appearance == appearance) return om.texture;
        return -1;
    };

    std::vector<Mesh> meshes;
    std::vector<Material> extraMaterials;
    for (size_t ni = 0; ni < data->nodes_count; ++ni) {
        const cgltf_node* node = &data->nodes[ni];
        if (!node->mesh) continue;
        float nodeWorld[16];
        cgltf_node_transform_world(node, nodeWorld);
        const Mat4 world = matrixOf(nodeWorld);
        Mat4 rel; // (a mesh without a skin rides on its nearest skeleton node: rigid, in that node's space)
        const int carrier = skeleton ? skeletonNodeOf(node, n, &rel) : -1;
        const int nodeBone = std::max(0, carrier);
        const std::string meshName = node->name ? node->name : node->mesh->name ? node->mesh->name : "";
        const bool hidden = meshName.find("_hidden") != std::string::npos;
        // a skin's joints: their matrices (world x inverse bind) and skeleton nodes
        std::vector<Mat4> jointMatrix;
        std::vector<int> jointNode;
        const cgltf_skin* skin = node->skin;
        if (skin) {
            for (size_t j = 0; j < skin->joints_count; ++j) {
                float jw[16], ibm[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
                cgltf_node_transform_world(skin->joints[j], jw);
                if (skin->inverse_bind_matrices) cgltf_accessor_read_float(skin->inverse_bind_matrices, j, ibm, 16);
                jointMatrix.push_back(matrixOf(jw) * matrixOf(ibm));
                int sn = skeleton ? skeletonNodeOf(skin->joints[j], n) : -1;
                if (skeleton && sn < 0) ++st.unmatchedJoints, sn = 0;
                jointNode.push_back(sn);
            }
        }
        for (size_t pi = 0; pi < node->mesh->primitives_count; ++pi) {
            const cgltf_primitive& prim = node->mesh->primitives[pi];
            if (prim.type != cgltf_primitive_type_triangles) continue;
            const cgltf_accessor *position = nullptr, *uv = nullptr, *colour = nullptr, *joints = nullptr, *weights = nullptr;
            for (size_t k = 0; k < prim.attributes_count; ++k) {
                const cgltf_attribute& at = prim.attributes[k];
                if (at.type == cgltf_attribute_type_position) position = at.data;
                else if (at.type == cgltf_attribute_type_texcoord && at.index == 0) uv = at.data;
                else if (at.type == cgltf_attribute_type_color && at.index == 0) colour = at.data;
                else if (at.type == cgltf_attribute_type_joints && at.index == 0) joints = at.data;
                else if (at.type == cgltf_attribute_type_weights && at.index == 0) weights = at.data;
            }
            if (!position || position->count == 0) continue;
            const size_t nv = position->count;
            const bool useSkin = skeleton && skin && joints && weights && joints->count == nv && weights->count == nv;
            const bool rigid = skeleton && !useSkin && carrier >= 0;
            // skinned: bound in a node's space whose rest can be undone (skinMesh: W * Wrest^-1 * Wrest[attach] * bind)
            int attach = 0;
            while (size_t(attach) + 1 < n && !invertible(sk.restWorld[size_t(attach)])) ++attach;
            Mesh m;
            m.positions.resize(nv * 3);
            m.uvs.assign(nv * 2, 0.0f);
            m.colors.assign(nv * 4, 255);
            m.posOf.resize(nv);
            m.bindPositions.resize(nv * 3);
            m.influenceCount.assign(nv, 0);
            m.influenceBone.assign(nv * 4, 0);
            m.influenceWeight.assign(nv * 4, 0.0f);
            m.visible = !hidden;
            std::map<int, int> boneSlot;
            auto slot = [&](int skNode) {
                auto it = boneSlot.find(skNode);
                if (it != boneSlot.end()) return it->second;
                m.boneNodes.push_back(skNode);
                return boneSlot[skNode] = int(m.boneNodes.size()) - 1;
            };
            for (size_t v = 0; v < nv; ++v) {
                float p[3] = {0, 0, 0}, rest[3] = {0, 0, 0};
                cgltf_accessor_read_float(position, v, p, 3);
                std::array<std::pair<int, float>, 4> inf{};
                int count = 0;
                if (useSkin) {
                    cgltf_uint jv[4] = {0, 0, 0, 0};
                    float wv[4] = {0, 0, 0, 0};
                    cgltf_accessor_read_uint(joints, v, jv, 4);
                    cgltf_accessor_read_float(weights, v, wv, 4);
                    float sum = 0;
                    for (int k = 0; k < 4; ++k) {
                        if (wv[k] <= 0 || jv[k] >= jointMatrix.size()) continue;
                        const Vec3 q = jointMatrix[jv[k]].transformPoint(p);
                        rest[0] += wv[k] * q.x, rest[1] += wv[k] * q.y, rest[2] += wv[k] * q.z;
                        sum += wv[k];
                        const int sn = jointNode[jv[k]];
                        bool merged = false;
                        for (int e = 0; e < count; ++e)
                            if (inf[size_t(e)].first == sn) inf[size_t(e)].second += wv[k], merged = true;
                        if (!merged) inf[size_t(count++)] = {sn, wv[k]};
                    }
                    if (sum > 1e-6f) {
                        for (float& c : rest) c /= sum;
                        for (int e = 0; e < count; ++e) inf[size_t(e)].second /= sum;
                    } else {
                        count = 0;
                    }
                }
                if (rigid) { // in its skeleton node's space, as the game's rigid meshes: through the
                    // world when that node's rest can be undone (an editor may set its bones' rest
                    // frames its own way), else as placed under it (a node at scale 0 at rest)
                    const bool undo = invertible(sk.restWorld[size_t(carrier)]);
                    const Vec3 wpt = world.transformPoint(p);
                    const Vec3 b = undo ? sk.restWorldInverse[size_t(carrier)].transformPoint(&wpt.x) : rel.transformPoint(p);
                    const Vec3 q = undo ? wpt : sk.restWorld[size_t(carrier)].transformPoint(&b.x);
                    m.bindPositions[v * 3] = b.x, m.bindPositions[v * 3 + 1] = b.y, m.bindPositions[v * 3 + 2] = b.z;
                    m.positions[v * 3] = q.x, m.positions[v * 3 + 1] = q.y, m.positions[v * 3 + 2] = q.z;
                    m.posOf[v] = uint32_t(v);
                    if (uv) cgltf_accessor_read_float(uv, v, &m.uvs[v * 2], 2);
                    if (colour) {
                        float c[4] = {1, 1, 1, 1};
                        cgltf_accessor_read_float(colour, v, c, colour->type == cgltf_type_vec3 ? 3 : 4);
                        for (int k = 0; k < 4; ++k) m.colors[v * 4 + size_t(k)] = uint8_t(std::clamp(c[k], 0.0f, 1.0f) * 255 + 0.5f);
                    }
                    continue;
                }
                if (count == 0) { // without weights or a skeleton node: on the root
                    const Vec3 q = world.transformPoint(p);
                    rest[0] = q.x, rest[1] = q.y, rest[2] = q.z;
                    if (skeleton) inf[0] = {nodeBone, 1.0f}, count = 1;
                }
                std::memcpy(&m.positions[v * 3], rest, sizeof rest);
                m.posOf[v] = uint32_t(v);
                if (skeleton) {
                    const Vec3 b = sk.restWorldInverse[size_t(attach)].transformPoint(rest);
                    m.bindPositions[v * 3] = b.x, m.bindPositions[v * 3 + 1] = b.y, m.bindPositions[v * 3 + 2] = b.z;
                    m.influenceCount[v] = uint8_t(count);
                    for (int e = 0; e < count; ++e) {
                        m.influenceBone[v * 4 + size_t(e)] = uint8_t(slot(inf[size_t(e)].first));
                        m.influenceWeight[v * 4 + size_t(e)] = inf[size_t(e)].second;
                    }
                } else {
                    std::memcpy(&m.bindPositions[v * 3], rest, sizeof rest);
                }
                if (uv) cgltf_accessor_read_float(uv, v, &m.uvs[v * 2], 2);
                if (colour) {
                    float c[4] = {1, 1, 1, 1};
                    cgltf_accessor_read_float(colour, v, c, colour->type == cgltf_type_vec3 ? 3 : 4);
                    for (int k = 0; k < 4; ++k) m.colors[v * 4 + size_t(k)] = uint8_t(std::clamp(c[k], 0.0f, 1.0f) * 255 + 0.5f);
                }
            }
            if (m.boneNodes.size() > 255) return fail(error, meshName + ": more than 255 joints in one mesh");
            m.skinned = skeleton && !rigid;
            m.attachNode = rigid ? carrier : skeleton ? attach : -1;
            if (prim.indices) {
                m.indices.resize(prim.indices->count);
                for (size_t k = 0; k < prim.indices->count; ++k) m.indices[k] = uint32_t(cgltf_accessor_read_index(prim.indices, k));
            } else {
                m.indices.resize(nv);
                for (size_t k = 0; k < nv; ++k) m.indices[k] = uint32_t(k);
            }
            m.indices.resize(m.indices.size() / 3 * 3);
            for (uint32_t& i : m.indices) i = std::min<uint32_t>(i, uint32_t(nv - 1));
            if (m.indices.empty()) continue;
            // its render state: the appearance it names, else the package's first (or a plain one)
            int appearance = appearanceOf(prim.material);
            if (appearance < 0 || size_t(appearance) >= model.materials.size()) {
                appearance = model.materials.empty() ? -1 : 0;
                for (const Mesh& om : model.meshes)
                    if (om.visible && om.appearance >= 0) { appearance = om.appearance; break; }
                if (appearance < 0) {
                    Material plain;
                    plain.properties = 8 | 16;
                    extraMaterials.push_back(plain);
                    appearance = int(model.materials.size() + extraMaterials.size()) - 1;
                }
            }
            m.appearance = appearance;
            m.texture = textureFor(prim.material, appearance);
            for (const Mesh& om : model.meshes)
                if (om.appearance == appearance && m.texture == om.texture) m.texture2 = om.texture2;
            st.vertices += int(nv);
            st.triangles += int(m.indices.size() / 3);
            ++st.meshes;
            meshes.push_back(std::move(m));
        }
    }
    if (meshes.empty()) return fail(error, "no triangle meshes in it");

    model.meshes = std::move(meshes);
    for (Material& m : extraMaterials) {
        model.appearanceIds.push_back(-1);
        model.materials.push_back(m);
    }
    for (Image& img : newTextures) {
        model.textureIds.push_back(-1);
        model.textures.push_back(std::move(img));
    }
    st.newTextures = int(newTextures.size());
    float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
    for (const Mesh& m : model.meshes)
        for (size_t v = 0; v + 2 < m.positions.size(); v += 3)
            for (int c = 0; c < 3; ++c) mn[c] = std::min(mn[c], m.positions[v + size_t(c)]), mx[c] = std::max(mx[c], m.positions[v + size_t(c)]);
    std::memcpy(model.boundsMin, mn, sizeof mn);
    std::memcpy(model.boundsMax, mx, sizeof mx);
    model.stats.triangles = st.triangles;
    if (stats) *stats = st;
    return true;
}

} // namespace cr
