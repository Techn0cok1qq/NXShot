// mp4trim.h - mp4の「最初のほうを削って」指定サイズ以内に収める(再エンコードなし)
// libnx等に依存しない、標準Cライブラリ+STLだけのコード。
// 対応: ftyp / mdat / moov の構造で、ctts(B-frame)が無いmp4(Switchの動画)。編集リスト(edts)は出力では省く。
//
// 仕組み:
//   1. 動画のキーフレームを「切り始めの候補」にする
//   2. 候補ごとに「mdatの切り始め位置〜末尾をそのままコピーした場合の大きさ」を計算
//   3. 上限に収まる一番古い候補を選び、ftyp + 新しいmdat + 新しいmoov を書き出す
//   サンプルのオフセットは全部「一定の差」だけずれる。チャンク表は「1サンプル=1チャンク」に作り直す。
//
// もう1つの機能: cutRange() は「最初(秒)〜最後(秒)」を指定して切り出す(再エンコードなし)。
//   開始はキーフレームに切り下げる(指定より少し早くなる)。終了は、そのコマの手前まで(B-frameが無いので、どこでも切れる)。
//   probe() は、長さとキーフレームの時刻を返す(切り取り画面で使う)。
#pragma once
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

namespace mp4trim {

enum Result {
    OK = 0,             // 削って書き出した
    NOT_NEEDED = 1,     // もともと上限以内。何もしていない
    ERR_OPEN = -1,      // ファイルを開けない / 読めない
    ERR_FORMAT = -2,    // mp4の構造が想定外(未対応)
    ERR_TOO_BIG = -3,   // 削っても収まらない(残りが短すぎる)
    ERR_WRITE = -4,     // 書き込み失敗
    ERR_MEMORY = -5,
    ERR_RANGE = -6      // 切り出す範囲が正しくない(短すぎる、など)
};

struct Info {
    int64_t origSize = 0;
    int64_t newSize = 0;
    double cutSeconds = 0;      // 削った長さ(秒)
    double newSeconds = 0;      // 削ったあとの長さ(秒)
};

namespace detail {

typedef std::vector<uint8_t> Bytes;

static inline uint32_t rd32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static inline uint64_t rd64(const uint8_t *p) { return ((uint64_t)rd32(p) << 32) | rd32(p + 4); }
static inline void wr32(Bytes &b, uint32_t v) { b.push_back(v >> 24); b.push_back(v >> 16); b.push_back(v >> 8); b.push_back(v); }
static inline void wr64(Bytes &b, uint64_t v) { wr32(b, (uint32_t)(v >> 32)); wr32(b, (uint32_t)v); }
static inline void put32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static inline void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)(v >> 32)); put32(p + 4, (uint32_t)v); }
static inline uint32_t fourcc(const char *s) { return ((uint32_t)(uint8_t)s[0] << 24) | ((uint32_t)(uint8_t)s[1] << 16) | ((uint32_t)(uint8_t)s[2] << 8) | (uint8_t)s[3]; }

struct Box {
    uint32_t type = 0;
    bool container = false;
    Bytes payload;          // 葉のボックスの中身(ヘッダなし)
    std::vector<Box> kids;  // 入れ物ボックスの子
};

// 削ったあとのファイルでは意味が変わるので、出力しないボックス
static bool isDropped(uint32_t t) {
    return t == fourcc("edts") || t == fourcc("sbgp") || t == fourcc("sgpd");
}

static bool isContainer(uint32_t t) {
    return t == fourcc("moov") || t == fourcc("trak") || t == fourcc("mdia") || t == fourcc("minf") || t == fourcc("stbl");
}

static bool parseBoxes(const uint8_t *p, size_t len, std::vector<Box> &out, int depth) {
    if (depth > 8) return false;
    size_t i = 0;
    while (i + 8 <= len) {
        uint64_t size = rd32(p + i);
        uint32_t type = rd32(p + i + 4);
        size_t hdr = 8;
        if (size == 1) {
            if (i + 16 > len) return false;
            size = rd64(p + i + 8);
            hdr = 16;
        } else if (size == 0) {
            size = len - i;
        }
        if (size < hdr || i + size > len) return false;
        Box b;
        b.type = type;
        if (isContainer(type)) {
            b.container = true;
            if (!parseBoxes(p + i + hdr, (size_t)size - hdr, b.kids, depth + 1)) return false;
        } else {
            b.payload.assign(p + i + hdr, p + i + size);
        }
        out.push_back(std::move(b));
        i += (size_t)size;
    }
    return i == len;
}

