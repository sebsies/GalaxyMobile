// GX texture decoders.  Textures are stored in tiles (blocks) whose size
// depends on the texel size; all multi-byte values are big-endian.
#include "tex_decode.h"

#include <string.h>

namespace gpu {

namespace {

struct Block {
    uint32_t w, h, bytes;
};

Block blockOf(uint32_t fmt) {
    switch (fmt) {
    case TF_I4:
    case TF_CI4:
    case TF_CMPR:
        return {8, 8, 32};
    case TF_I8:
    case TF_IA4:
    case TF_CI8:
        return {8, 4, 32};
    case TF_IA8:
    case TF_RGB565:
    case TF_RGB5A3:
    case TF_CI14X2:
        return {4, 4, 32};
    case TF_RGBA8:
        return {4, 4, 64};
    default:
        return {4, 4, 32};
    }
}

inline uint16_t be16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }

inline uint32_t rgba(uint32_t r, uint32_t g, uint32_t b, uint32_t a) { return r | (g << 8) | (b << 16) | (a << 24); }

inline uint32_t ext5(uint32_t v) { return (v << 3) | (v >> 2); }
inline uint32_t ext6(uint32_t v) { return (v << 2) | (v >> 4); }
inline uint32_t ext4(uint32_t v) { return (v << 4) | v; }
inline uint32_t ext3(uint32_t v) { return (v << 5) | (v << 2) | (v >> 1); }

inline uint32_t div3(uint32_t v) { return (v * 0xAAABu) >> 17; }

inline uint32_t fromRGB565(uint16_t c) { return rgba(ext5(c >> 11), ext6((c >> 5) & 63), ext5(c & 31), 255); }

inline uint32_t fromRGB5A3(uint16_t c) {
    if (c & 0x8000) {
        return rgba(ext5((c >> 10) & 31), ext5((c >> 5) & 31), ext5(c & 31), 255);
    }
    return rgba(ext4((c >> 8) & 15), ext4((c >> 4) & 15), ext4(c & 15), ext3((c >> 12) & 7));
}

inline uint32_t fromIA8(uint16_t c) {
    uint32_t i = c & 0xFF, a = c >> 8;
    return rgba(i, i, i, a);
}

inline uint32_t fromTlut(uint16_t c, uint32_t tlutFmt) {
    switch (tlutFmt) {
    case TLUT_IA8:
        return fromIA8(c);
    case TLUT_RGB565:
        return fromRGB565(c);
    default:
        return fromRGB5A3(c);
    }
}

inline void makeCmprPal(const uint8_t* src, uint32_t pal[4]) {
    uint16_t c0 = be16(src), c1 = be16(src + 2);
    uint32_t r0 = ext5(c0 >> 11), g0 = ext6((c0 >> 5) & 63), b0 = ext5(c0 & 31);
    uint32_t r1 = ext5(c1 >> 11), g1 = ext6((c1 >> 5) & 63), b1 = ext5(c1 & 31);
    pal[0] = rgba(r0, g0, b0, 255);
    pal[1] = rgba(r1, g1, b1, 255);
    if (c0 > c1) {
        pal[2] = rgba(div3(2 * r0 + r1), div3(2 * g0 + g1), div3(2 * b0 + b1), 255);
        pal[3] = rgba(div3(r0 + 2 * r1), div3(g0 + 2 * g1), div3(b0 + 2 * b1), 255);
    } else {
        pal[2] = rgba((r0 + r1) >> 1, (g0 + g1) >> 1, (b0 + b1) >> 1, 255);
        pal[3] = rgba((r0 + r1) >> 1, (g0 + g1) >> 1, (b0 + b1) >> 1, 0);
    }
}

// Decodes one 4x4 CMPR (S3TC/DXT1-style) sub-block (unclipped).
inline void decodeCmprSubFast(uint32_t* out, uint32_t stride, const uint8_t* src) {
    uint32_t pal[4];
    makeCmprPal(src, pal);
    for (uint32_t y = 0; y < 4; y++) {
        uint8_t bits = src[4 + y];
        uint32_t* row = out + y * stride;
        row[0] = pal[(bits >> 6) & 3];
        row[1] = pal[(bits >> 4) & 3];
        row[2] = pal[(bits >> 2) & 3];
        row[3] = pal[bits & 3];
    }
}

// Decodes one 4x4 CMPR sub-block with bounds check for edge tiles.
inline void decodeCmprSub(uint32_t* out, uint32_t stride, const uint8_t* src, uint32_t maxW, uint32_t maxH) {
    if (maxW >= 4 && maxH >= 4) {
        decodeCmprSubFast(out, stride, src);
        return;
    }
    uint32_t pal[4];
    makeCmprPal(src, pal);
    for (uint32_t y = 0; y < 4 && y < maxH; y++) {
        uint8_t bits = src[4 + y];
        for (uint32_t x = 0; x < 4 && x < maxW; x++) {
            out[y * stride + x] = pal[(bits >> (6 - 2 * x)) & 3];
        }
    }
}

template <uint32_t BW, uint32_t BH, uint32_t BBYTES, typename TexelFn>
void decodeBlocks(uint32_t* dst, const uint8_t* src, uint32_t width, uint32_t height, TexelFn&& texelFn) {
    uint32_t bw = (width + BW - 1) / BW;
    uint32_t bh = (height + BH - 1) / BH;
    const uint8_t* p = src;
    if ((width % BW) == 0 && (height % BH) == 0) {
        for (uint32_t by = 0; by < bh; by++) {
            uint32_t* rowBase = dst + by * BH * width;
            for (uint32_t bx = 0; bx < bw; bx++, p += BBYTES) {
                uint32_t* blockDst = rowBase + bx * BW;
                for (uint32_t ty = 0; ty < BH; ty++) {
                    uint32_t* line = blockDst + ty * width;
                    uint32_t base = ty * BW;
                    for (uint32_t tx = 0; tx < BW; tx++) {
                        line[tx] = texelFn(p, base + tx);
                    }
                }
            }
        }
    } else {
        for (uint32_t by = 0; by < bh; by++) {
            uint32_t y0 = by * BH;
            for (uint32_t bx = 0; bx < bw; bx++, p += BBYTES) {
                uint32_t x0 = bx * BW;
                for (uint32_t ty = 0; ty < BH; ty++) {
                    uint32_t y = y0 + ty;
                    if (y >= height) continue;
                    uint32_t* line = dst + y * width;
                    uint32_t base = ty * BW;
                    for (uint32_t tx = 0; tx < BW; tx++) {
                        uint32_t x = x0 + tx;
                        if (x < width) {
                            line[x] = texelFn(p, base + tx);
                        }
                    }
                }
            }
        }
    }
}

}  // namespace

