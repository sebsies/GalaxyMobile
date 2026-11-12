// Game-thread half of the renderer: records the GX command stream into
// self-contained frames (see gx_record.h).
//
// Byte order: vertex data inside the command stream / display lists is
// big-endian; vertex arrays referenced by index are little-endian (cooked
// model data, or written by the CPU), except single-byte components.
// Texture memory stays big-endian and is decoded here.
#include <string.h>

#include <unordered_map>

#include "gpu.h"
#include "gx_record.h"
#include "port/heap_routing.h"
#include "port/port.h"
#include "tex_decode.h"

namespace gpu {

// ---------------------------------------------------------------------------
// Frame queue
// ---------------------------------------------------------------------------
std::unique_ptr<Frame> FrameQueue::publish(std::unique_ptr<Frame> f) {
    Frame* raw = f.release();
    std::shared_ptr<const Frame> shared(raw, [this](const Frame* p) {
        std::lock_guard<std::mutex> lock(mLock);
        mFree.emplace_back(const_cast< Frame* >(p));
    });
    std::unique_ptr<Frame> next;
    {
        std::lock_guard<std::mutex> lock(mLock);
        mLatestNumber.store(raw->number);
        mLatest.swap(shared);
        if (!mFree.empty()) {
            next = std::move(mFree.back());
            mFree.pop_back();
        }
    }
    // `shared` (the previous latest) is released outside the lock.
    shared.reset();
    if (void (*listener)() = mListener.load()) {
        listener();
    }
    if (!next) {
        next.reset(new Frame());
    }
    next->clear();
    return next;
}

std::shared_ptr<const Frame> FrameQueue::latest() {
    std::lock_guard<std::mutex> lock(mLock);
    return mLatest;
}

FrameQueue& frameQueue() {
    static FrameQueue q;
    return q;
}

namespace {

// ---------------------------------------------------------------------------
// Vertex loading
// ---------------------------------------------------------------------------
static const u8 kCompSize[8] = {1, 1, 2, 2, 4, 0, 0, 0};

struct Reader {
    const u8* p;
    bool bigEndian;