static const Box *findKid(const Box &b, const char *t) {
    uint32_t f = fourcc(t);
    for (auto &k : b.kids) if (k.type == f) return &k;
    return nullptr;
}

struct Track {
    uint32_t handler = 0;
    uint32_t timescale = 0;
    bool hasStss = false;
    std::vector<uint32_t> size, dur;
    std::vector<uint8_t> sync;
    std::vector<uint64_t> off;
    std::vector<double> startSec;
    double totalSec = 0;
};

struct NewTrack {
    Bytes stts, stss, stsz, stsc, stco;
    uint64_t mediaDur = 0;
};

static bool loadTrack(const Box &trak, Track &t) {
    const Box *mdia = findKid(trak, "mdia");
    if (!mdia) return false;
    const Box *hdlr = findKid(*mdia, "hdlr");
    const Box *mdhd = findKid(*mdia, "mdhd");
    const Box *minf = findKid(*mdia, "minf");
    if (!hdlr || !mdhd || !minf || hdlr->payload.size() < 12 || mdhd->payload.size() < 24) return false;
    t.handler = rd32(&hdlr->payload[8]);
    const Bytes &mh = mdhd->payload;
    t.timescale = (mh[0] == 1) ? rd32(&mh[20]) : rd32(&mh[12]);
    if (t.timescale == 0) return false;
    const Box *stbl = findKid(*minf, "stbl");
    if (!stbl) return false;
    if (findKid(*stbl, "ctts")) return false;  // B-frameは未対応

    const Box *stts = findKid(*stbl, "stts"), *stsz = findKid(*stbl, "stsz"), *stsc = findKid(*stbl, "stsc");
    const Box *stco = findKid(*stbl, "stco"), *co64 = findKid(*stbl, "co64"), *stss = findKid(*stbl, "stss");
    if (!stts || !stsz || !stsc || (!stco && !co64)) return false;

    // サンプルの大きさ
    const Bytes &sz = stsz->payload;
    if (sz.size() < 12) return false;
    uint32_t fixed = rd32(&sz[4]), count = rd32(&sz[8]);
    if (fixed == 0 && sz.size() < 12 + (size_t)count * 4) return false;
    t.size.resize(count);
    for (uint32_t i = 0; i < count; i++) t.size[i] = fixed ? fixed : rd32(&sz[12 + (size_t)i * 4]);

    // サンプルの長さ
    const Bytes &st = stts->payload;
    if (st.size() < 8) return false;
    uint32_t ne = rd32(&st[4]);
    if (st.size() < 8 + (size_t)ne * 8) return false;
    t.dur.reserve(count);
    for (uint32_t e = 0; e < ne; e++) {
        uint32_t c = rd32(&st[8 + (size_t)e * 8]), d = rd32(&st[12 + (size_t)e * 8]);
        for (uint32_t k = 0; k < c && t.dur.size() < count; k++) t.dur.push_back(d);
    }
    if (t.dur.size() != count) return false;

    // キーフレーム
    t.sync.assign(count, stss ? 0 : 1);
    if (stss) {
        t.hasStss = true;
        const Bytes &ss = stss->payload;
        if (ss.size() < 8) return false;
        uint32_t n = rd32(&ss[4]);
        if (ss.size() < 8 + (size_t)n * 4) return false;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t idx = rd32(&ss[8 + (size_t)i * 4]);
            if (idx >= 1 && idx <= count) t.sync[idx - 1] = 1;
        }
    }

    // チャンクの位置
    std::vector<uint64_t> chunkOff;
    if (stco) {
        const Bytes &c = stco->payload;
        if (c.size() < 8) return false;
        uint32_t n = rd32(&c[4]);
        if (c.size() < 8 + (size_t)n * 4) return false;
        for (uint32_t i = 0; i < n; i++) chunkOff.push_back(rd32(&c[8 + (size_t)i * 4]));
    } else {
        const Bytes &c = co64->payload;
        if (c.size() < 8) return false;
        uint32_t n = rd32(&c[4]);
        if (c.size() < 8 + (size_t)n * 8) return false;
        for (uint32_t i = 0; i < n; i++) chunkOff.push_back(rd64(&c[8 + (size_t)i * 8]));
    }
    const Bytes &sc = stsc->payload;
    if (sc.size() < 8) return false;
    uint32_t nsc = rd32(&sc[4]);
    if (nsc == 0 || sc.size() < 8 + (size_t)nsc * 12) return false;
    t.off.reserve(count);
    uint32_t sampleIdx = 0;
    for (size_t ch = 0; ch < chunkOff.size(); ch++) {
        uint32_t spc = 0;
        for (uint32_t e = 0; e < nsc; e++) {
            uint32_t first = rd32(&sc[8 + (size_t)e * 12]);
            if (first <= ch + 1) spc = rd32(&sc[12 + (size_t)e * 12]);
        }
        uint64_t o = chunkOff[ch];
        for (uint32_t k = 0; k < spc && sampleIdx < count; k++) {
            t.off.push_back(o);
            o += t.size[sampleIdx];
            sampleIdx++;
        }
    }
    if (t.off.size() != count) return false;

    // 各サンプルの開始時刻
    t.startSec.resize(count);
    uint64_t cum = 0;
    for (uint32_t i = 0; i < count; i++) {
        t.startSec[i] = (double)cum / t.timescale;
        cum += t.dur[i];
    }
    t.totalSec = (double)cum / t.timescale;
    return true;
}

