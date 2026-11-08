// imgops.h - 写真の編集に使う、画素の処理(SDLなどに依存しない。RGBA 1画素4バイト)
//   切り抜き / モザイク / ぼかし / モーションブラー / 文字(アルファの型紙を重ねる)
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace imgops {

struct Img {
    int w = 0, h = 0;
    std::vector<uint8_t> px;  // w*h*4 (RGBA)
    void alloc(int W, int H) { w = W; h = H; px.assign((size_t)W * H * 4, 255); }
    uint8_t *at(int x, int y) { return &px[((size_t)y * w + x) * 4]; }
    const uint8_t *at(int x, int y) const { return &px[((size_t)y * w + x) * 4]; }
};

struct Rect { int x = 0, y = 0, w = 0, h = 0; };

// 画像の中に収める(はみ出した分は切る)。中身が無くなったら w=h=0
inline Rect clampRect(Rect r, int W, int H) {
    int x0 = std::max(0, r.x), y0 = std::max(0, r.y);
    int x1 = std::min(W, r.x + r.w), y1 = std::min(H, r.y + r.h);
    Rect o;
    if (x1 > x0 && y1 > y0) { o.x = x0; o.y = y0; o.w = x1 - x0; o.h = y1 - y0; }
    return o;
}

// 切り抜き
inline bool crop(const Img &src, Rect r, Img &out) {
    r = clampRect(r, src.w, src.h);
    if (r.w < 2 || r.h < 2) return false;
    out.alloc(r.w, r.h);
    for (int y = 0; y < r.h; y++) memcpy(out.at(0, y), src.at(r.x, r.y + y), (size_t)r.w * 4);
    return true;
}

// モザイク: block x block の四角ごとに、平均の色で塗る
inline void mosaic(Img &im, Rect r, int block) {
    r = clampRect(r, im.w, im.h);
    if (r.w == 0 || block < 2) return;
    for (int by = r.y; by < r.y + r.h; by += block) {
        for (int bx = r.x; bx < r.x + r.w; bx += block) {
            int x1 = std::min(bx + block, r.x + r.w), y1 = std::min(by + block, r.y + r.h);
            unsigned s[3] = {0, 0, 0}, n = 0;
            for (int y = by; y < y1; y++)
                for (int x = bx; x < x1; x++) { const uint8_t *p = im.at(x, y); s[0] += p[0]; s[1] += p[1]; s[2] += p[2]; n++; }
            uint8_t c[3] = {(uint8_t)(s[0] / n), (uint8_t)(s[1] / n), (uint8_t)(s[2] / n)};
            for (int y = by; y < y1; y++)
                for (int x = bx; x < x1; x++) { uint8_t *p = im.at(x, y); p[0] = c[0]; p[1] = c[1]; p[2] = c[2]; }
        }
    }
}

// ぼかし: 範囲の中を、箱形のぼかし(横→縦)を passes 回くり返す(2〜3回でなめらかになる)
inline void boxBlur(Img &im, Rect r, int radius, int passes = 3) {
    r = clampRect(r, im.w, im.h);
    if (r.w == 0 || radius < 1) return;
    int W = r.w, H = r.h;
    std::vector<float> a((size_t)W * H * 3), b((size_t)W * H * 3);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            const uint8_t *p = im.at(r.x + x, r.y + y);
            float *q = &a[((size_t)y * W + x) * 3];
            q[0] = p[0]; q[1] = p[1]; q[2] = p[2];
        }
    for (int ps = 0; ps < passes; ps++) {
        // 横
        for (int y = 0; y < H; y++) {
            for (int c = 0; c < 3; c++) {
                float sum = 0;
                for (int k = -radius; k <= radius; k++) sum += a[((size_t)y * W + std::min(W - 1, std::max(0, k))) * 3 + c];
                for (int x = 0; x < W; x++) {
                    b[((size_t)y * W + x) * 3 + c] = sum / (2 * radius + 1);
                    int xo = std::max(0, x - radius), xi = std::min(W - 1, x + radius + 1);
                    sum += a[((size_t)y * W + xi) * 3 + c] - a[((size_t)y * W + xo) * 3 + c];
                }
            }
        }
        // 縦
        for (int x = 0; x < W; x++) {
            for (int c = 0; c < 3; c++) {
                float sum = 0;
                for (int k = -radius; k <= radius; k++) sum += b[((size_t)std::min(H - 1, std::max(0, k)) * W + x) * 3 + c];
                for (int y = 0; y < H; y++) {
                    a[((size_t)y * W + x) * 3 + c] = sum / (2 * radius + 1);
                    int yo = std::max(0, y - radius), yi = std::min(H - 1, y + radius + 1);
                    sum += b[((size_t)yi * W + x) * 3 + c] - b[((size_t)yo * W + x) * 3 + c];
                }
            }
        }
    }
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            uint8_t *p = im.at(r.x + x, r.y + y);
            const float *q = &a[((size_t)y * W + x) * 3];
            p[0] = (uint8_t)std::min(255.f, q[0] + 0.5f);
            p[1] = (uint8_t)std::min(255.f, q[1] + 0.5f);
            p[2] = (uint8_t)std::min(255.f, q[2] + 0.5f);
        }
}