    u8 u8_() { return *p++; }
    u16 u16_() {
        u16 v = bigEndian ? (u16)((p[0] << 8) | p[1]) : (u16)(p[0] | (p[1] << 8));
        p += 2;
        return v;
    }
    u32 u32_() {
        u32 v = bigEndian ? be32(p) : ((u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24));
        p += 4;
        return v;
    }
    float comp(u32 fmt, float scale) {
        switch (fmt) {
        case 0:
            return u8_() * scale;
        case 1:
            return (s8)u8_() * scale;
        case 2:
            return u16_() * scale;
        case 3:
            return (s16)u16_() * scale;
        case 4: {
            u32 v = u32_();
            float f;
            memcpy(&f, &v, 4);
            return f;
        }
        default:
            return 0.0f;
        }
    }
    u32 color(u32 fmt) {
        switch (fmt) {
        case 0: {  // RGB565
            u16 c = u16_();
            u32 r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
            return ((r << 3) | (r >> 2)) | (((g << 2) | (g >> 4)) << 8) | (((b << 3) | (b >> 2)) << 16) | 0xFF000000u;
        }
        case 1: {  // RGB888
            u32 r = u8_(), g = u8_(), b = u8_();
            return r | (g << 8) | (b << 16) | 0xFF000000u;
        }
        case 2: {  // RGB888x
            u32 r = u8_(), g = u8_(), b = u8_();
            p++;
            return r | (g << 8) | (b << 16) | 0xFF000000u;
        }
        case 3: {  // RGBA4444
            u16 c = u16_();
            u32 r = (c >> 12) & 15, g = (c >> 8) & 15, b = (c >> 4) & 15, a = c & 15;
            return (r * 17) | ((g * 17) << 8) | ((b * 17) << 16) | ((a * 17) << 24);
        }
        case 4: {  // RGBA6666 (24 bits, big-endian bit order)
            u32 v = ((u32)p[0] << 16) | ((u32)p[1] << 8) | p[2];
            p += 3;
            u32 r = (v >> 18) & 63, g = (v >> 12) & 63, b = (v >> 6) & 63, a = v & 63;
            auto e = [](u32 x) { return (x << 2) | (x >> 4); };
            return e(r) | (e(g) << 8) | (e(b) << 16) | (e(a) << 24);
        }
        default: {  // RGBA8888
            u32 r = u8_(), g = u8_(), b = u8_(), a = u8_();
            return r | (g << 8) | (b << 16) | (a << 24);
        }
        }
    }
};

inline float fromBits(u32 v) {
    float f;
    memcpy(&f, &v, 4);
    return f;
}
inline u32 toBits(float f) {
    u32 v;
    memcpy(&v, &f, 4);
    return v;
}

struct AttrDesc {
    u32 type;   // AttrType
    u32 cnt, fmt, shift;
};

// Reads `n` components of format `fmt` (u8, s8, u16, s16, f32) from `p`
// into `w` as float bits; returns the end of the data read.
inline const u8* readComps(const u8* p, u32 fmt, bool bigEndian, u32 n, float scale, u32*& w) {
    switch (fmt) {
    case 0:
        for (u32 i = 0; i < n; i++) *w++ = toBits(p[i] * scale);
        return p + n;
    case 1:
        for (u32 i = 0; i < n; i++) *w++ = toBits((s8)p[i] * scale);
        return p + n;
    case 2:
        for (u32 i = 0; i < n; i++, p += 2) *w++ = toBits((bigEndian ? (u16)((p[0] << 8) | p[1]) : (u16)(p[0] | (p[1] << 8))) * scale);
        return p;
    case 3:
        for (u32 i = 0; i < n; i++, p += 2) *w++ = toBits((s16)(bigEndian ? ((p[0] << 8) | p[1]) : (p[0] | (p[1] << 8))) * scale);
        return p;
    case 4:
        for (u32 i = 0; i < n; i++, p += 4) {
            u32 v;
            memcpy(&v, p, 4);
            *w++ = bigEndian ? __builtin_bswap32(v) : v;
        }
        return p;
    default:
        for (u32 i = 0; i < n; i++) *w++ = 0;
        return p;
    }
}

// The parts of the indexed vertex arrays (0..11) a draw read: elements lo..hi
// (none if lo > hi) of elemBytes bytes each, at the array base and stride
// the CP had then.
struct ArrayUse {
    u32 base[12], stride[12], elemBytes[12], lo[12], hi[12];
    void reset() {
        for (int i = 0; i < 12; i++) {
            lo[i] = ~0u;
            hi[i] = 0;
        }
    }
};

// Decodes `count` vertices of format `vat` into `out` (appended); returns
// flags.  With Track, also notes which array elements were read in `use`.
template <bool Track>
u32 loadVerticesT(WordBuffer& out, const u8* data, u32 count, int vat, u32 stride, ArrayUse* use) {
    const CPState& cp = g.cp;
    u32 lo = cp.vcdLo, hi = cp.vcdHi;
    u32 a = cp.vatA[vat], b = cp.vatB[vat], c = cp.vatC[vat];

    bool pnmtx = lo & 1;
    bool texmtx = ((lo >> 1) & 0xFF) != 0;
    AttrDesc pos = {(lo >> 9) & 3, (a >> 0) & 1, (a >> 1) & 7, (a >> 4) & 31};
    AttrDesc nrm = {(lo >> 11) & 3, (a >> 9) & 1, (a >> 10) & 7, 0};
    bool nrmIndex3 = (a >> 31) & 1;
    AttrDesc clr[2] = {{(lo >> 13) & 3, (a >> 13) & 1, (a >> 14) & 7, 0}, {(lo >> 15) & 3, (a >> 17) & 1, (a >> 18) & 7, 0}};
    AttrDesc tex[8];
    tex[0] = {(hi >> 0) & 3, (a >> 21) & 1, (a >> 22) & 7, (a >> 25) & 31};
    tex[1] = {(hi >> 2) & 3, (b >> 0) & 1, (b >> 1) & 7, (b >> 4) & 31};
    tex[2] = {(hi >> 4) & 3, (b >> 9) & 1, (b >> 10) & 7, (b >> 13) & 31};
    tex[3] = {(hi >> 6) & 3, (b >> 18) & 1, (b >> 19) & 7, (b >> 22) & 31};
    tex[4] = {(hi >> 8) & 3, (b >> 27) & 1, (b >> 28) & 7, (c >> 0) & 31};
    tex[5] = {(hi >> 10) & 3, (c >> 5) & 1, (c >> 6) & 7, (c >> 9) & 31};
    tex[6] = {(hi >> 12) & 3, (c >> 14) & 1, (c >> 15) & 7, (c >> 18) & 31};
    tex[7] = {(hi >> 14) & 3, (c >> 23) & 1, (c >> 24) & 7, (c >> 27) & 31};

    u32 flags = 0;
    if (pnmtx) flags |= VF_POSMTX;
    if (nrm.type) flags |= nrm.cnt ? (VF_NRM | VF_NBT) : VF_NRM;
    if (clr[0].type) flags |= VF_CLR0;
    if (clr[1].type) flags |= VF_CLR1;
    for (int i = 0; i < 8; i++) {
        if (tex[i].type) flags |= VF_TEX0 << i;
    }
    if (texmtx) flags |= VF_TEXMTX;

    float posScale = (pos.fmt == 4) ? 1.0f : ldexpf(1.0f, -(int)pos.shift);
    float nrmScale = (nrm.fmt == 1) ? 1.0f / 64.0f : (nrm.fmt == 3) ? 1.0f / 16384.0f : 1.0f;
    // The texture coordinates present, in order, with their scales.
    int texList[8], texCount = 0;
    float texScale[8];
    for (int i = 0; i < 8; i++) {
        if (tex[i].type) {
            texList[texCount++] = i;
            texScale[i] = (tex[i].fmt == 4) ? 1.0f : ldexpf(1.0f, -(int)tex[i].shift);
        }
    }

    // Host addresses of the indexed arrays, resolved once for the draw.
    const u8* arrayHost[12] = {};
    auto resolveArray = [&](int array, u32 type) {
        if (type >= ATTR_INDEX8) {
            arrayHost[array] = memPtr(cp.arrayBase[array]);
        }
    };
    resolveArray(0, pos.type);
    resolveArray(1, nrm.type);
    resolveArray(2, clr[0].type);
    resolveArray(3, clr[1].type);
    for (int t = 0; t < 8; t++) {
        resolveArray(4 + t, tex[t].type);
    }
    if (Track) {
        static const u8 kClrBytes[8] = {2, 3, 4, 2, 3, 4, 4, 4};
        u32 elem[12];
        elem[0] = (pos.cnt ? 3 : 2) * kCompSize[pos.fmt];
        elem[1] = (nrm.cnt && !nrmIndex3 ? 9 : 3) * kCompSize[nrm.fmt];
        elem[2] = kClrBytes[clr[0].fmt];
        elem[3] = kClrBytes[clr[1].fmt];
        for (int t = 0; t < 8; t++) {
            elem[4 + t] = (tex[t].cnt ? 2 : 1) * kCompSize[tex[t].fmt];
        }
        for (int i = 0; i < 12; i++) {
            if (arrayHost[i]) {
                // Several draws of a display list may read the same array.
                if (use->lo[i] <= use->hi[i] && (use->base[i] != cp.arrayBase[i] || use->stride[i] != cp.arrayStride[i])) {
                    use->lo[i] = 0;  // based elsewhere: the tracking gives up on it
                    use->hi[i] = ~0u;
                }
                use->base[i] = cp.arrayBase[i];
                use->stride[i] = cp.arrayStride[i];
                use->elemBytes[i] = elem[i];
            }
        }
    }
    auto arrayPtr = [&](int array, u32 index) -> const u8* {
        if (Track) {
            use->lo[array] = index < use->lo[array] ? index : use->lo[array];
            use->hi[array] = index > use->hi[array] ? index : use->hi[array];
        }
        return arrayHost[array] + index * cp.arrayStride[array];
    };
    auto readIndex = [](const u8*& v, u32 type) -> u32 {
        if (type == ATTR_INDEX8) {
            return *v++;
        }
        u32 idx = ((u32)v[0] << 8) | v[1];
        v += 2;
        return idx;
    };
    u32 texmtxMask = (lo >> 1) & 0xFF;
    // A vertex that carries texture matrix indices may carry them for only
    // some tex coords (skinned models: for the texgens that follow the
    // bones); the others take the index in the CP's matrix index registers,
    // as on the console's vertex loader.  Leaving them 0 transformed those
    // coords by position matrix 0: Rosalina's hair and dress sampled a
    // different part of her texture every frame.
    u32 defaultTexMtx[8];
    for (int i = 0; i < 8; i++) {
        defaultTexMtx[i] = i < 4 ? (cp.matIndexA >> (6 * (i + 1))) & 63 : (cp.matIndexB >> (6 * (i - 4))) & 63;
    }

    size_t base = out.size();
    out.resize(base + (size_t)count * vtxWords(flags));
    u32* w = out.data() + base;

    for (u32 n = 0; n < count; n++) {
        const u8* v = data + (size_t)n * stride;
        u32 midx[9] = {0};
        if (pnmtx) {
            midx[0] = *v++ & 63;
        }
        if (texmtxMask) {
            for (int i = 0; i < 8; i++) {
                midx[1 + i] = (texmtxMask >> i) & 1 ? (*v++ & 63) : defaultTexMtx[i];
            }
        }

        // Position
        {
            u32 comps = pos.cnt ? 3 : 2;
            if (pos.type < ATTR_INDEX8) {
                v = readComps(v, pos.fmt, true, comps, posScale, w);
            } else {
                readComps(arrayPtr(0, readIndex(v, pos.type)), pos.fmt, false, comps, posScale, w);
            }
            if (!pos.cnt) {
                *w++ = 0;  // z = 0.0f
            }
        }
        if (pnmtx) {
            *w++ = midx[0];
        }

        // Normal (+ binormal, tangent)
        if (nrm.type) {
            u32 vecs = nrm.cnt ? 3 : 1;
            if (nrm.type == ATTR_DIRECT) {
                v = readComps(v, nrm.fmt, true, vecs * 3, nrmScale, w);
            } else if (nrm.cnt && nrmIndex3) {
                u32 idx[3];
                for (int k = 0; k < 3; k++) {
                    idx[k] = readIndex(v, nrm.type);
                }
                for (int k = 0; k < 3; k++) {
                    readComps(arrayPtr(1, idx[k]), nrm.fmt, false, 3, nrmScale, w);
                }
            } else {
                readComps(arrayPtr(1, readIndex(v, nrm.type)), nrm.fmt, false, vecs * 3, nrmScale, w);
            }
        }

        // Colors
        for (int ci = 0; ci < 2; ci++) {
            if (!clr[ci].type) {
                continue;
            }
            const u8* src = clr[ci].type == ATTR_DIRECT ? v : arrayPtr(2 + ci, readIndex(v, clr[ci].type));
            u32 color;
            if (clr[ci].fmt == 5) {  // RGBA8888, the common case
                memcpy(&color, src, 4);
                src += 4;
            } else {
                Reader r{src, clr[ci].type == ATTR_DIRECT};
                color = r.color(clr[ci].fmt);
                src = r.p;
            }
            *w++ = color;
            if (clr[ci].type == ATTR_DIRECT) {
                v = src;
            }
        }

        // Texture coordinates
        for (int ti = 0; ti < texCount; ti++) {
            int t = texList[ti];
            u32 comps = tex[t].cnt ? 2 : 1;
            if (tex[t].type == ATTR_DIRECT) {
                v = readComps(v, tex[t].fmt, true, comps, texScale[t], w);
            } else {
                readComps(arrayPtr(4 + t, readIndex(v, tex[t].type)), tex[t].fmt, false, comps, texScale[t], w);
            }
            if (!tex[t].cnt) {
                *w++ = 0;  // t = 0.0f
            }
        }

        if (texmtx) {
            *w++ = midx[1] | (midx[2] << 8) | (midx[3] << 16) | (midx[4] << 24);
            *w++ = midx[5] | (midx[6] << 8) | (midx[7] << 16) | (midx[8] << 24);
        }
    }
    return flags;
}

u32 loadVertices(WordBuffer& out, const u8* data, u32 count, int vat, u32 stride) { return loadVerticesT<false>(out, data, count, vat, stride, nullptr); }

// ---------------------------------------------------------------------------
// Display list cache.  A display list's draws decode to the same vertices as
// long as the list, the CP's vertex state when it is called and the parts of
// the vertex arrays its draws read are unchanged: J3D models call the same
// lists every frame, with their static vertex arrays.  The decoded words
// are kept and copied into the frame on later calls, which spares the game
// thread most of its recording time (models are drawn with thousands of
// short strips).  The array contents are checked with a hash of each range
// read, once per FIFO flush: the CPU may rewrite arrays between flushes
// (CPU skinning, blend shapes), never while a flush is being processed.
// ---------------------------------------------------------------------------
u64 hashRange(const u8* p, size_t n) {
    // xxHash64-style rounds on four lanes.
    const u64 k1 = 0x9E3779B185EBCA87ull, k2 = 0xC2B2AE3D27D4EB4Full;
    u64 h[4] = {k1 + k2, k2, 0, 0 - k1};
    auto round = [&](u64 acc, u64 v) {
        acc += v * k2;
        acc = (acc << 31) | (acc >> 33);
        return acc * k1;
    };
    size_t i = 0;
    for (; i + 32 <= n; i += 32) {
        u64 v[4];
        memcpy(v, p + i, 32);
        h[0] = round(h[0], v[0]);
        h[1] = round(h[1], v[1]);
        h[2] = round(h[2], v[2]);
        h[3] = round(h[3], v[3]);
    }
    u64 r = h[0] ^ (h[1] * 3) ^ (h[2] * 5) ^ (h[3] * 7) ^ n;
    for (; i + 8 <= n; i += 8) {
        u64 v;
        memcpy(&v, p + i, 8);
        r = round(r, v);
    }
    for (; i < n; i++) {
        r = round(r, p[i]);
    }
    r ^= r >> 29;
    r *= k1;
    return r ^ (r >> 32);
}

// CP state a display list's vertex decoding starts from.
struct VtxState {
    u32 vcdLo, vcdHi, matIndexA, matIndexB;
    u32 vat[8][3];
    u32 arrayBase[12], arrayStride[12];
    void capture() {
        const CPState& cp = g.cp;
        vcdLo = cp.vcdLo;
        vcdHi = cp.vcdHi;
        matIndexA = cp.matIndexA;
        matIndexB = cp.matIndexB;
        for (int i = 0; i < 8; i++) {
            vat[i][0] = cp.vatA[i];
            vat[i][1] = cp.vatB[i];
            vat[i][2] = cp.vatC[i];
        }
        memcpy(arrayBase, cp.arrayBase, sizeof(arrayBase));
        memcpy(arrayStride, cp.arrayStride, sizeof(arrayStride));
    }
    bool operator==(const VtxState& o) const { return memcmp(this, &o, sizeof(*this)) == 0; }
};

// A range of vertex array data some cached draws were decoded from.
struct ArrayDep {
    u32 addr, bytes;
    u64 hash;
};

struct DlDraw {
    u32 prim, word, count, flags;  // vertices: words from `word` in DlEntry::words
};

struct DlEntry {
    std::vector<u8> bytes;  // the list's contents when it was decoded
    VtxState state;
    WordBuffer words;
    std::vector<DlDraw> draws;
    std::vector<ArrayDep> deps;
    u64 lastUsed = 0;  // frame number
    bool valid = false;
    bool drawsOnly = false;  // no register loads between the draws: replayed without parsing
};

// ---------------------------------------------------------------------------
// Textures
// ---------------------------------------------------------------------------
static const u8 kTexRegBase[8] = {0x80, 0x81, 0x82, 0x83, 0xA0, 0xA1, 0xA2, 0xA3};

u64 hashBytes(const u8* p, size_t n) { return hashRange(p, n); }

struct TexKey {
    u32 addr, fmt, width, height, levels, tlutAddr, tlutFmt;
    bool operator==(const TexKey& o) const { return memcmp(this, &o, sizeof(*this)) == 0; }
};
struct TexKeyHash {
    size_t operator()(const TexKey& k) const { return (size_t)hashBytes((const u8*)&k, sizeof(k)); }
};

struct TexEntry {
    std::shared_ptr<TexImage> image;
    u64 dataHash = 0;
    u64 checkedFrame = ~0ull;
    u32 checkedInval = ~0u;
};

struct EfbCopyInfo {
    u32 id;
    u32 width, height, format;
    u64 frame;
    u64 memHash;
};

// ---------------------------------------------------------------------------
// Recorder backend
// ---------------------------------------------------------------------------
struct Recorder final : Backend {
    std::unique_ptr<Frame> frame{new Frame()};
    u64 frameNumber = 1;
    u8 tlutMem[0x80000];  // TMEM half used for palettes
    std::unordered_map<TexKey, TexEntry, TexKeyHash> texCache;
    std::unordered_map<const TexImage*, u32> frameTexIndex;
    std::unordered_map<u32, EfbCopyInfo> efbCopies;  // by destination address
    u32 nextCopyId = 1;
    u32 boundTex[8];      // last bound per unit (frame texture index or copy id | 0x80000000)
    bool texDirty = true;
    // What each unit's registers resolved to: reused while they stay the
    // same and no EFB copy, palette load or frame end happened (resolving
    // takes hash lookups, and draws re-resolve after most register writes).
    struct UnitMemo {
        u32 mode0, mode1, image0, image3, tlut, binding;
        u64 epoch = 0;
    };
    UnitMemo unitMemo[8];
    u64 bindEpoch = 1;
    std::vector<u32> decodeScratch;