// 最初に時刻 >= T となるサンプル番号(無ければ count)
static size_t firstAtOrAfter(const Track &t, double T) {
    size_t lo = 0, hi = t.startSec.size();
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (t.startSec[mid] + 1e-9 < T) lo = mid + 1; else hi = mid;
    }
    return lo;
}

struct Plan {
    std::vector<size_t> first;   // トラックごとの、残す最初のサンプル
    int64_t cutOffset = 0;       // 元ファイルの、コピー開始位置
    double T = 0;
};

static void buildNewTrack(const Track &t, size_t first, size_t last, int64_t cutOffset, int64_t newDataStart, NewTrack &n) {
    size_t count = t.size.size();
    if (last > count) last = count;
    if (first > last) first = last;
    size_t keep = last - first;
    // stsz
    wr32(n.stsz, 0); wr32(n.stsz, 0); wr32(n.stsz, (uint32_t)keep);
    for (size_t i = first; i < last; i++) wr32(n.stsz, t.size[i]);
    // stco(1サンプル=1チャンク)
    wr32(n.stco, 0); wr32(n.stco, (uint32_t)keep);
    for (size_t i = first; i < last; i++) wr32(n.stco, (uint32_t)((int64_t)t.off[i] - cutOffset + newDataStart));
    // stsc
    wr32(n.stsc, 0); wr32(n.stsc, 1); wr32(n.stsc, 1); wr32(n.stsc, 1); wr32(n.stsc, 1);
    // stts(同じ長さをまとめる)
    Bytes runs;
    uint32_t nruns = 0;
    for (size_t i = first; i < last;) {
        size_t j = i;
        while (j < last && t.dur[j] == t.dur[i]) j++;
        wr32(runs, (uint32_t)(j - i)); wr32(runs, t.dur[i]);
        nruns++;
        i = j;
    }
    wr32(n.stts, 0); wr32(n.stts, nruns);
    n.stts.insert(n.stts.end(), runs.begin(), runs.end());
    // stss
    if (t.hasStss) {
        Bytes ss;
        uint32_t ns = 0;
        for (size_t i = first; i < last; i++) if (t.sync[i]) { wr32(ss, (uint32_t)(i - first + 1)); ns++; }
        wr32(n.stss, 0); wr32(n.stss, ns);
        n.stss.insert(n.stss.end(), ss.begin(), ss.end());
    }
    uint64_t md = 0;
    for (size_t i = first; i < last; i++) md += t.dur[i];
    n.mediaDur = md;
}