// モーションブラー: 角度 angleDeg(0=横、90=縦)の向きに、長さ length 画素ぶん平均する。
// 範囲の外の画素も読む(はみ出す部分は画像の端の色)。
inline void motionBlur(Img &im, Rect r, int length, int angleDeg) {
    r = clampRect(r, im.w, im.h);
    if (r.w == 0 || length < 2) return;
    Img src = im;
    double rad = angleDeg * 3.14159265358979 / 180.0;
    double dx = std::cos(rad), dy = std::sin(rad);
    int n = length;
    for (int y = r.y; y < r.y + r.h; y++) {
        for (int x = r.x; x < r.x + r.w; x++) {
            unsigned s0 = 0, s1 = 0, s2 = 0;
            for (int k = 0; k < n; k++) {
                double t = k - (n - 1) / 2.0;
                int sx = (int)std::lround(x + dx * t), sy = (int)std::lround(y + dy * t);
                sx = std::min(src.w - 1, std::max(0, sx));
                sy = std::min(src.h - 1, std::max(0, sy));
                const uint8_t *p = src.at(sx, sy);
                s0 += p[0]; s1 += p[1]; s2 += p[2];
            }
            uint8_t *q = im.at(x, y);
            q[0] = (uint8_t)(s0 / n); q[1] = (uint8_t)(s1 / n); q[2] = (uint8_t)(s2 / n);
        }
    }
}

// ---- 文字 ----
// 型紙(アルファ値だけの画像)。文字を描いた画像から、アルファだけを取り出して使う
struct Mask {
    int w = 0, h = 0;
    std::vector<uint8_t> a;
};

// 型紙を r 画素ふくらませる(ふちどり用)。結果の大きさは (w+2r) x (h+2r)
inline Mask dilate(const Mask &m, int r) {
    Mask o;
    if (r <= 0) { o = m; return o; }
    o.w = m.w + 2 * r; o.h = m.h + 2 * r;
    o.a.assign((size_t)o.w * o.h, 0);
    std::vector<int> offs;  // 円形のぼかし範囲
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++)
            if (dx * dx + dy * dy <= r * r + r / 2) offs.push_back(dy * 100000 + dx);
    for (int y = 0; y < m.h; y++)
        for (int x = 0; x < m.w; x++) {
            uint8_t v = m.a[(size_t)y * m.w + x];
            if (!v) continue;
            for (int code : offs) {
                int dx = ((code + 50000) % 100000 + 100000) % 100000 - 50000;
                int dy = (code - dx) / 100000;
                size_t idx = (size_t)(y + r + dy) * o.w + (x + r + dx);
                if (o.a[idx] < v) o.a[idx] = v;
            }
        }
    return o;
}

// 型紙を、色 (cr,cg,cb) で、画像の (x,y) に重ねる(型紙の左上が (x,y))
inline void blendMask(Img &im, const Mask &m, int x, int y, uint8_t cr, uint8_t cg, uint8_t cb) {
    for (int my = 0; my < m.h; my++) {
        int iy = y + my;
        if (iy < 0 || iy >= im.h) continue;
        for (int mx = 0; mx < m.w; mx++) {
            int ix = x + mx;
            if (ix < 0 || ix >= im.w) continue;
            unsigned a = m.a[(size_t)my * m.w + mx];
            if (!a) continue;
            uint8_t *p = im.at(ix, iy);
            p[0] = (uint8_t)((p[0] * (255 - a) + cr * a + 127) / 255);
            p[1] = (uint8_t)((p[1] * (255 - a) + cg * a + 127) / 255);
            p[2] = (uint8_t)((p[2] * (255 - a) + cb * a + 127) / 255);
        }
    }
}

// 文字の型紙を、ふち(outline 画素。0ならなし)つきで重ねる。(x,y) は文字の左上
inline void drawTextMask(Img &im, const Mask &text, int x, int y, const uint8_t col[3], int outline, const uint8_t ocol[3]) {
    if (outline > 0) {
        Mask o = dilate(text, outline);
        blendMask(im, o, x - outline, y - outline, ocol[0], ocol[1], ocol[2]);
    }
    blendMask(im, text, x, y, col[0], col[1], col[2]);
}

}  // namespace imgops