    // Display list cache (DlEntry), by address << 32 | size, with a few
    // variants per list (different vertex state or arrays).
    struct DlSlot {
        std::vector<DlEntry> ways;
        u64 noDrawsUntil = 0;  // the list drew nothing when last seen: not cached until this frame
    };
    std::unordered_map<u64, DlSlot> dlCache;
    bool dlCacheOn = getenv("PETARI_NODLCACHE") == nullptr;
    // PETARI_DLCHECK=1: every cached draw is decoded as well and compared
    // (lists are then replayed a draw at a time).
    bool dlCheck = getenv("PETARI_DLCHECK") != nullptr;
    WordBuffer dlCheckScratch;
    u64 dlCheckDraws = 0, dlCheckBad = 0;
    DlSlot* dlSlot = nullptr;    // the list being called
    DlEntry* dlEntry = nullptr;  // its variant replayed from, or recorded into
    bool dlReplay = false;
    u32 dlDrawIndex = 0;
    ArrayUse dlUse;
    // Array ranges hashed during the current FIFO flush (address << 32 | bytes).
    std::unordered_map<u64, u64> flushHashes;
    u64 flushHashesStream = ~0ull;
    u32 texInvalEpoch = 0;
    struct DlStats {
        u64 calls = 0, hits = 0, recorded = 0, hashedBytes = 0, copiedWords = 0, decodedWords = 0;
    } dlStats;