struct Ctx {
    const std::vector<Track> *tracks;
    const std::vector<NewTrack> *ntracks;
    uint32_t movieTimescale;
    uint64_t movieDur;
    int trakCounter;
};

static void setDurV01(Bytes &p, size_t off0, size_t off1, uint64_t dur) {
    // バージョン0は4バイト、1は8バイトの長さ
    if (p.empty()) return;
    if (p[0] == 1) { if (p.size() >= off1 + 8) put64(&p[off1], dur); }
    else { if (p.size() >= off0 + 4) put32(&p[off0], (uint32_t)dur); }
}

static void serialize(const Box &b, Bytes &out, Ctx &cx, int curTrak, uint32_t parentType) {
    if (isDropped(b.type)) return;
    if (b.container) {
        size_t start = out.size();
        wr32(out, 0); wr32(out, b.type);
        int trk = curTrak;
        if (b.type == fourcc("trak")) trk = cx.trakCounter++;
        for (auto &k : b.kids) serialize(k, out, cx, trk, b.type);
        put32(&out[start], (uint32_t)(out.size() - start));
        return;
    }
    Bytes pay;
    uint32_t type = b.type;
    const NewTrack *nt = (curTrak >= 0 && curTrak < (int)cx.ntracks->size()) ? &(*cx.ntracks)[curTrak] : nullptr;
    const Track *ot = (curTrak >= 0 && curTrak < (int)cx.tracks->size()) ? &(*cx.tracks)[curTrak] : nullptr;
    bool replaced = false;
    if (nt && parentType == fourcc("stbl")) {
        if (type == fourcc("stts")) { pay = nt->stts; replaced = true; }
        else if (type == fourcc("stss")) { pay = nt->stss; replaced = true; }
        else if (type == fourcc("stsz")) { pay = nt->stsz; replaced = true; }
        else if (type == fourcc("stsc")) { pay = nt->stsc; replaced = true; }
        else if (type == fourcc("stco") || type == fourcc("co64")) { pay = nt->stco; type = fourcc("stco"); replaced = true; }
    }
    if (!replaced) pay = b.payload;
    if (type == fourcc("mdhd") && nt && ot) {
        setDurV01(pay, 16, 24, nt->mediaDur);
    } else if (type == fourcc("tkhd") && nt && ot) {
        uint64_t d = (uint64_t)((double)nt->mediaDur * cx.movieTimescale / ot->timescale + 0.5);
        setDurV01(pay, 20, 28, d);
    } else if (type == fourcc("mvhd")) {
        setDurV01(pay, 16, 24, cx.movieDur);
    }
    wr32(out, (uint32_t)(8 + pay.size()));
    wr32(out, type);
    out.insert(out.end(), pay.begin(), pay.end());
}

struct Layout {
    int64_t ftypOff = -1, ftypSize = 0;
    int64_t mdatOff = -1, mdatHdr = 8, mdatSize = 0;
    int64_t moovOff = -1, moovSize = 0;
};

static bool fread_at(FILE *f, int64_t off, void *buf, size_t n) {
    if (fseeko(f, (off_t)off, SEEK_SET) != 0) return false;
    return fread(buf, 1, n, f) == n;
}

static bool scanTop(FILE *f, int64_t fileSize, Layout &L) {
    int64_t pos = 0;
    uint8_t h[16];
    while (pos + 8 <= fileSize) {
        if (!fread_at(f, pos, h, 16 <= fileSize - pos ? 16 : 8)) return false;
        uint64_t size = rd32(h);
        uint32_t type = rd32(h + 4);
        int64_t hdr = 8;
        if (size == 1) {
            if (pos + 16 > fileSize) return false;
            size = rd64(h + 8);
            hdr = 16;
        } else if (size == 0) {
            size = (uint64_t)(fileSize - pos);
        }
        if (size < (uint64_t)hdr) {
            // 末尾のゼロ埋めなど。ここで走査をやめる
            break;
        }
        if (pos + (int64_t)size > fileSize) {
            // 末尾が途切れている。mdatだけは許す(moovが先にある場合)
            if (type != fourcc("mdat")) break;
            size = (uint64_t)(fileSize - pos);
        }
        if (type == fourcc("ftyp") && L.ftypOff < 0) { L.ftypOff = pos; L.ftypSize = (int64_t)size; }
        else if (type == fourcc("mdat") && L.mdatOff < 0) { L.mdatOff = pos; L.mdatHdr = hdr; L.mdatSize = (int64_t)size; }
        else if (type == fourcc("moov") && L.moovOff < 0) { L.moovOff = pos; L.moovSize = (int64_t)size; }
        pos += (int64_t)size;
    }
    return L.ftypOff >= 0 && L.mdatOff >= 0 && L.moovOff >= 0;
}

}  // namespace detail

