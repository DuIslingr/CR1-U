#include "assets/model.hpp"

#include "assets/gxt.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/lzw.hpp"
#include "scene/pose.hpp"

namespace cr {

namespace {

enum Section {
    kAppearance = 0, kVertexBuffer, kIndexBuffer, kLight, kCamera, kGeometricShape, kBone,
    kTransformTree, kBoneDeformer, kMorphDeformer, kNodeDeformer, kFigure, kSectionCount
};

// Primitive types use GL enum values: 4 = GL_TRIANGLES (m201*), 5 = GL_TRIANGLE_STRIP
constexpr int kPrimTriangles = 4;
constexpr int kPrimStrips = 5;

// Bounds-checked little-endian reader over a byte range.
struct Reader {
    const uint8_t* p;
    size_t size;
    size_t pos = 0;
    bool ok = true;

    bool need(size_t n) {
        if (pos + n > size) ok = false;
        return ok;
    }
    uint8_t u8() { return need(1) ? p[pos++] : 0; }
    uint16_t u16() {
        if (!need(2)) return 0;
        const uint16_t v = uint16_t(p[pos] | (p[pos + 1] << 8));
        pos += 2;
        return v;
    }
    uint32_t u32() {
        if (!need(4)) return 0;
        uint32_t v;
        std::memcpy(&v, p + pos, 4); // little-endian hosts only (all our targets)
        pos += 4;
        return v;
    }
    float f32() {
        const uint32_t u = u32();
        float f;
        std::memcpy(&f, &u, 4);
        return f;
    }
    bool fixed = false; // header option byte: 0 = f32, 1 = s32 16.16 fixed point
    // A "dynamic"-typed real: f32 or 16.16 fixed depending on the file's option byte
    float real() {
        if (!fixed) return f32();
        return float(int32_t(u32())) / 65536.0f;
    }
};

struct RecordHeader {
    size_t start = 0, end = 0;
    uint32_t cls = 0;
    int32_t id = 0, user = 0;
};

RecordHeader readHeader(Reader& r) {
    RecordHeader h;
    h.start = r.pos;
    const uint32_t size = r.u32();
    h.cls = r.u32();
    h.id = int32_t(r.u32());
    h.user = int32_t(r.u32());
    h.end = h.start + size;
    if (size < 16 || h.end > r.size) r.ok = false;
    return h;
}

// Payload after the "compressed" flag: either the raw remainder of the record or its
// HI-LZW expansion. `r` must be positioned at the payload start.
bool payload(Reader& r, const RecordHeader& h, bool compressed, uint32_t decodedSize,
             std::vector<uint8_t>& buf, Reader& out) {
    if (compressed) {
        if (!lzwDecode(r.p, h.end, r.pos, buf, decodedSize)) return false;
        out = Reader{buf.data(), buf.size()};
    } else {
        out = Reader{r.p + r.pos, h.end - r.pos};
    }
    out.fixed = r.fixed;
    return true;
}

struct Attr {
    uint16_t count = 0;
    uint8_t type = 0, comps = 0;
};

int typeSize(uint8_t t) {
    switch (t) {
    case 0: return 4; // f32
    case 1: return 4; // s32 16.16 fixed point (positions in 36 models, e.g. c301, m203a)
    case 2: return 4; // packed RGBA8 (vertex colours, slot 4)
    case 3: return 2; // s16
    default: return 0;
    }
}

struct VertexBuffer {
    int id = -1;
    Attr attrs[8];
    float uvScale[2][4] = {};
    std::vector<float> pos;  // xyz per position
    std::vector<float> uv;   // uv per render vertex
    std::vector<uint8_t> colors; // RGBA per render vertex (slot 4), empty if none
    int maxCount = 0;
};

bool readVertexBuffer(Reader& r, VertexBuffer& vb, std::string& err) {
    const RecordHeader h = readHeader(r);
    vb.id = h.id;
    const bool compressed = r.u8() == 1;
    const uint32_t decodedSize = r.u32();
    for (int i = 0; i < 5; ++i) vb.attrs[i] = {r.u16(), r.u8(), r.u8()};
    if (h.cls > 0) r.u8();
    for (int i = 5; i < 8; ++i) vb.attrs[i] = {r.u16(), r.u8(), r.u8()};
    for (auto& s : vb.uvScale)
        for (float& f : s) f = r.real();
    if (!r.ok) { err = "truncated vertex buffer header"; return false; }

    std::vector<uint8_t> buf;
    Reader d{nullptr, 0};
    if (!payload(r, h, compressed, decodedSize, buf, d)) { err = "vertex buffer LZW failed"; return false; }

    // Attribute arrays are stored back to back in slot order
    size_t offsets[8], total = 0;
    for (int i = 0; i < 8; ++i) {
        const Attr& a = vb.attrs[i];
        offsets[i] = total;
        if (a.count == 0) continue;
        const int ts = typeSize(a.type);
        if (!ts) { err = "unknown vertex data type " + std::to_string(a.type); return false; }
        total += size_t(a.count) * a.comps * ts;
        vb.maxCount = std::max(vb.maxCount, int(a.count));
    }
    // Strict: the attribute table must describe the payload exactly (a wrong type size
    // otherwise silently shifts every later attribute)
    if (total != d.size) {
        err = "vertex layout (" + std::to_string(total) + " bytes) != payload (" + std::to_string(d.size) + ")";
        return false;
    }

    auto fetch = [&](int slot, size_t elem, int comp) -> float {
        const Attr& a = vb.attrs[slot];
        const uint8_t* q = d.p + offsets[slot] + (elem * a.comps + comp) * typeSize(a.type);
        if (a.type == 0) { float f; std::memcpy(&f, q, 4); return f; }
        if (a.type == 3) return float(int16_t(q[0] | (q[1] << 8)));
        if (a.type == 1) { int32_t v; std::memcpy(&v, q, 4); return float(v) / 65536.0f; }
        return float(q[0]);
    };

    const Attr& P = vb.attrs[0];
    if (P.comps < 3) { err = "position attribute has <3 components"; return false; }
    vb.pos.resize(size_t(P.count) * 3);
    for (size_t i = 0; i < P.count; ++i)
        for (int c = 0; c < 3; ++c) vb.pos[i * 3 + c] = fetch(0, i, c);

    const Attr& C = vb.attrs[4];
    if (C.count && C.type == 2 && C.comps == 1) {
        vb.colors.assign(d.p + offsets[4], d.p + offsets[4] + size_t(C.count) * 4);
    }

    const Attr& T = vb.attrs[5];
    if (T.count && T.comps >= 2) {
        // uv = raw * scale + offset (scale/offset per UV set, see __readVertexBuffer)
        const float* s = vb.uvScale[0];
        vb.uv.resize(size_t(T.count) * 2);
        for (size_t i = 0; i < T.count; ++i) {
            vb.uv[i * 2] = fetch(5, i, 0) * s[0] + s[1];
            vb.uv[i * 2 + 1] = fetch(5, i, 1) * s[0] + s[2];
        }
    }
    r.pos = h.end;
    return true;
}

struct IndexBuffer {
    int id = -1;
    int primType = 0;
    int appearanceId = -1;
    std::vector<int16_t> lengths;
    std::vector<uint16_t> indices; // empty = sequential
};

bool readIndexBuffer(Reader& r, IndexBuffer& ib, std::string& err) {
    const RecordHeader h = readHeader(r);
    ib.id = h.id;
    const bool compressed = r.u8() == 1;
    const uint32_t decodedSize = r.u32();
    ib.primType = r.u8();
    const bool hasUserData = r.u8() != 0;
    r.u16();
    const uint16_t nPrims = r.u16();
    const uint16_t nIndices = r.u16();
    ib.appearanceId = int32_t(r.u32());
    r.u32();
    if (!r.ok) { err = "truncated index buffer header"; return false; }
    (void)hasUserData; // user data follows the index data; not needed for rendering

    std::vector<uint8_t> buf;
    Reader d{nullptr, 0};
    if (!payload(r, h, compressed, decodedSize, buf, d)) { err = "index buffer LZW failed"; return false; }
    ib.lengths.resize(nPrims);
    for (auto& l : ib.lengths) l = int16_t(d.u16());
    ib.indices.resize(nIndices);
    for (auto& i : ib.indices) i = d.u16();
    if (!d.ok) { err = "index data truncated"; return false; }
    r.pos = h.end;
    return true;
}

struct Skin {
    int id = -1;
    int nPositions = 0;
    std::vector<int> boneNodes;     // skeleton node per bone
    std::vector<uint8_t> dup;       // render vertices per position
    std::vector<uint8_t> count;     // influences per position
    std::vector<uint8_t> bone;      // 4 per position
    std::vector<float> weight;      // 4 per position
};

bool readBoneDeformer(Reader& r, Skin& skin, std::string& err) {
    const RecordHeader h = readHeader(r);
    skin.id = h.id;
    const bool compressed = r.u8() == 1;
    const uint32_t decodedSize = r.u32();
    skin.nPositions = r.u16();
    const int nBones = r.u16();
    if (!r.ok) { err = "truncated bone deformer header"; return false; }

    std::vector<uint8_t> buf;
    Reader d{nullptr, 0};
    if (!payload(r, h, compressed, decodedSize, buf, d)) { err = "bone deformer LZW failed"; return false; }
    skin.boneNodes.resize(nBones);
    for (int& b : skin.boneNodes) b = d.u16();
    const size_t n = size_t(skin.nPositions);
    skin.dup.resize(n);
    skin.count.resize(n);
    skin.bone.assign(n * 4, 0);
    skin.weight.assign(n * 4, 0.0f);
    for (size_t i = 0; i < n; ++i) {
        const int k = d.u8();
        skin.dup[i] = d.u8();
        if (k > 4) { err = "bone deformer: more than 4 influences"; return false; }
        skin.count[i] = uint8_t(k);
        for (int j = 0; j < k; ++j) skin.weight[i * 4 + j] = d.real();
        for (int j = 0; j < k; ++j) skin.bone[i * 4 + j] = d.u8();
    }
    if (!d.ok) { err = "bone deformer data truncated"; return false; }
    r.pos = h.end;
    return true;
}

// Appearance (material) record — reader 0xa69a0. Layout verified on all 1,503 records.
bool readAppearance(Reader& r, Material& m, std::string& err) {
    const RecordHeader h = readHeader(r);
    m.id = h.id;
    auto rgba = [&](float* out) {
        const uint32_t c = r.u32(); // bytes R, G, B, A
        for (int i = 0; i < 4; ++i) out[i] = float((c >> (8 * i)) & 255) / 255.0f;
    };
    if (h.cls == 0) r.u32();
    rgba(m.diffuse);
    rgba(m.ambient);
    rgba(m.emission);
    rgba(m.specular);
    r.u32();                  // silhouette colour
    m.shininess = r.u8();
    m.alphaThreshold = r.u8();
    m.blendSrc = r.u8();
    m.blendDst = r.u8();
    m.shading = r.u8();
    m.culling = r.u8();
    r.u8();
    m.drawOrder = int16_t(r.u16());
    m.properties = r.u32();
    for (int i = 0; i < 3; ++i) r.real(); // polygon offset factor/units, silhouette width
    for (int unit = 0; unit < 2; ++unit) {
        const int ref = int32_t(r.u32());
        if (h.cls > 0) m.unitWord[unit] = r.u32();
        if (h.cls > 1) r.u8(); // uv stream
        const uint8_t texBlend = r.u8(), wrapS = r.u8(), wrapT = r.u8();
        const uint8_t magFilter = r.u8(), minFilter = r.u8();
        r.u8();                // texture type
        r.u32();               // texture blend colour
        float uv[15];
        for (float& v : uv) v = r.real(); // UV transform (4 x vec3) + UV scale (vec3)
        if (unit == 1) {
            m.textureRef2 = ref;
            m.texBlend2 = texBlend;
        }
        if (unit == 0) {       // only the first unit is used by shipped data with a texture
            std::memcpy(m.uvTransform, uv, sizeof uv);
            m.textureRef = ref;
            m.texBlend = texBlend;
            m.wrapS = wrapS;
            m.wrapT = wrapT;
            m.magFilter = magFilter;
            m.minFilter = minFilter;
        }
    }
    if (h.cls > 1) m.tail = r.u32();
    m.refGid = r.u32();        // ref gid
    if (!r.ok || r.pos != h.end) { err = "appearance size mismatch"; return false; }
    return true;
}

bool readTransformTree(Reader& r, Skeleton& skel, std::string& err) {
    const RecordHeader h = readHeader(r);
    r.u32(); // ref gid
    const int n = r.u16();
    skel.nodes.resize(n);
    for (SkeletonNode& node : skel.nodes) {
        const int parent = r.u16();
        node.parent = (parent == 0xFFFF || parent >= n) ? -1 : parent;
        for (auto& axis : node.axes)
            for (float& f : axis) f = r.real();
        for (float& f : node.translation) f = r.real();
        for (float& f : node.scale) f = r.real();
        // A few shipped files (be101mu*, m203*) store NaN for unused nodes; neutralise them
        for (int a = 0; a < 3; ++a) {
            if (!std::isfinite(node.translation[a])) node.translation[a] = 0;
            if (!std::isfinite(node.scale[a])) node.scale[a] = 1;
            for (int c = 0; c < 3; ++c)
                if (!std::isfinite(node.axes[a][c])) node.axes[a][c] = (a == c) ? 1.0f : 0.0f;
        }
        r.u8();
        for (int i = 0; i < 7; ++i) r.real(); // rotation limits (min/max) and damping
    }
    if (!r.ok || r.pos != h.end) { err = "transform tree size mismatch"; return false; }
    return true;
}

// NodeDeformer: object id -> node it is attached to
struct Attachment {
    int objectId = -1, node = -1;
    bool visible = true; // initial visibility; 21 helper/effect meshes start hidden
};

bool readNodeDeformer(Reader& r, std::vector<Attachment>& attach, std::string& err) {
    const RecordHeader h = readHeader(r);
    const int n = r.u16();
    r.u32();
    for (int i = 0; i < n; ++i) {
        Attachment a;
        a.objectId = int32_t(r.u32());
        a.node = r.u16();
        a.visible = r.u8() != 0;
        attach.push_back(a);
    }
    if (!r.ok || r.pos != h.end) { err = "node deformer size mismatch"; return false; }
    return true;
}

struct Figure {
    int id = -1;
    std::vector<int> indexBufferIds;
    int vertexBufferId = -1, boneDeformerId = -1;
};

bool readFigure(Reader& r, Figure& fig, std::string& err) {
    const RecordHeader h = readHeader(r);
    fig.id = h.id;
    const int n = r.u16();
    for (int i = 0; i < n; ++i) fig.indexBufferIds.push_back(int32_t(r.u32()));
    fig.vertexBufferId = int32_t(r.u32());
    r.u32();
    r.u32();
    fig.boneDeformerId = int32_t(r.u32());
    if (!r.ok || r.pos > h.end) { err = "figure truncated"; return false; }
    r.pos = h.end;
    return true;
}

// Converts strips to a triangle list, alternating winding and dropping degenerates.
int stripsToTriangles(const IndexBuffer& ib, int vertexCount, std::vector<uint32_t>& out) {
    size_t k = 0;
    int tris = 0;
    for (int16_t rawLen : ib.lengths) {
        const int len = rawLen < 0 ? -rawLen : rawLen;
        const bool flip = rawLen < 0;
        auto at = [&](size_t i) -> uint32_t {
            return ib.indices.empty() ? uint32_t(i) : ib.indices[i];
        };
        for (int i = 0; i + 2 < len; ++i) {
            uint32_t a = at(k + i), b = at(k + i + 1), c = at(k + i + 2);
            if (((i & 1) != 0) != flip) std::swap(a, b);
            if (a == b || b == c || a == c) continue;
            if (int(std::max({a, b, c})) >= vertexCount) continue;
            out.insert(out.end(), {a, b, c});
            ++tris;
        }
        k += size_t(len);
    }
    return tris;
}

// Triangle lists: each primitive is a run of complete triangles
int listToTriangles(const IndexBuffer& ib, int vertexCount, std::vector<uint32_t>& out) {
    size_t k = 0;
    int tris = 0;
    for (int16_t rawLen : ib.lengths) {
        const int len = rawLen < 0 ? -rawLen : rawLen;
        for (int i = 0; i + 2 < len; i += 3) {
            uint32_t v[3];
            for (int j = 0; j < 3; ++j) v[j] = ib.indices.empty() ? uint32_t(k + i + j) : ib.indices[k + i + j];
            if (v[0] == v[1] || v[1] == v[2] || v[0] == v[2]) continue;
            if (int(std::max({v[0], v[1], v[2]})) >= vertexCount) continue;
            out.insert(out.end(), {v[0], v[1], v[2]});
            ++tris;
        }
        k += size_t(len);
    }
    return tris;
}

} // namespace

bool isModelPackage(const std::vector<uint8_t>& file) {
    if (file.size() < 16) return false;
    uint32_t off;
    std::memcpy(&off, file.data(), 4);
    return off + 4 <= file.size() && std::memcmp(file.data() + off, "MCM_", 4) == 0;
}

bool loadModelPackage(const std::vector<uint8_t>& file, Model& out, std::string* error,
                      bool decodeTextures) {
    auto fail = [&](const std::string& msg) {
        if (error) *error = msg;
        return false;
    };
    if (!isModelPackage(file)) return fail("not a model package");

    // Package table: model, textures until a zero offset, then animations until a zero offset
    Reader table{file.data(), file.size()};
    const uint32_t modelOff = table.u32(), modelSize = table.u32();
    if (size_t(modelOff) + modelSize > file.size()) return fail("model entry out of range");
    for (;;) {
        const uint32_t off = table.u32(), size = table.u32();
        if (!table.ok || off == 0) break;
        if (size_t(off) + size > file.size()) return fail("texture entry out of range");
        if (!decodeTextures) continue;
        std::vector<uint8_t> tex(file.begin() + off, file.begin() + off + size);
        Image img;
        std::string terr;
        MctInfo info;
        if (!decodeMct(tex, img, &info, &terr)) {
            // The Vita release's packages: "MCT_", u32 texture id, a byte, then a GXT texture
            std::string gerr;
            if (tex.size() < 9 || !decodeGxt(tex, img, &gerr)) return fail("texture: " + terr + " / " + gerr);
            uint32_t id = 0;
            std::memcpy(&id, &tex[4], 4);
            info.texId = int(id);
        }
        out.textureIds.push_back(info.texId);
        out.textures.push_back(std::move(img));
    }
    for (int n = 0;; ++n) {
        const uint32_t off = table.u32(), size = table.u32();
        if (!table.ok || off == 0) break;
        if (size_t(off) + size > file.size()) return fail("animation entry out of range");
        const size_t first = out.actions.size();
        std::string aerr;
        if (!parseMca(file.data(), size_t(off) + size, off, out.actions, &aerr)) return fail("animation: " + aerr);
        for (size_t a = first; a < out.actions.size(); ++a)
            out.actions[a].name = "built-in " + std::to_string(n + 1) + "." + std::to_string(a - first + 1);
    }

    // Model block: 'MCM_' + 8 bytes, then 12 sections of size-prefixed records
    Reader r{file.data() + modelOff, modelSize, 12};
    r.fixed = file[modelOff + 8] == 1; // MCM_ u32 version, u8 option (real-number format)
    std::vector<VertexBuffer> vbs;
    std::vector<IndexBuffer> ibs;
    std::vector<Skin> skins;
    std::vector<Figure> figures;
    std::vector<Attachment> attachments;
    std::string err;
    for (int sec = 0; sec < kSectionCount; ++sec) {
        const int count = r.u16();
        out.stats.recordCounts[sec] = count;
        for (int i = 0; i < count; ++i) {
            const size_t start = r.pos;
            bool ok = true;
            switch (sec) {
            case kAppearance: {
                Material mat;
                ok = readAppearance(r, mat, err);
                out.appearanceIds.push_back(mat.id);
                out.materials.push_back(mat);
                break;
            }
            case kVertexBuffer: vbs.emplace_back(); ok = readVertexBuffer(r, vbs.back(), err); break;
            case kIndexBuffer: ibs.emplace_back(); ok = readIndexBuffer(r, ibs.back(), err); break;
            case kBoneDeformer: skins.emplace_back(); ok = readBoneDeformer(r, skins.back(), err); break;
            case kTransformTree:
                if (out.skeleton.nodes.empty()) {
                    ok = readTransformTree(r, out.skeleton, err);
                } else {
                    const RecordHeader h = readHeader(r); // only the first tree is used
                    r.pos = h.end;
                }
                break;
            case kNodeDeformer: ok = readNodeDeformer(r, attachments, err); break;
            case kFigure: {
                Figure fig;
                ok = readFigure(r, fig, err);
                if (ok && !fig.indexBufferIds.empty()) figures.push_back(std::move(fig));
                break;
            }
            case kLight: { // kept raw (crtool lightdump): the format isn't read yet
                const RecordHeader h = readHeader(r);
                if (r.ok) out.lightRecords.emplace_back(file.begin() + long(modelOff + h.start), file.begin() + long(modelOff + h.end));
                r.pos = h.end;
                break;
            }
            default: {
                const RecordHeader h = readHeader(r); // not needed for static meshes yet
                r.pos = h.end;
                break;
            }
            }
            if (ok && r.ok && start + 16 <= r.size) {
                Reader peek = r;
                peek.pos = start;
                const RecordHeader ph = readHeader(peek);
                out.records.push_back({sec, ph.id, ph.user});
            }
            if (!ok || !r.ok) {
                return fail("section " + std::to_string(sec) + " record " + std::to_string(i) + " @" +
                            std::to_string(start) + ": " + (err.empty() ? "truncated" : err));
            }
        }
        ++out.stats.sections;
    }
    out.stats.vertexBuffers = int(vbs.size());
    out.stats.indexBuffers = int(ibs.size());
    out.stats.boneDeformers = int(skins.size());

    // Skeleton rest pose
    Skeleton& skel = out.skeleton;
    computeWorld(skel, nullptr, 0, skel.restWorld);
    skel.restWorldInverse.resize(skel.restWorld.size());
    for (size_t i = 0; i < skel.restWorld.size(); ++i) skel.restWorldInverse[i] = skel.restWorld[i].affineInverse();

    auto findVb = [&](int id) -> const VertexBuffer* {
        for (const auto& v : vbs) if (v.id == id) return &v;
        return nullptr;
    };
    auto findSkin = [&](int id) -> const Skin* {
        for (const auto& s : skins) if (s.id == id) return &s;
        return nullptr;
    };
    for (const auto& a : attachments) out.objectNodes.push_back({a.objectId, a.node});
    auto attachOf = [&](int objectId) {
        for (const auto& a : attachments) if (a.objectId == objectId) return a.node;
        return -1;
    };
    auto visibleOf = [&](int objectId) {
        for (const auto& a : attachments) if (a.objectId == objectId) return a.visible;
        return true;
    };

    auto buildMesh = [&](const IndexBuffer& ib, const VertexBuffer& vb, const Skin* sk, int attach, bool visible) {
        if (ib.primType != kPrimStrips && ib.primType != kPrimTriangles) {
            ++out.stats.unsupportedPrims;
            return;
        }
        const size_t nPos = vb.pos.size() / 3;
        if (sk && (size_t(sk->nPositions) != nPos)) sk = nullptr; // mismatched skin: draw rigidly
        Mesh m;
        m.skinned = sk != nullptr;
        m.visible = visible;
        m.attachNode = attach < int(skel.nodes.size()) ? attach : -1;
        m.bindPositions = vb.pos;

        // Render vertices share positions: expand by the skin's duplicate counts
        const int nRender = vb.maxCount;
        m.posOf.resize(nRender);
        if (sk) {
            size_t k = 0;
            for (int p = 0; p < sk->nPositions; ++p)
                for (int d = 0; d < sk->dup[p] && k < m.posOf.size(); ++d) m.posOf[k++] = uint32_t(p);
            m.boneNodes = sk->boneNodes;
            m.influenceCount = sk->count;
            m.influenceBone = sk->bone;
            m.influenceWeight = sk->weight;
        } else {
            for (int v = 0; v < nRender; ++v)
                m.posOf[v] = std::min(uint32_t(v), nPos ? uint32_t(nPos - 1) : 0u);
        }
        m.uvs.assign(size_t(nRender) * 2, 0.0f);
        m.colors.assign(size_t(nRender) * 4, 255);
        if (!vb.colors.empty())
            std::memcpy(m.colors.data(), vb.colors.data(), std::min(m.colors.size(), vb.colors.size()));
        for (int v = 0; v < nRender && size_t(v) * 2 + 1 < vb.uv.size(); ++v) {
            m.uvs[v * 2] = vb.uv[v * 2];
            m.uvs[v * 2 + 1] = vb.uv[v * 2 + 1];
        }
        out.stats.triangles += ib.primType == kPrimTriangles ? listToTriangles(ib, nRender, m.indices)
                                                             : stripsToTriangles(ib, nRender, m.indices);

        for (size_t a = 0; a < out.appearanceIds.size(); ++a)
            if (out.appearanceIds[a] == ib.appearanceId) m.appearance = int(a);
        // Texture: the material references a texture by its MCT header id; a handful of
        // materials use a 1-based package index instead
        if (m.appearance >= 0) {
            const int ref = out.materials[m.appearance].textureRef;
            for (size_t t = 0; t < out.textureIds.size() && m.texture < 0; ++t)
                if (out.textureIds[t] == ref) m.texture = int(t);
            if (ref < 0) {
                ++out.stats.untextured;
            } else if (m.texture >= 0) {
                ++out.stats.texById;
            } else if (ref >= 1 && ref <= int(out.textures.size())) {
                m.texture = ref - 1;
                ++out.stats.texByIndex;
            } else {
                ++out.stats.texMissing;
            }
        }
        out.meshes.push_back(std::move(m));
    };

    // Preferred: bindings from mesh Figures (index buffers, vertex buffer, skin, attach node)
    std::vector<bool> ibUsed(ibs.size(), false);
    for (const Figure& fig : figures) {
        const VertexBuffer* vb = findVb(fig.vertexBufferId);
        if (!vb) continue;
        for (int ibId : fig.indexBufferIds) {
            for (size_t i = 0; i < ibs.size(); ++i) {
                if (ibs[i].id != ibId || ibUsed[i]) continue;
                ibUsed[i] = true;
                ++out.stats.figureBound;
                buildMesh(ibs[i], *vb, findSkin(fig.boneDeformerId), attachOf(fig.id), visibleOf(fig.id));
            }
        }
    }

    // Fallback for index buffers no Figure references: pair in file order, and match
    // skins by position / render-vertex counts
    for (size_t i = 0, v = 0; i < ibs.size() && !vbs.empty(); ++i) {
        if (ibUsed[i]) continue;
        int maxIdx = 0;
        for (uint16_t x : ibs[i].indices) maxIdx = std::max(maxIdx, int(x));
        if (maxIdx >= vbs[v].maxCount && v + 1 < vbs.size()) ++v;
        const Skin* sk = nullptr;
        for (const auto& s : skins) {
            int sum = 0;
            for (uint8_t d : s.dup) sum += d;
            if (size_t(s.nPositions) == vbs[v].pos.size() / 3 && sum == vbs[v].maxCount) sk = &s;
        }
        buildMesh(ibs[i], vbs[v], sk, -1, true);
    }

    // Rest-pose positions in skeleton space (used for bounds and static display)
    for (Mesh& m : out.meshes) skinMesh(out, m, skel.restWorld, m.positions);

    // Bounds over everything we will draw
    bool first = true;
    for (const Mesh& m : out.meshes) {
        if (!m.visible) continue;
        for (size_t v = 0; v + 2 < m.positions.size(); v += 3) {
            for (int c = 0; c < 3; ++c) {
                const float x = m.positions[v + c];
                if (first || x < out.boundsMin[c]) out.boundsMin[c] = x;
                if (first || x > out.boundsMax[c]) out.boundsMax[c] = x;
            }
            first = false;
        }
    }
    return true;
}

} // namespace cr
