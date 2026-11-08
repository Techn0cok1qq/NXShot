// pngsave.h - 画像をPNGで保存する(zlibだけを使う。SDL2_imageの保存機能は使わない)
// RGBA(1画素4バイト)を受け取り、透明度は捨てて RGB で書き出す。
#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <vector>
#include <zlib.h>

namespace pngsave {

static inline void put32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

static inline bool writeChunk(FILE *f, const char *type, const unsigned char *data, uint32_t len) {
    unsigned char hdr[8];
    put32(hdr, len);
    memcpy(hdr + 4, type, 4);
    if (fwrite(hdr, 1, 8, f) != 8) return false;
    if (len && fwrite(data, 1, len, f) != len) return false;
    uLong crc = crc32(0L, (const Bytef *)type, 4);
    if (len) crc = crc32(crc, data, len);
    unsigned char c[4];
    put32(c, (uint32_t)crc);
    return fwrite(c, 1, 4, f) == 4;
}

// 保存できたら true
static inline bool saveRGBA(const char *path, const unsigned char *rgba, int w, int h) {
    if (!path || !rgba || w <= 0 || h <= 0) return false;
    const size_t stride = (size_t)w * 3 + 1;  // 各行の先頭に、フィルタの種類(1バイト)
    const size_t rawLen = stride * (size_t)h;
    std::vector<unsigned char> raw(rawLen);
    for (int y = 0; y < h; y++) {
        const unsigned char *src = rgba + (size_t)y * w * 4;
        unsigned char *row = raw.data() + (size_t)y * stride;
        row[0] = 1;  // Sub: 左の画素との差
        unsigned char *d = row + 1;
        for (int x = 0; x < w; x++) {
            for (int c = 0; c < 3; c++) {
                unsigned char cur = src[x * 4 + c];
                unsigned char left = x ? src[(x - 1) * 4 + c] : 0;
                d[x * 3 + c] = (unsigned char)(cur - left);
            }
        }
    }
    uLongf zlen = compressBound((uLong)rawLen);
    std::vector<unsigned char> z(zlen);
    if (compress2(z.data(), &zlen, raw.data(), (uLong)rawLen, 5) != Z_OK) return false;

    FILE *f = fopen(path, "wb");
    if (!f) return false;
    static const unsigned char sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    unsigned char ihdr[13];
    put32(ihdr, (uint32_t)w);
    put32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8;   // 1色8ビット
    ihdr[9] = 2;   // RGB
    ihdr[10] = 0;  // 圧縮方式
    ihdr[11] = 0;  // フィルタ方式
    ihdr[12] = 0;  // インターレースなし
    bool ok = fwrite(sig, 1, 8, f) == 8 && writeChunk(f, "IHDR", ihdr, 13) && writeChunk(f, "IDAT", z.data(), (uint32_t)zlen) &&
              writeChunk(f, "IEND", nullptr, 0);
    if (fclose(f) != 0) ok = false;
    if (!ok) remove(path);
    return ok;
}

}  // namespace pngsave