// in を読み、out(新しいファイル)へ「最初を削って maxBytes 以内にした動画」を書く。
// 戻り値が OK のときだけ out ができている。NOT_NEEDED のときは out を作らない。
static inline Result trim(const char *inPath, const char *outPath, int64_t maxBytes, Info *info = nullptr) {
    using namespace detail;
    Info local;
    Info &I = info ? *info : local;
    FILE *f = fopen(inPath, "rb");
    if (!f) return ERR_OPEN;
    struct Closer { FILE *f; ~Closer() { if (f) fclose(f); } } closer{f};
    if (fseeko(f, 0, SEEK_END) != 0) return ERR_OPEN;
    int64_t fileSize = (int64_t)ftello(f);
    I.origSize = fileSize;
    if (fileSize <= maxBytes) { I.newSize = fileSize; return NOT_NEEDED; }

    Layout L;
    if (!scanTop(f, fileSize, L)) return ERR_FORMAT;
    if (L.moovSize > 4 * 1024 * 1024) return ERR_FORMAT;
    Bytes moov((size_t)L.moovSize);
    if (!fread_at(f, L.moovOff, moov.data(), moov.size())) return ERR_OPEN;

    std::vector<Box> top;
    if (!parseBoxes(moov.data(), moov.size(), top, 0) || top.size() != 1 || top[0].type != fourcc("moov")) return ERR_FORMAT;
    const Box &moovBox = top[0];
    const Box *mvhd = findKid(moovBox, "mvhd");
    if (!mvhd || mvhd->payload.size() < 20) return ERR_FORMAT;
    uint32_t movieTimescale = (mvhd->payload[0] == 1) ? rd32(&mvhd->payload[20]) : rd32(&mvhd->payload[12]);
    if (movieTimescale == 0) return ERR_FORMAT;

    std::vector<Track> tracks;
    for (auto &k : moovBox.kids) {
        if (k.type != fourcc("trak")) continue;
        Track t;
        if (!loadTrack(k, t)) return ERR_FORMAT;
        tracks.push_back(std::move(t));
    }
    int vtrack = -1;
    for (size_t i = 0; i < tracks.size(); i++) if (tracks[i].handler == fourcc("vide")) { vtrack = (int)i; break; }
    if (vtrack < 0 || tracks[vtrack].size.empty()) return ERR_FORMAT;
    for (auto &t : tracks) {  // サンプルは mdat の中にあること
        for (size_t i = 0; i < t.off.size(); i++)
            if ((int64_t)t.off[i] < L.mdatOff + L.mdatHdr || (int64_t)(t.off[i] + t.size[i]) > L.mdatOff + L.mdatSize) return ERR_FORMAT;
    }

    const Track &vt = tracks[vtrack];
    std::vector<size_t> cands;  // 切り始めの候補(動画のキーフレーム番号)
    for (size_t i = 0; i < vt.sync.size(); i++) if (vt.sync[i]) cands.push_back(i);
    if (cands.empty() || cands[0] != 0) return ERR_FORMAT;

    int64_t mdatEnd = L.mdatOff + L.mdatSize;
    const int64_t newDataStart = L.ftypSize + 8;

    // 候補ごとに、書き出すファイルの大きさと moov を作る
    struct Built { Plan plan; Bytes moov; int64_t total = 0; };
    auto build = [&](size_t candIdx, Built &B) {
        B.plan.first.assign(tracks.size(), 0);
        size_t vf = cands[candIdx];
        double T = vt.startSec[vf];
        B.plan.T = T;
        int64_t cut = INT64_MAX;
        for (size_t k = 0; k < tracks.size(); k++) {
            size_t fi = ((int)k == vtrack) ? vf : firstAtOrAfter(tracks[k], T);
            B.plan.first[k] = fi;
            if (fi < tracks[k].off.size()) cut = std::min<int64_t>(cut, (int64_t)tracks[k].off[fi]);
        }
        if (cut == INT64_MAX) cut = mdatEnd;
        B.plan.cutOffset = cut;
        std::vector<NewTrack> nts(tracks.size());
        uint64_t movieDur = 0;
        for (size_t k = 0; k < tracks.size(); k++) {
            buildNewTrack(tracks[k], B.plan.first[k], tracks[k].size.size(), cut, newDataStart, nts[k]);
            uint64_t d = (uint64_t)((double)nts[k].mediaDur * movieTimescale / tracks[k].timescale + 0.5);
            movieDur = std::max(movieDur, d);
        }
        Ctx cx{&tracks, &nts, movieTimescale, movieDur, 0};
        B.moov.clear();
        serialize(moovBox, B.moov, cx, -1, 0);
        B.total = newDataStart + (mdatEnd - cut) + (int64_t)B.moov.size();
    };

    // 上限に収まる一番古い候補を二分探索で探す(大きさは候補が進むほど小さくなる)
    size_t lo = 0, hi = cands.size();  // [lo, hi)
    Built best;
    bool found = false;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        Built b;
        build(mid, b);
        if (b.total <= maxBytes) { best = std::move(b); found = true; hi = mid; }
        else lo = mid + 1;
    }
    if (!found) return ERR_TOO_BIG;
    {
        // 残りが短すぎる(2秒未満)なら、送る意味が薄いので失敗にする
        double remain = vt.totalSec - best.plan.T;
        if (remain < 2.0) return ERR_TOO_BIG;
        I.cutSeconds = best.plan.T;
        I.newSeconds = remain;
    }

    // 書き出し
    FILE *o = fopen(outPath, "wb");
    if (!o) return ERR_WRITE;
    struct OCloser { FILE *f; ~OCloser() { if (f) fclose(f); } } ocl{o};
    Bytes ftyp((size_t)L.ftypSize);
    if (!fread_at(f, L.ftypOff, ftyp.data(), ftyp.size())) return ERR_OPEN;
    if (fwrite(ftyp.data(), 1, ftyp.size(), o) != ftyp.size()) return ERR_WRITE;
    int64_t dataLen = mdatEnd - best.plan.cutOffset;
    if (dataLen + 8 > 0xFFFFFFFFLL) return ERR_TOO_BIG;
    uint8_t mh[8];
    put32(mh, (uint32_t)(dataLen + 8));
    put32(mh + 4, fourcc("mdat"));
    if (fwrite(mh, 1, 8, o) != 8) return ERR_WRITE;
    if (fseeko(f, (off_t)best.plan.cutOffset, SEEK_SET) != 0) return ERR_OPEN;
    static uint8_t buf[64 * 1024];
    int64_t left = dataLen;
    while (left > 0) {
        size_t n = (size_t)std::min<int64_t>(left, (int64_t)sizeof(buf));
        if (fread(buf, 1, n, f) != n) return ERR_OPEN;
        if (fwrite(buf, 1, n, o) != n) return ERR_WRITE;
        left -= (int64_t)n;
    }
    if (fwrite(best.moov.data(), 1, best.moov.size(), o) != best.moov.size()) return ERR_WRITE;
    if (fflush(o) != 0) return ERR_WRITE;
    I.newSize = best.total;
    return OK;
}