    Recorder() {
        memset(tlutMem, 0, sizeof(tlutMem));
        resetBindings();
        snapshot();
    }

    void snapshot() {
        memcpy(frame->startBp, g.bp, sizeof(frame->startBp));
        memcpy(frame->startTevReg, g.tevReg, sizeof(frame->startTevReg));
        memcpy(frame->startTevKonst, g.tevKonst, sizeof(frame->startTevKonst));
        memcpy(frame->startXfRegs, g.xf.regs, sizeof(frame->startXfRegs));
        memcpy(frame->startXfMem, g.xf.mem, sizeof(frame->startXfMem));
    }

    void resetBindings() {
        for (u32& b : boundTex) {
            b = ~0u;
        }
        texDirty = true;
        bindEpoch++;
        frameTexIndex.clear();
    }

    WordBuffer& cmds() { return frame->cmds; }
    // Room for n more command words.
    u32* appendCmds(size_t n) {
        WordBuffer& c = frame->cmds;
        size_t at = c.size();
        c.resize(at + n);
        return c.data() + at;
    }

    // Hash of `bytes` of vertex array data at `addr`, computed once per flush.
    u64 arrayHash(u32 addr, u32 bytes) {
        if (flushHashesStream != gStreamSerial) {
            flushHashes.clear();
            flushHashesStream = gStreamSerial;
        }
        u64 key = ((u64)addr << 32) | bytes;
        auto it = flushHashes.find(key);
        if (it != flushHashes.end()) {
            return it->second;
        }
        dlStats.hashedBytes += bytes;
        u64 h = hashRange(memPtr(addr), bytes);
        flushHashes.emplace(key, h);
        return h;
    }