size_t texLevelSize(uint32_t fmt, uint32_t width, uint32_t height) {
    Block b = blockOf(fmt);
    uint32_t bw = (width + b.w - 1) / b.w, bh = (height + b.h - 1) / b.h;
    return (size_t)bw * bh * b.bytes;
}

void decodeTexture(uint8_t* dstBytes, const uint8_t* src, uint32_t fmt, uint32_t width, uint32_t height, const uint16_t* tlut, uint32_t tlutFmt) {
    uint32_t* dst = (uint32_t*)dstBytes;
    switch (fmt) {
    case TF_CMPR: {
        uint32_t bw = (width + 7) / 8, bh = (height + 7) / 8;
        const uint8_t* p = src;
        if ((width & 7) == 0 && (height & 7) == 0) {
            for (uint32_t by = 0; by < bh; by++) {
                uint32_t* rowBase = dst + by * 8 * width;
                for (uint32_t bx = 0; bx < bw; bx++, p += 32) {
                    uint32_t* blockDst = rowBase + bx * 8;
                    decodeCmprSubFast(blockDst, width, p);
                    decodeCmprSubFast(blockDst + 4, width, p + 8);
                    decodeCmprSubFast(blockDst + 4 * width, width, p + 16);
                    decodeCmprSubFast(blockDst + 4 * width + 4, width, p + 24);
                }
            }
        } else {
            for (uint32_t by = 0; by < bh; by++) {
                uint32_t y0 = by * 8;
                for (uint32_t bx = 0; bx < bw; bx++, p += 32) {
                    uint32_t x0 = bx * 8;
                    for (uint32_t s = 0; s < 4; s++) {
                        uint32_t sx = x0 + (s & 1) * 4, sy = y0 + (s >> 1) * 4;
                        if (sx < width && sy < height) {
                            decodeCmprSub(dst + sy * width + sx, width, p + s * 8, width - sx, height - sy);
                        }
                    }
                }
            }
        }
        break;
    }
    case TF_I4:
        decodeBlocks<8, 8, 32>(dst, src, width, height, [](const uint8_t* p, uint32_t i) {
            uint32_t v = ext4((p[i >> 1] >> ((i & 1) ? 0 : 4)) & 15);
            return rgba(v, v, v, v);
        });
        break;
    case TF_I8:
        decodeBlocks<8, 4, 32>(dst, src, width, height, [](const uint8_t* p, uint32_t i) {
            uint32_t v = p[i];
            return rgba(v, v, v, v);
        });
        break;
    case TF_IA4:
        decodeBlocks<8, 4, 32>(dst, src, width, height, [](const uint8_t* p, uint32_t i) {
            uint32_t v = p[i];
            uint32_t in = ext4(v & 15), a = ext4(v >> 4);
            return rgba(in, in, in, a);
        });
        break;
    case TF_IA8:
        decodeBlocks<4, 4, 32>(dst, src, width, height, [](const uint8_t* p, uint32_t i) {
            return fromIA8(be16(p + i * 2));
        });
        break;
    case TF_RGB565:
        decodeBlocks<4, 4, 32>(dst, src, width, height, [](const uint8_t* p, uint32_t i) {
            return fromRGB565(be16(p + i * 2));
        });
        break;
    case TF_RGB5A3:
        decodeBlocks<4, 4, 32>(dst, src, width, height, [](const uint8_t* p, uint32_t i) {
            return fromRGB5A3(be16(p + i * 2));
        });
        break;
    case TF_RGBA8:
        decodeBlocks<4, 4, 64>(dst, src, width, height, [](const uint8_t* p, uint32_t i) {
            const uint8_t* ar = p + i * 2;
            const uint8_t* gb = p + 32 + i * 2;
            return rgba(ar[1], gb[0], gb[1], ar[0]);
        });
        break;
    case TF_CI4: {
        uint32_t pal[16];
        for (int i = 0; i < 16; i++) {
            pal[i] = tlut ? fromTlut(be16((const uint8_t*)(tlut + i)), tlutFmt) : rgba((uint32_t)i * 17, (uint32_t)i * 17, (uint32_t)i * 17, 255);
        }
        decodeBlocks<8, 8, 32>(dst, src, width, height, [&pal](const uint8_t* p, uint32_t i) {
            uint32_t idx = (p[i >> 1] >> ((i & 1) ? 0 : 4)) & 15;
            return pal[idx];
        });
        break;
    }
    case TF_CI8: {
        uint32_t pal[256];
        for (int i = 0; i < 256; i++) {
            pal[i] = tlut ? fromTlut(be16((const uint8_t*)(tlut + i)), tlutFmt) : rgba((uint32_t)i, (uint32_t)i, (uint32_t)i, 255);
        }
        decodeBlocks<8, 4, 32>(dst, src, width, height, [&pal](const uint8_t* p, uint32_t i) {
            return pal[p[i]];
        });
        break;
    }
    case TF_CI14X2:
        decodeBlocks<4, 4, 32>(dst, src, width, height, [tlut, tlutFmt](const uint8_t* p, uint32_t i) {
            uint32_t idx = be16(p + i * 2) & 0x3FFF;
            return tlut ? fromTlut(be16((const uint8_t*)(tlut + idx)), tlutFmt) : rgba(255, 0, 255, 255);
        });
        break;
    default:
        decodeBlocks<4, 4, 32>(dst, src, width, height, [](const uint8_t*, uint32_t) {
            return rgba(255, 0, 255, 255);
        });
        break;
    }
}

}  // namespace gpu