// ---- 以下: 範囲を指定して切り出す ----
namespace detail {

struct Loaded {
    Layout L;
    Bytes moov;
    std::vector<Box> top;       // top[0] が moov
    uint32_t movieTimescale = 0;
    std::vector<Track> tracks;
    int vtrack = -1;
    int64_t fileSize = 0;
    const Box &moovBox() const { return top[0]; }
};

static Result loadAll(FILE *f, Loaded &D) {
    if (fseeko(f, 0, SEEK_END) != 0) return ERR_OPEN;
    D.fileSize = (int64_t)ftello(f);
    if (!scanTop(f, D.fileSize, D.L)) return ERR_FORMAT;
    if (D.L.moovSize > 4 * 1024 * 1024) return ERR_FORMAT;
    D.moov.resize((size_t)D.L.moovSize);
    if (!fread_at(f, D.L.moovOff, D.moov.data(), D.moov.size())) return ERR_OPEN;
    if (!parseBoxes(D.moov.data(), D.moov.size(), D.top, 0) || D.top.size() != 1 || D.top[0].type != fourcc("moov")) return ERR_FORMAT;
    const Box *mvhd = findKid(D.top[0], "mvhd");
    if (!mvhd || mvhd->payload.size() < 20) return ERR_FORMAT;
    D.movieTimescale = (mvhd->payload[0] == 1) ? rd32(&mvhd->payload[20]) : rd32(&mvhd->payload[12]);
    if (D.movieTimescale == 0) return ERR_FORMAT;
    for (auto &k : D.top[0].kids) {
        if (k.type != fourcc("trak")) continue;
        Track t;
        if (!loadTrack(k, t)) return ERR_FORMAT;
        D.tracks.push_back(std::move(t));
    }
    for (size_t i = 0; i < D.tracks.size(); i++) if (D.tracks[i].handler == fourcc("vide")) { D.vtrack = (int)i; break; }
    if (D.vtrack < 0 || D.tracks[D.vtrack].size.empty()) return ERR_FORMAT;
    for (auto &t : D.tracks) {
        for (size_t i = 0; i < t.off.size(); i++)
            if ((int64_t)t.off[i] < D.L.mdatOff + D.L.mdatHdr || (int64_t)(t.off[i] + t.size[i]) > D.L.mdatOff + D.L.mdatSize) return ERR_FORMAT;
    }
    const Track &vt = D.tracks[D.vtrack];
    bool anySync = false;
    for (size_t i = 0; i < vt.sync.size(); i++) if (vt.sync[i]) { anySync = true; break; }
    if (!anySync || !vt.sync[0]) return ERR_FORMAT;
    return OK;
}

}  // namespace detail