    bool depsCurrent(const DlEntry& e) {
        for (const ArrayDep& d : e.deps) {
            if (arrayHash(d.addr, d.bytes) != d.hash) {
                return false;
            }
        }
        return true;
    }

    bool displayListBegin(u32 addr, const u8* data, u32 size) override {
        dlSlot = nullptr;
        dlEntry = nullptr;
        if (!dlCacheOn) {
            return false;
        }
        dlStats.calls++;
        DlSlot& slot = dlCache[((u64)addr << 32) | size];
        dlSlot = &slot;
        if (frameNumber < slot.noDrawsUntil) {
            return false;  // a material list: registers only
        }
        VtxState st;
        st.capture();
        DlEntry* victim = nullptr;
        for (DlEntry& e : slot.ways) {
            if (e.valid && e.state == st && e.bytes.size() == size && memcmp(e.bytes.data(), data, size) == 0 && depsCurrent(e)) {
                e.lastUsed = frameNumber;
                dlStats.hits++;
                if (e.drawsOnly && !dlCheck) {
                    replayDraws(e);
                    dlSlot = nullptr;
                    return true;
                }
                dlEntry = &e;
                dlReplay = true;
                dlDrawIndex = 0;
                return false;
            }
            if (!victim || !e.valid || (victim->valid && e.lastUsed < victim->lastUsed)) {
                victim = &e;
            }
        }
        if ((!victim || victim->valid) && slot.ways.size() < 4) {
            slot.ways.emplace_back();
            victim = &slot.ways.back();
        }
        // Record the list's draws into the least recently used variant.
        DlEntry& e = *victim;
        e.valid = false;
        e.bytes.assign(data, data + size);
        e.state = st;
        e.words.clear();
        e.draws.clear();
        e.deps.clear();
        e.lastUsed = frameNumber;
        e.drawsOnly = true;
        dlEntry = &e;
        dlReplay = false;
        dlUse.reset();
        dlStats.recorded++;
        return false;
    }

    // A cached list of draws alone: all its vertices in one copy, and the
    // draw commands pointing at them.  The texture bindings cannot change
    // between its draws.
    void replayDraws(const DlEntry& e) {
        resolveTextures();
        WordBuffer& verts = frame->verts;
        size_t base = verts.size();
        verts.resize(base + e.words.size());
        memcpy(verts.data() + base, e.words.data(), e.words.size() * 4);
        u32* cmd = appendCmds(5 * e.draws.size());
        for (const DlDraw& d : e.draws) {
            cmd[0] = CMD_DRAW;
            cmd[1] = d.prim;
            cmd[2] = d.flags;
            cmd[3] = (u32)base + d.word;
            cmd[4] = d.count;
            cmd += 5;
        }
        frame->draws += (u32)e.draws.size();
        dlStats.copiedWords += e.words.size();
    }

    // A register load while a list is being recorded: it is replayed
    // through the parser.
    void noteListLoad() {
        if (dlEntry && !dlReplay) {
            dlEntry->drawsOnly = false;
        }
    }

    void displayListEnd() override {
        if (dlEntry && !dlReplay) {
            DlEntry& e = *dlEntry;
            if (e.draws.empty()) {
                // Nothing drawn: not worth comparing on every call.  Looked
                // at again later, in case the address comes to hold a
                // different list of the same size.
                dlSlot->noDrawsUntil = frameNumber + 300;
                e.bytes.clear();
                e.bytes.shrink_to_fit();
            } else {
                bool ok = true;
                for (int i = 0; i < 12; i++) {
                    if (dlUse.lo[i] > dlUse.hi[i]) {
                        continue;
                    }
                    if (dlUse.hi[i] == ~0u) {
                        ok = false;  // an array rebased inside the list
                        break;
                    }
                    u32 addr = dlUse.base[i] + dlUse.lo[i] * dlUse.stride[i];
                    u32 bytes = (dlUse.hi[i] - dlUse.lo[i]) * dlUse.stride[i] + dlUse.elemBytes[i];
                    e.deps.push_back({addr, bytes, arrayHash(addr, bytes)});
                }
                e.valid = ok;
            }
        }
        dlSlot = nullptr;
        dlEntry = nullptr;
    }