struct Probe {
    double duration = 0;              // 映像の長さ(秒)
    std::vector<double> keyframes;    // キーフレームの時刻(秒)。先頭は0
    int64_t size = 0;
};

static inline Result probe(const char *inPath, Probe &P) {
    using namespace detail;
    FILE *f = fopen(inPath, "rb");
    if (!f) return ERR_OPEN;
    struct Closer { FILE *f; ~Closer() { if (f) fclose(f); } } closer{f};
    Loaded D;
    Result r = loadAll(f, D);
    if (r != OK) return r;
    const Track &vt = D.tracks[D.vtrack];
    P.size = D.fileSize;
    P.duration = vt.totalSec;
    P.keyframes.clear();
    for (size_t i = 0; i < vt.sync.size(); i++) if (vt.sync[i]) P.keyframes.push_back(vt.startSec[i]);
    return OK;
}

struct RangeInfo {
    int64_t origSize = 0, newSize = 0;
    double start = 0, end = 0;   // 実際に切り出す範囲(開始はキーフレームに切り下げ済み)
};

// startSec〜endSec を切り出して outPath に書く。dryRun のときは書かず、大きさと範囲だけ返す。
static inline Result cutRange(const char *inPath, const char *outPath, double startSec, double endSec, RangeInfo *info = nullptr,
                              bool dryRun = false) {
    using namespace detail;
    RangeInfo local;
    RangeInfo &I = info ? *info : local;
    FILE *f = fopen(inPath, "rb");
    if (!f) return ERR_OPEN;
    struct Closer { FILE *f; ~Closer() { if (f) fclose(f); } } closer{f};
    Loaded D;
    Result lr = loadAll(f, D);
    if (lr != OK) return lr;
    I.origSize = D.fileSize;
    const std::vector<Track> &tracks = D.tracks;
    const Track &vt = tracks[D.vtrack];

    if (startSec < 0) startSec = 0;
    if (endSec > vt.totalSec) endSec = vt.totalSec;
    // 開始: 指定より前(同じ)で一番近いキーフレーム
    size_t vf = 0;
    for (size_t i = 0; i < vt.sync.size(); i++) {
        if (!vt.sync[i]) continue;
        if (vt.startSec[i] <= startSec + 1e-6) vf = i; else break;
    }
    double T = vt.startSec[vf];
    // 終了: 時刻 endSec 以降で最初のコマの手前まで
    size_t vlast = firstAtOrAfter(vt, endSec);
    if (vlast <= vf + 1) return ERR_RANGE;
    double E = vlast >= vt.startSec.size() ? vt.totalSec : vt.startSec[vlast];
    if (E - T < 0.2) return ERR_RANGE;

    size_t nT = tracks.size();
    std::vector<size_t> first(nT), last(nT);
    int64_t cut = INT64_MAX, endOff = 0;
    for (size_t k = 0; k < nT; k++) {
        if ((int)k == D.vtrack) { first[k] = vf; last[k] = vlast; }
        else {
            first[k] = firstAtOrAfter(tracks[k], T);
            last[k] = firstAtOrAfter(tracks[k], E);
            if (last[k] < first[k]) last[k] = first[k];
        }
        if (last[k] > first[k]) {
            cut = std::min<int64_t>(cut, (int64_t)tracks[k].off[first[k]]);
            endOff = std::max<int64_t>(endOff, (int64_t)(tracks[k].off[last[k] - 1] + tracks[k].size[last[k] - 1]));
        }
    }
    if (cut == INT64_MAX || endOff <= cut) return ERR_RANGE;

    const int64_t newDataStart = D.L.ftypSize + 8;
    std::vector<NewTrack> nts(nT);
    uint64_t movieDur = 0;
    for (size_t k = 0; k < nT; k++) {
        buildNewTrack(tracks[k], first[k], last[k], cut, newDataStart, nts[k]);
        uint64_t d = (uint64_t)((double)nts[k].mediaDur * D.movieTimescale / tracks[k].timescale + 0.5);
        movieDur = std::max(movieDur, d);
    }
    Ctx cx{&tracks, &nts, D.movieTimescale, movieDur, 0};
    Bytes moovOut;
    serialize(D.moovBox(), moovOut, cx, -1, 0);
    int64_t dataLen = endOff - cut;
    if (dataLen + 8 > 0xFFFFFFFFLL) return ERR_TOO_BIG;
    I.newSize = newDataStart + dataLen + (int64_t)moovOut.size();
    I.start = T;
    I.end = E;
    if (dryRun) return OK;

    FILE *o = fopen(outPath, "wb");
    if (!o) return ERR_WRITE;
    struct OCloser { FILE *f; ~OCloser() { if (f) fclose(f); } } ocl{o};
    Bytes ftyp((size_t)D.L.ftypSize);
    if (!fread_at(f, D.L.ftypOff, ftyp.data(), ftyp.size())) return ERR_OPEN;
    if (fwrite(ftyp.data(), 1, ftyp.size(), o) != ftyp.size()) return ERR_WRITE;
    uint8_t mh[8];
    put32(mh, (uint32_t)(dataLen + 8));
    put32(mh + 4, fourcc("mdat"));
    if (fwrite(mh, 1, 8, o) != 8) return ERR_WRITE;
    if (fseeko(f, (off_t)cut, SEEK_SET) != 0) return ERR_OPEN;
    static uint8_t buf[64 * 1024];
    int64_t left = dataLen;
    while (left > 0) {
        size_t n = (size_t)std::min<int64_t>(left, (int64_t)sizeof(buf));
        if (fread(buf, 1, n, f) != n) return ERR_OPEN;
        if (fwrite(buf, 1, n, o) != n) return ERR_WRITE;
        left -= (int64_t)n;
    }
    if (fwrite(moovOut.data(), 1, moovOut.size(), o) != moovOut.size()) return ERR_WRITE;
    if (fflush(o) != 0) return ERR_WRITE;
    return OK;
}

}  // namespace mp4trim