    // Vertices of a draw: from the display list cache when the list is
    // replayed, decoded (and kept, when a list is being recorded) otherwise.
    u32 drawVertices(int prim, int vat, u32 count, const u8* data, u32 vertexStride) {
        WordBuffer& verts = frame->verts;
        size_t first = verts.size();
        if (dlEntry && dlReplay) {
            if (dlDrawIndex < dlEntry->draws.size() && dlEntry->draws[dlDrawIndex].count == count) {
                const DlDraw& d = dlEntry->draws[dlDrawIndex++];
                size_t n = (size_t)count * vtxWords(d.flags);
                verts.resize(first + n);
                memcpy(verts.data() + first, dlEntry->words.data() + d.word, n * 4);
                dlStats.copiedWords += n;
                if (dlCheck) {
                    dlCheckScratch.clear();
                    u32 flags = loadVertices(dlCheckScratch, data, count, vat, vertexStride);
                    dlCheckDraws++;
                    if (flags != d.flags || (u32)prim != d.prim || dlCheckScratch.size() != n ||
                        memcmp(dlCheckScratch.data(), verts.data() + first, n * 4) != 0) {
                        if (dlCheckBad++ < 20) {
                            port_log("dlcheck: cached draw differs: prim %x/%x flags %x/%x words %zu/%zu", prim, d.prim, flags, d.flags,
                                     dlCheckScratch.size(), n);
                        }
                    }
                    if (dlCheckDraws % 200000 == 0) {
                        port_log("dlcheck: %llu cached draws compared, %llu differed", (unsigned long long)dlCheckDraws, (unsigned long long)dlCheckBad);
                    }
                }
                return d.flags;
            }
            // Cannot happen for the same contents and state; decode from
            // here on and record the list again next time.
            dlEntry->valid = false;
            dlEntry = nullptr;
        } else if (dlEntry) {
            u32 flags = loadVerticesT<true>(verts, data, count, vat, vertexStride, &dlUse);
            DlDraw d{(u32)prim, (u32)dlEntry->words.size(), count, flags};
            dlEntry->words.insert(dlEntry->words.end(), verts.begin() + first, verts.end());
            dlEntry->draws.push_back(d);
            dlStats.decodedWords += verts.size() - first;
            return flags;
        } else if (dlSlot && dlSlot->noDrawsUntil) {
            dlSlot->noDrawsUntil = 0;  // it draws after all: record it next time
        }
        u32 flags = loadVertices(verts, data, count, vat, vertexStride);
        dlStats.decodedWords += verts.size() - first;
        return flags;
    }

    void draw(int primitive, int vat, u32 count, const u8* data, u32 vertexStride) override {
        resolveTextures();
        u32 first = (u32)frame->verts.size();
        u32 flags = drawVertices(primitive, vat, count, data, vertexStride);
        u32* cmd = appendCmds(5);
        cmd[0] = CMD_DRAW;
        cmd[1] = (u32)primitive;
        cmd[2] = flags;
        cmd[3] = first;
        cmd[4] = count;
        frame->draws++;
    }

    void bpWrite(u32 reg, u32 value, u32) override {
        noteListLoad();
        u32* cmd = appendCmds(2);
        cmd[0] = CMD_BP;
        cmd[1] = (reg << 24) | (value & 0xFFFFFF);
        if ((reg >= 0x80 && reg <= 0xBB) || (reg >= 0x28 && reg <= 0x2F) || reg == 0x00 || reg == 0x27 || reg == 0x66) {
            texDirty = true;
            if (reg == 0x66) {
                texInvalEpoch++;
            }
        }
    }

    void xfWrite(u32 addr, u32 count) override {
        noteListLoad();
        auto& c = cmds();
        size_t at = c.size();
        c.resize(at + 3 + count);
        u32* w = c.data() + at;
        w[0] = CMD_XF;
        w[1] = addr;
        w[2] = count;
        for (u32 i = 0; i < count; i++) {
            u32 a = addr + i;
            u32 v = 0;
            if (a < 0x1000) {
                v = toBits(g.xf.mem[a]);
            } else if (a < 0x1100) {
                v = g.xf.regs[a - 0x1000];
            }
            w[3 + i] = v;
        }
    }

    void cpWrite(u32, u32) override { noteListLoad(); }

    void efbCopy(bool toXfb, bool clear) override {
        noteListLoad();
        (void)clear;
        const u32* bp = g.bp;
        u32 id = nextCopyId++;
        auto& c = cmds();
        c.push_back(CMD_EFB_COPY);
        c.push_back(id);
        static const u8 regs[9] = {0x49, 0x4A, 0x4B, 0x4D, 0x4E, 0x4F, 0x50, 0x51, 0x52};
        for (u8 r : regs) {
            c.push_back(bp[r]);
        }
        if (toXfb) {
            frame->xfbCopies++;
            return;
        }
        u32 addr = (bp[0x4B] & 0xFFFFFF) << 5;
        u32 w = (bp[0x4A] & 0x3FF) + 1, h = ((bp[0x4A] >> 10) & 0x3FF) + 1;
        u32 ctrl = bp[0x52];
        if ((ctrl >> 9) & 1) {  // half-size (box filtered) copy
            w = (w + 1) / 2;
            h = (h + 1) / 2;
        }
        u32 fmt = ((ctrl >> 4) & 7) | (((ctrl >> 3) & 1) << 3);
        EfbCopyInfo info{id, w, h, fmt, frameNumber, 0};
        info.memHash = hashBytes(memPtr(addr), 64);
        efbCopies[addr] = info;
        texDirty = true;
        bindEpoch++;
    }

    void loadTlut(u32 regValue) override {
        noteListLoad();
        u32 src = (g.bp[0x64] & 0xFFFFFF) << 5;
        u32 tmem = (regValue & 0x3FF) << 9;
        u32 lines = (regValue >> 10) & 0x7FF;
        u32 bytes = lines * 32;
        if (tmem + bytes > sizeof(tlutMem)) {
            bytes = sizeof(tlutMem) - (tmem & (sizeof(tlutMem) - 1));
        }
        memcpy(tlutMem + (tmem & (sizeof(tlutMem) - 1)), memPtr(src), bytes);
        texDirty = true;
        bindEpoch++;
    }

    void preloadTexture() override {}
    u32 peekColor(u16, u16) override { return 0; }
    u32 peekZ(u16, u16) override { return 0x00FFFFFF; }

    // Texture maps referenced by the current TEV / indirect configuration.
    u32 usedTexMaps() const {
        const u32* bp = g.bp;
        u32 genMode = bp[0x00];
        u32 numTev = ((genMode >> 10) & 15) + 1;
        u32 numInd = (genMode >> 16) & 7;
        u32 mask = 0;
        for (u32 s = 0; s < numTev; s++) {
            u32 order = bp[0x28 + s / 2] >> ((s & 1) * 12);
            if ((order >> 6) & 1) {
                mask |= 1u << (order & 7);
            }
        }
        u32 iref = bp[0x27];
        for (u32 s = 0; s < numInd; s++) {
            mask |= 1u << ((iref >> (s * 6)) & 7);
        }
        return mask;
    }

    void resolveTextures() {
        if (!texDirty) {
            return;
        }
        texDirty = false;
        u32 mask = usedTexMaps();
        for (u32 unit = 0; unit < 8; unit++) {
            if (!(mask & (1u << unit))) {
                continue;
            }
            const u32* bp = g.bp;
            u32 rb = kTexRegBase[unit];
            UnitMemo& m = unitMemo[unit];
            u32 binding;
            if (m.epoch == bindEpoch && m.mode0 == bp[rb] && m.mode1 == bp[rb + 4] && m.image0 == bp[rb + 8] && m.image3 == bp[rb + 0x14] &&
                m.tlut == bp[rb + 0x18]) {
                binding = m.binding;
            } else {
                binding = resolveUnit(unit);
                m = UnitMemo{bp[rb], bp[rb + 4], bp[rb + 8], bp[rb + 0x14], bp[rb + 0x18], binding, bindEpoch};
            }
            if (binding != boundTex[unit]) {
                boundTex[unit] = binding;
                auto& c = cmds();
                if (binding & 0x80000000u) {
                    const EfbCopyInfo* info = nullptr;
                    u32 addr = 0;
                    for (auto& kv : efbCopies) {
                        if (kv.second.id == (binding & 0x7FFFFFFF)) {
                            info = &kv.second;
                            addr = kv.first;
                            break;
                        }
                    }
                    c.push_back(CMD_TEX_EFB);
                    c.push_back(unit);
                    c.push_back(binding & 0x7FFFFFFF);
                    c.push_back(info ? info->width : 1);
                    c.push_back(info ? info->height : 1);
                    c.push_back(addr);
                } else {
                    c.push_back(CMD_TEX);
                    c.push_back(unit);
                    c.push_back(binding);
                }
            }
        }
    }

    u32 resolveUnit(u32 unit) {
        const u32* bp = g.bp;
        u32 rb = kTexRegBase[unit];
        u32 mode0 = bp[rb], mode1 = bp[rb + 4], image0 = bp[rb + 8], image3 = bp[rb + 0x14], tlutReg = bp[rb + 0x18];
        u32 width = (image0 & 0x3FF) + 1, height = ((image0 >> 10) & 0x3FF) + 1, fmt = (image0 >> 20) & 15;
        u32 addr = (image3 & 0xFFFFFF) << 5;

        auto copy = efbCopies.find(addr);
        if (copy != efbCopies.end()) {
            // Still the copy unless the CPU has since written new data there.
            if (hashBytes(memPtr(addr), 64) == copy->second.memHash) {
                return 0x80000000u | copy->second.id;
            }
            efbCopies.erase(copy);
        }

        u32 minFilter = (mode0 >> 5) & 7;
        u32 maxLod = (mode1 >> 8) & 0xFF;
        u32 levels = 1;
        if (minFilter != 0 && minFilter != 4) {  // mipmapped filter
            levels = maxLod / 16 + 1;
            u32 maxLevels = 1;
            for (u32 w = width, h = height; w > 1 || h > 1; w = w > 1 ? w / 2 : 1, h = h > 1 ? h / 2 : 1) {
                maxLevels++;
            }
            if (levels > maxLevels) {
                levels = maxLevels;
            }
        }
        bool ci = fmt == TF_CI4 || fmt == TF_CI8 || fmt == TF_CI14X2;
        TexKey key{addr, fmt, width, height, levels, ci ? ((tlutReg & 0x3FF) << 9) : 0u, ci ? ((tlutReg >> 10) & 3) : 0u};
        TexEntry& e = texCache[key];

        size_t total = 0;
        for (u32 l = 0, w = width, h = height; l < levels; l++, w = w > 1 ? w / 2 : 1, h = h > 1 ? h / 2 : 1) {
            total += texLevelSize(fmt, w, h);
        }
        if (e.checkedFrame != frameNumber || e.checkedInval != texInvalEpoch) {
            e.checkedFrame = frameNumber;
            e.checkedInval = texInvalEpoch;
            const u8* src = memPtr(addr);
            u64 h = hashBytes(src, total);
            if (ci) {
                u32 entries = fmt == TF_CI4 ? 16 : fmt == TF_CI8 ? 256 : 16384;
                h ^= hashBytes(tlutMem + (key.tlutAddr & (sizeof(tlutMem) - 1)), entries * 2) * 31;
            }
            if (!e.image || h != e.dataHash) {
                // Images are immutable once handed to a frame: build a new one.
                e.dataHash = h;
                e.image = std::make_shared< TexImage >();
                decode(*e.image, src, key);
            }
        }

        auto it = frameTexIndex.find(e.image.get());
        if (it != frameTexIndex.end()) {
            return it->second;
        }
        u32 index = (u32)frame->textures.size();
        frame->textures.push_back(e.image);
        frameTexIndex[e.image.get()] = index;
        return index;
    }

    void decode(TexImage& img, const u8* src, const TexKey& key) {
        size_t texels = 0;
        for (u32 l = 0, w = key.width, h = key.height; l < key.levels; l++, w = w > 1 ? w / 2 : 1, h = h > 1 ? h / 2 : 1) {
            texels += (size_t)w * h;
        }
        img.rgba.resize(texels);
        img.width = key.width;
        img.height = key.height;
        img.levels = key.levels;
        const u16* tlut = (const u16*)(tlutMem + (key.tlutAddr & (sizeof(tlutMem) - 1)));
        u32* dst = img.rgba.data();
        const u8* p = src;
        for (u32 l = 0, w = key.width, h = key.height; l < key.levels; l++, w = w > 1 ? w / 2 : 1, h = h > 1 ? h / 2 : 1) {
            decodeTexture((u8*)dst, p, key.fmt, w, h, tlut, key.tlutFmt);
            dst += (size_t)w * h;
            p += texLevelSize(key.fmt, w, h);
        }
        img.version++;
    }

    // Called when the game presents a frame (VI flip).  A frame is complete
    // once it contains the EFB->XFB display copy.
    void endFrame() {
        if (frame->xfbCopies == 0 && frame->cmds.size() < (16u << 20)) {
            return;
        }
        frame->number = frameNumber++;
        static const bool perfLog = getenv("PETARI_PERFLOG") != nullptr;
        if (perfLog && frame->number % 600 == 0) {
            port_log("perf: recorded frame %llu: %u draws, %zu vertex words, %zu command words, %zu textures", (unsigned long long)frame->number,
                     frame->draws, frame->verts.size(), frame->cmds.size(), frame->textures.size());
            size_t entries = 0, bytes = 0;
            for (auto& kv : dlCache) {
                for (DlEntry& e : kv.second.ways) {
                    entries++;
                    bytes += e.bytes.capacity() + e.words.capacity() * 4 + e.draws.capacity() * sizeof(DlDraw);
                }
            }
            const DlStats& s = dlStats;
            port_log("perf: display lists per frame: %llu calls, %llu from the cache, %llu recorded; %llu KB of arrays hashed, %llu KB of vertices "
                     "copied, %llu KB decoded; cache %zu lists, %zu MB",
                     (unsigned long long)(s.calls / 600), (unsigned long long)(s.hits / 600), (unsigned long long)(s.recorded / 600),
                     (unsigned long long)(s.hashedBytes / 600 / 1024), (unsigned long long)(s.copiedWords * 4 / 600 / 1024),
                     (unsigned long long)(s.decodedWords * 4 / 600 / 1024), entries, bytes >> 20);
            dlStats = DlStats();
        }
        // Lists not called for 10 s leave the display list cache.
        if (frameNumber % 120 == 0) {
            for (auto it = dlCache.begin(); it != dlCache.end();) {
                std::vector<DlEntry>& ways = it->second.ways;
                for (size_t i = 0; i < ways.size();) {
                    if (ways[i].lastUsed + 600 < frameNumber) {
                        ways.erase(ways.begin() + (long)i);
                    } else {
                        i++;
                    }
                }
                if (ways.empty() && it->second.noDrawsUntil < frameNumber) {
                    it = dlCache.erase(it);
                } else {
                    ++it;
                }
            }
        }
        frame = frameQueue().publish(std::move(frame));
        resetBindings();
        snapshot();
        // Drop EFB copy associations that were not refreshed recently.
        for (auto it = efbCopies.begin(); it != efbCopies.end();) {
            if (frameNumber - it->second.frame > 8) {
                it = efbCopies.erase(it);
            } else {
                ++it;
            }
        }
        // Keep the texture cache bounded.
        if (texCache.size() > 1024) {  // a scene uses a few hundred
            for (auto it = texCache.begin(); it != texCache.end();) {
                if (frameNumber - it->second.checkedFrame > 600) {
                    it = texCache.erase(it);
                } else {
                    ++it;
                }
            }
        }
    }
};

Recorder* sRecorder;

}  // namespace

void useRecorderBackend() {
    if (!sRecorder) {
        sRecorder = new Recorder();
    }
    setBackend(sRecorder);
}

}  // namespace gpu

extern "C" void port_gx_use_recorder_backend(void) { gpu::useRecorderBackend(); }

// Number the frame being recorded will carry (gpu::Renderer::frameNumber()
// once it is shown).
extern "C" uint64_t port_gx_recording_frame(void) { return gpu::sRecorder ? gpu::sRecorder->frameNumber : 0; }

// Frame boundary: VI flip of a new XFB.
extern "C" void port_gx_recorder_end_frame(void) {
    PortHostAllocScope hostAlloc;
    if (gpu::sRecorder && gpu::backend() == gpu::sRecorder) {
        gpu::sRecorder->endFrame();
    }
}

extern "C" void __PortGXFifoFlush(void);

// Markers inserted by the game code (e.g. around 2D/HUD drawing).  Pending
// FIFO commands are processed first so the marker lands after them.
extern "C" void port_gx_marker(uint32_t kind) {
    __PortGXFifoFlush();
    PortHostAllocScope hostAlloc;
    if (gpu::sRecorder && gpu::backend() == gpu::sRecorder) {
        gpu::sRecorder->cmds().push_back(gpu::CMD_MARKER);
        gpu::sRecorder->cmds().push_back(kind);
    }
}

// The main 3D camera of the frame (see CMD_CAMERA).
extern "C" void port_gx_camera(const float* camera) {
    __PortGXFifoFlush();
    PortHostAllocScope hostAlloc;
    if (gpu::sRecorder && gpu::backend() == gpu::sRecorder) {
        gpu::WordBuffer& cmds = gpu::sRecorder->cmds();
        cmds.push_back(gpu::CMD_CAMERA);
        const uint32_t* w = (const uint32_t*)camera;
        cmds.insert(cmds.end(), w, w + gpu::kCameraWords);
    }
}
