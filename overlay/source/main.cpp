// NXShot オーバーレイ (Tesla .ovl)
//
// Tesla メニューから開いて、ゲームを止めずに NXShot の設定を変える。
// 常駐版(sysmodule)は config.txt を5秒ごとに読み直すので、ここで書き換えた内容は、
// 数秒で反映される(常駐版の変更は不要)。
//
//   ・自動送信(auto=) / ゲームの通知(notify=) / フッター(footer=) / 動画を削る(trim=)
//   ・ゲームのアイコン位置(icon=) / 送信先(use=)
//   ・送る上限は 20MB に固定(開いたときに limit=20 にそろえる。画面では変えられない)
//   ・直近の写真・動画(最大10件)の一覧(サムネ付き。Aで開いて、大きく見る・送る)
//     送るときは ovl_send.txt に依頼を書き、常駐版(1.3.3以降)が送る。結果は ovl_result.txt に返る
//   ・常駐版が動いているかの表示
//
// config.txt は、書き換えたい行だけを直す(1行目のWebhook、hook=、album=、コメントなどは、そのまま残す)。
// Webhook URL は、画面にも、メモリにも、取り込まない(送信先は名前だけ読む)。
#define TESLA_INIT_IMPL
#include <tesla.hpp>

#include <dirent.h>
#include <sys/stat.h>

#include <jpeglib.h>

#include <algorithm>
#include <csetjmp>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

#define OVL_VERSION "1.3.3"

#define CONFIG_DIR  "sdmc:/config/nxdiscord"
#define CONFIG_PATH CONFIG_DIR "/config.txt"
#define CONFIG_TMP  CONFIG_DIR "/config.txt.ovltmp"
#define CONFIG_BAK  CONFIG_DIR "/config.txt.ovlbak"

#define SYSMODULE_TID 0x4200000000000A7EULL
#define MAX_CFG_LINES 58  // 常駐版は60行目までしか読まない。それを超えて足さない
#define FIXED_LIMIT_MB 20  // オーバーレイでは、送る上限を20MBに固定する
#define DEFAULT_ALBUM "sdmc:/Nintendo/Album"
#define RECENT_COUNT 10
#define OVL_REQ_PATH CONFIG_DIR "/ovl_send.txt"
#define OVL_REQ_TMP  CONFIG_DIR "/ovl_send.txt.tmp"
#define OVL_RES_PATH CONFIG_DIR "/ovl_result.txt"
#define OVL_READY_PATH CONFIG_DIR "/ovl_ready.txt"
#define VIEW_W 288  // 大きく見るときの絵の大きさ(16:9)
#define VIEW_H 162
#define THUMB_W 106  // サムネの大きさ(16:9)
#define THUMB_H 60

static bool g_pmOk = false;

// ---- 文字列 ----
static std::string trimLine(const char *line) {
    std::string s(line);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && s[i] == ' ') i++;
    return s.substr(i);
}

// 1行目や hook= の「名前|URL」から、名前だけを取り出す(URLは持たない)
static std::string hookNameOf(const std::string &s, int index) {
    size_t p = s.find('|');
    std::string nm;
    if (s.compare(0, 8, "https://") != 0 && p != std::string::npos) nm = trimLine(s.substr(0, p).c_str());
    if (nm.empty()) nm = "送信先" + std::to_string(index);
    return nm;
}

// ---- config.txt の読み込み(表示用) ----
struct Cfg {
    bool ok = false;  // config.txt が読めた
    long limitMB = 10;
    bool trim = true, autoSend = true, notify = true, footer = true;
    int icon = 1;
    int use = 1;  // 1始まり
    std::string album = DEFAULT_ALBUM;
    std::vector<std::string> hookNames;
};

static bool loadCfg(Cfg &c) {
    c = Cfg();
    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) return false;
    char line[512];
    int n = 0;
    while (n < 60 && fgets(line, sizeof(line), f)) {
        n++;
        std::string t = trimLine(line);
        if (n == 1) {
            c.hookNames.push_back(hookNameOf(t, 1));
            c.ok = true;
            continue;
        }
        if (t.empty()) continue;
        if (t.compare(0, 6, "limit=") == 0) {
            long v = atol(trimLine(t.c_str() + 6).c_str());
            if (v >= 1 && v <= 500) c.limitMB = v;
        } else if (t.compare(0, 5, "trim=") == 0) {
            c.trim = atol(trimLine(t.c_str() + 5).c_str()) != 0;
        } else if (t.compare(0, 5, "auto=") == 0) {
            c.autoSend = atol(trimLine(t.c_str() + 5).c_str()) != 0;
        } else if (t.compare(0, 7, "notify=") == 0) {
            c.notify = atol(trimLine(t.c_str() + 7).c_str()) != 0;
        } else if (t.compare(0, 7, "footer=") == 0) {
            c.footer = atol(trimLine(t.c_str() + 7).c_str()) != 0;
        } else if (t.compare(0, 5, "icon=") == 0) {
            long v = atol(trimLine(t.c_str() + 5).c_str());
            c.icon = (v >= 0 && v <= 3) ? (int)v : 1;
        } else if (t.compare(0, 5, "hook=") == 0) {
            std::string e = trimLine(t.c_str() + 5);
            std::string url = e;
            size_t p = e.find('|');
            if (e.compare(0, 8, "https://") != 0 && p != std::string::npos) url = trimLine(e.substr(p + 1).c_str());
            if (url.compare(0, 8, "https://") == 0) c.hookNames.push_back(hookNameOf(e, (int)c.hookNames.size() + 1));
        } else if (t.compare(0, 4, "use=") == 0) {
            c.use = (int)atol(trimLine(t.c_str() + 4).c_str());
        } else if (t.compare(0, 6, "album=") == 0) {
            std::string a = trimLine(t.c_str() + 6);
            if (!a.empty()) c.album = a;
        }
    }
    fclose(f);
    while (c.album.size() > 1 && c.album.back() == '/') c.album.pop_back();
    if (c.use < 1 || c.use > (int)c.hookNames.size()) c.use = 1;
    return c.ok;
}

// ---- config.txt の書き換え(1行ずつ。変えない行は、そのまま残す) ----
static bool readLines(std::vector<std::string> &out) {
    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) return false;
    char buf[512];
    std::string cur;
    while (fgets(buf, sizeof(buf), f)) {
        cur += buf;
        if (!cur.empty() && cur.back() == '\n') {  // 長い行は、改行までつなげる
            while (!cur.empty() && (cur.back() == '\n' || cur.back() == '\r')) cur.pop_back();
            out.push_back(cur);
            cur.clear();
        }
    }
    if (!cur.empty()) {
        while (!cur.empty() && (cur.back() == '\n' || cur.back() == '\r')) cur.pop_back();
        out.push_back(cur);
    }
    fclose(f);
    return true;
}

static bool writeLines(const std::vector<std::string> &L) {
    FILE *f = fopen(CONFIG_TMP, "w");
    if (!f) return false;
    bool ok = true;
    for (auto &s : L) {
        if (fprintf(f, "%s\n", s.c_str()) < 0) { ok = false; break; }
    }
    if (fclose(f) != 0) ok = false;
    if (!ok) { remove(CONFIG_TMP); return false; }
    // 途中で止まっても config.txt を失わないよう、いったん退避してから入れ替える
    remove(CONFIG_BAK);
    if (rename(CONFIG_PATH, CONFIG_BAK) != 0) { remove(CONFIG_TMP); return false; }
    if (rename(CONFIG_TMP, CONFIG_PATH) != 0) {
        rename(CONFIG_BAK, CONFIG_PATH);
        remove(CONFIG_TMP);
        return false;
    }
    remove(CONFIG_BAK);
    return true;
}

// key=val の行を書き換える(同じキーが何行かあれば、全部。無ければ末尾に足す。1行目は触らない)
static bool setKey(const char *key, const std::string &val) {
    std::vector<std::string> L;
    if (!readLines(L) || L.empty()) return false;
    std::string prefix = std::string(key) + "=";
    bool found = false;
    for (size_t i = 1; i < L.size(); i++) {
        std::string t = trimLine(L[i].c_str());
        if (t.compare(0, prefix.size(), prefix) == 0) {
            L[i] = prefix + val;
            found = true;
        }
    }
    if (!found) {
        if ((int)L.size() >= MAX_CFG_LINES) return false;
        L.push_back(prefix + val);
    }
    return writeLines(L);
}

// ---- 常駐版が動いているか ----
// 1: 動作中 / 0: 動いていない / -1: 調べられない
static int sysmoduleState() {
    if (!g_pmOk) return -1;
    u64 pid = 0;
    return R_SUCCEEDED(pmdmntGetProcessId(&pid, SYSMODULE_TID)) ? 1 : 0;
}

static const char *stateText(int s) {
    return s == 1 ? "動作中" : (s == 0 ? "停止中" : "不明");
}


// ---- アルバム(直近の写真・動画) ----
struct MediaInfo {
    std::string name;
    std::string path;
    bool video = false;
    long long size = 0;
};

static int mediaKindOf(const std::string &n) {  // 1: 写真 / 2: 動画 / 0: ちがう
    size_t dot = n.rfind('.');
    if (dot == std::string::npos) return 0;
    std::string e = n.substr(dot + 1);
    for (auto &c : e) c = (char)tolower((unsigned char)c);
    if (e == "jpg" || e == "jpeg") return 1;
    if (e == "mp4") return 2;
    return 0;
}

// 数字だけの名前のフォルダ(年・月・日)を、新しい順に
static std::vector<std::string> numDirsDesc(const std::string &path) {
    std::vector<std::string> out;
    DIR *d = opendir(path.c_str());
    if (!d) return out;
    while (dirent *e = readdir(d)) {
        if (e->d_type != DT_DIR) continue;
        std::string n = e->d_name;
        bool digits = !n.empty();
        for (char c : n) {
            if (c < '0' || c > '9') { digits = false; break; }
        }
        if (digits) out.push_back(n);
    }
    closedir(d);
    std::sort(out.begin(), out.end(), std::greater<std::string>());
    return out;
}

// 写真・動画のファイル名を、新しい順に(名前が撮った時刻の順になっている)
static std::vector<std::string> mediaFilesDesc(const std::string &path) {
    std::vector<std::string> out;
    DIR *d = opendir(path.c_str());
    if (!d) return out;
    while (dirent *e = readdir(d)) {
        if (e->d_type == DT_DIR) continue;
        std::string n = e->d_name;
        if (mediaKindOf(n) != 0) out.push_back(n);
    }
    closedir(d);
    std::sort(out.begin(), out.end(), std::greater<std::string>());
    return out;
}

static std::vector<MediaInfo> recentMedia(const std::string &album, size_t maxCount) {
    std::vector<MediaInfo> out;
    for (auto &y : numDirsDesc(album)) {
        for (auto &m : numDirsDesc(album + "/" + y)) {
            for (auto &dd : numDirsDesc(album + "/" + y + "/" + m)) {
                std::string dir = album + "/" + y + "/" + m + "/" + dd;
                for (auto &f : mediaFilesDesc(dir)) {
                    MediaInfo mi;
                    mi.name = f;
                    mi.path = dir + "/" + f;
                    mi.video = mediaKindOf(f) == 2;
                    struct stat st;
                    if (stat(mi.path.c_str(), &st) == 0) mi.size = (long long)st.st_size;
                    out.push_back(mi);
                    if (out.size() >= maxCount) return out;
                }
            }
        }
    }
    return out;
}

// 「2026111412345600-....jpg」→「11/14 12:34:56」
static std::string timeLabel(const std::string &name) {
    if (name.size() < 14) return name;
    for (int i = 0; i < 14; i++) {
        if (name[(size_t)i] < '0' || name[(size_t)i] > '9') return name;
    }
    return name.substr(4, 2) + "/" + name.substr(6, 2) + " " + name.substr(8, 2) + ":" + name.substr(10, 2) + ":" + name.substr(12, 2);
}

static std::string sizeLabel(const MediaInfo &m) {
    char b[48];
    snprintf(b, sizeof(b), "%s %.1fMB", m.video ? "動画" : "写真", (double)m.size / 1048576.0);
    std::string s = b;
    if (m.size > (long long)FIXED_LIMIT_MB * 1048576LL) s += "(" + std::to_string(FIXED_LIMIT_MB) + "MB超)";
    return s;
}

// ---- サムネ ----
// 写真(JPEG)を、THUMB_W x THUMB_H の小さな絵(RGBA 各8bit)にする。
// libjpeg の縮小読み込み(1/8 など)を使うので、速く、メモリも少なくて済む。
struct JErr {
    jpeg_error_mgr pub;
    jmp_buf jb;
};

static void jerrExit(j_common_ptr c) {
    longjmp(((JErr *)c->err)->jb, 1);
}

static void fillRGBA(std::vector<u8> &out, int w, int h, u8 r, u8 g, u8 b) {
    out.assign((size_t)w * h * 4, 255);
    for (size_t i = 0; i < (size_t)w * h; i++) {
        out[i * 4 + 0] = r;
        out[i * 4 + 1] = g;
        out[i * 4 + 2] = b;
    }
}

static void fillThumb(std::vector<u8> &out, u8 r, u8 g, u8 b) {
    fillRGBA(out, THUMB_W, THUMB_H, r, g, b);
}

// 動画の印(暗い地に、再生マーク)。動画の中に絵が見つからなかったときの代わり
static void videoMark(std::vector<u8> &out, int w, int h) {
    fillRGBA(out, w, h, 28, 28, 42);
    const int tri = h * 27 / 100;  // 三角の高さの半分
    const int cx = w / 2 - tri / 3, cy = h / 2;
    for (int x = cx - tri * 2 / 3; x <= cx + tri * 4 / 3; x++) {
        int half = (cx + tri * 4 / 3 - x) * tri / (tri * 2);
        for (int y = cy - half; y <= cy + half; y++) {
            if (x < 0 || x >= w || y < 0 || y >= h) continue;
            size_t k = ((size_t)y * w + (size_t)x) * 4;
            out[k + 0] = 235;
            out[k + 1] = 235;
            out[k + 2] = 245;
        }
    }
}

// JPEG を tw x th の絵(RGBA 各8bit)にする。読み込み元は、ファイルでもメモリでもよい。
// libjpeg の縮小読み込み(1/8 など)を使うので、速く、メモリも少なくて済む。
typedef void (*JSrcFn)(jpeg_decompress_struct *, void *);

struct MemBuf {
    const u8 *p;
    size_t n;
};

static void srcFile(jpeg_decompress_struct *ci, void *arg) { jpeg_stdio_src(ci, (FILE *)arg); }
static void srcMem(jpeg_decompress_struct *ci, void *arg) {
    MemBuf *m = (MemBuf *)arg;
    jpeg_mem_src(ci, m->p, (unsigned long)m->n);
}

static bool jpegDecode(JSrcFn setSrc, void *arg, int tw, int th, int minW, std::vector<u8> &out) {
    jpeg_decompress_struct ci;
    JErr je;
    u8 *volatile row = nullptr;  // longjmp をまたぐので、C流に確保する
    ci.err = jpeg_std_error(&je.pub);
    je.pub.error_exit = jerrExit;
    if (setjmp(je.jb)) {
        jpeg_destroy_decompress(&ci);
        if (row) free(row);
        return false;
    }
    jpeg_create_decompress(&ci);
    setSrc(&ci, arg);
    jpeg_read_header(&ci, TRUE);
    if ((int)ci.image_width < minW) {  // 小さすぎる絵は、動画の中のたまたまの並びとみなして捨てる
        jpeg_destroy_decompress(&ci);
        return false;
    }
    // 出す大きさより小さくならない範囲で、いちばん小さく読む(1/1, 1/2, 1/4, 1/8)
    int d = 8;
    while (d > 1 && (ci.image_width / (unsigned)d < (unsigned)tw || ci.image_height / (unsigned)d < (unsigned)th)) d /= 2;
    ci.scale_num = 1;
    ci.scale_denom = (unsigned)d;
    ci.out_color_space = JCS_RGB;
    jpeg_start_decompress(&ci);
    const int w = (int)ci.output_width, h = (int)ci.output_height;
    if (ci.output_components != 3 || w < 1 || h < 1) {
        jpeg_destroy_decompress(&ci);
        return false;
    }
    row = (u8 *)malloc((size_t)w * 3);
    if (!row) {
        jpeg_destroy_decompress(&ci);
        return false;
    }
    fillRGBA(out, tw, th, 0, 0, 0);
    int sy = 0;
    for (int dy = 0; dy < th; dy++) {
        int want = dy * h / th;
        while (sy <= want) {
            JSAMPROW rp = row;
            jpeg_read_scanlines(&ci, &rp, 1);
            sy++;
        }
        for (int dx = 0; dx < tw; dx++) {
            int sx = dx * w / tw;
            size_t k = ((size_t)dy * tw + (size_t)dx) * 4;
            out[k + 0] = row[sx * 3 + 0];
            out[k + 1] = row[sx * 3 + 1];
            out[k + 2] = row[sx * 3 + 2];
        }
    }
    jpeg_abort_decompress(&ci);
    jpeg_destroy_decompress(&ci);
    free(row);
    return true;
}

// 写真(JPEG)を、tw x th の絵にする
static bool jpegPhoto(const std::string &path, int tw, int th, std::vector<u8> &out) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;
    bool ok = jpegDecode(srcFile, f, tw, th, 0, out);
    fclose(f);
    return ok;
}

// 動画(mp4)の中に付いている絵(JPEG)を探して、絵にする。
// オーバーレイには動画をひらく部品が入っていない(メモリが足りない)ので、
// ファイルのおしりのほうにある JPEG を探す。見つからなければ false(呼ぶほうが、再生マークで代わりにする)
static bool videoEmbeddedThumb(const std::string &path, int tw, int th, std::vector<u8> &out) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return false; }
    long sz = ftell(f);
    const long TAIL = 131072;
    long n = sz < TAIL ? sz : TAIL;
    if (n < 1024 || fseek(f, sz - n, SEEK_SET) != 0) { fclose(f); return false; }
    std::vector<u8> buf((size_t)n);
    size_t got = fread(buf.data(), 1, (size_t)n, f);
    fclose(f);
    if (got < 1024) return false;
    int tries = 0;
    for (long i = (long)got - 4; i >= 0 && tries < 6; i--) {  // うしろから探す(いちばん後ろのものが、本物のことが多い)
        if (buf[(size_t)i] == 0xFF && buf[(size_t)i + 1] == 0xD8 && buf[(size_t)i + 2] == 0xFF) {
            tries++;
            MemBuf mb{buf.data() + i, got - (size_t)i};
            if (jpegDecode(srcMem, &mb, tw, th, 160, out)) return true;
        }
    }
    return false;
}

// 一覧用のサムネ(写真は縮小、動画は中の絵。どちらもだめなら、印)
static void makeThumb(const MediaInfo &m, std::vector<u8> &bmp) {
    if (m.video) {
        if (!videoEmbeddedThumb(m.path, THUMB_W, THUMB_H, bmp)) videoMark(bmp, THUMB_W, THUMB_H);
    } else if (!jpegPhoto(m.path, THUMB_W, THUMB_H, bmp)) {
        fillThumb(bmp, 40, 40, 40);  // 読めなかった写真は、灰色の四角
    }
}

// ============================================================
// 見た目(NEXUS風): 濃い紺の地、ピンク〜紫、虹色の線と枠
// ============================================================
// Tesla の標準の枠・行は使わず、地・題・行・選んだ行の枠・下の案内を、ここで自分で描く。
// 画面の大きさは Tesla のオーバーレイ画面(448 x 720)。
static const s32 FB_W = 448, FB_H = 720;

static tsl::Color C(u8 r, u8 g, u8 b, u8 a = 15) { return tsl::Color(r, g, b, a); }

static const tsl::Color COL_BG    = tsl::Color(1, 1, 2, 15);     // 地: ほぼ黒(ほんの少しだけ青)
static const tsl::Color COL_PANEL = tsl::Color(3, 3, 5, 15);     // カード・選んだ行の地
static const tsl::Color COL_TEXT  = tsl::Color(15, 15, 15, 15);  // ふつうの文字
static const tsl::Color COL_SUB   = tsl::Color(10, 10, 12, 15);  // 小さい文字(灰色)
static const tsl::Color COL_VALUE = tsl::Color(14, 11, 14, 15);  // 右側の値(うすいピンク)
static const tsl::Color COL_ACC   = tsl::Color(13, 5, 10, 15);   // 目印の色(NXShotのピンク)
static const tsl::Color COL_GOOD  = tsl::Color(7, 15, 9, 15);
static const tsl::Color COL_BAD   = tsl::Color(15, 7, 7, 15);
static const tsl::Color COL_WARN  = tsl::Color(15, 13, 4, 15);
static const tsl::Color COL_LINE  = tsl::Color(3, 3, 5, 15);     // 行の区切り

// 題の色: アイコンの「N」(ピンク→紫)から、右上の丸(青みのある紫)へ
static tsl::Color gradColor(int i, int n) {
    static const float S[3][3] = {{13.f, 5.f, 9.f}, {10.f, 6.f, 13.f}, {7.f, 7.f, 14.f}};
    float t = (n > 1) ? (float)i / (float)(n - 1) : 0.f;
    int a = t < 0.5f ? 0 : 1;
    float u = t < 0.5f ? t * 2.f : (t - 0.5f) * 2.f;
    u8 c[3];
    for (int k = 0; k < 3; k++) {
        float v = S[a][k] + (S[a + 1][k] - S[a][k]) * u + 0.5f;
        c[k] = (u8)(v < 0.f ? 0.f : (v > 15.f ? 15.f : v));
    }
    return tsl::Color(c[0], c[1], c[2], 15);
}

// ピンク→紫の横線(静止)
static void gradLine(tsl::gfx::Renderer *r, s32 x, s32 y, s32 w, s32 t) {
    const int N = 24;
    for (int i = 0; i < N; i++) {
        s32 x0 = x + w * i / N, x1 = x + w * (i + 1) / N;
        r->drawRect(x0, y, x1 - x0, t, gradColor(i, N));
    }
}

// 虹色の線(題の下)。色相から、少し白を混ぜた明るい色にして、8秒で1周ゆっくり流す
static float clamp01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }
static tsl::Color hueColor(float h) {
    h -= floorf(h);
    float r = clamp01(fabsf(h * 6.f - 3.f) - 1.f);
    float g = clamp01(2.f - fabsf(h * 6.f - 2.f));
    float b = clamp01(2.f - fabsf(h * 6.f - 4.f));
    r = 0.2f + 0.8f * r;
    g = 0.2f + 0.8f * g;
    b = 0.2f + 0.8f * b;
    return tsl::Color((u8)(r * 15.f + 0.5f), (u8)(g * 15.f + 0.5f), (u8)(b * 15.f + 0.5f), 15);
}

static void rainbowLine(tsl::gfx::Renderer *r, s32 x, s32 y, s32 w, s32 t) {
    u64 ms = armTicksToNs(armGetSystemTick()) / 1000000ULL;
    float phase = (float)(ms % 8000ULL) / 8000.f;
    for (s32 i = 0; i < w; i += 8) {
        s32 sw = (w - i < 8) ? (w - i) : 8;
        r->drawRect(x + i, y, sw, t, hueColor(phase + (float)i / (float)w));
    }
}

// 「NXShot」を、必ず1度に描く(1文字ずつだと文字の幅の取り方で重なることがあった)。
// 色の変化は、縦に細く切った帯ごとに、描く範囲を絞って(scissor)つける。
// ※ scissor は「入れたら、必ずその場で出す」(入れっぱなしにすると、あとの描画が切り取られる)
static void drawTitle(tsl::gfx::Renderer *r) {
    const char *TITLE = "NXShot";
    const s32 X0 = 20, Y = 50, SZ = 34;
    auto dim = r->drawString(TITLE, false, X0, Y, SZ, tsl::Color(0, 0, 0, 0));  // 透明で描いて、幅を測る
    s32 W = (s32)dim.first;
    if (W < 80 || W > 240) W = 130;
    const int BANDS = 12;
    for (int i = 0; i < BANDS; i++) {
        s32 x0 = X0 + W * i / BANDS;
        s32 x1 = (i == BANDS - 1) ? X0 + W + 40 : X0 + W * (i + 1) / BANDS;
        r->enableScissoring(x0, 8, x1 - x0, 62);
        r->drawString(TITLE, false, X0, Y, SZ, gradColor(i, BANDS));
        r->disableScissoring();
    }
}

// ---- 画面の枠(地・題・虹色の線・下の案内を自分で描く) ----
class NXFrame : public tsl::elm::OverlayFrame {
public:
    explicit NXFrame(const std::string &subtitle) : tsl::elm::OverlayFrame("", subtitle), m_sub(subtitle) {}

    // setContent の代わりに使う(中身の持ち主は、これまでどおり枠)
    void setNXContent(tsl::elm::Element *e) {
        m_nx = e;
        setContent(e);
    }

    void draw(tsl::gfx::Renderer *r) override {
        r->fillScreen(COL_BG);
        drawTitle(r);
        r->drawString(m_sub.c_str(), false, 21, 74, 16, COL_SUB);
        rainbowLine(r, 20, 88, FB_W - 40, 2);
        if (m_nx) m_nx->frame(r);
        // 下の案内(中身が下にはみ出しても隠れるよう、あとから描く)
        r->drawRect(0, FB_H - 74, FB_W, 74, COL_BG);
        r->drawRect(20, FB_H - 73, FB_W - 40, 1, C(5, 5, 7));
        r->drawString("\uE0E1  戻る", false, 30, 693, 22, COL_SUB);
        r->drawString("\uE0E0  OK", false, 200, 693, 22, COL_SUB);
    }

    // 中身を、題のすぐ下(虹色の線のあと)から始める
    void layout(u16 px, u16 py, u16 pw, u16 ph) {
        tsl::elm::OverlayFrame::layout(px, py, pw, ph);
        if (m_nx) {
            m_nx->setBoundaries(px + 25, py + 100, pw - 60, ph - 100 - 76);
            m_nx->invalidate();
        }
    }

private:
    std::string m_sub;
    tsl::elm::Element *m_nx = nullptr;
};

// ---- 行の土台: 選んでいる行は、薄い地を敷いて、左に太い目印を付ける ----
class NXItem : public tsl::elm::ListItem {
public:
    NXItem() : tsl::elm::ListItem("") {}

    // Tesla が、選んでいる行の「下地」と「枠」を描くときに呼ぶ(標準の見た目の代わり)
    void drawFocusBackground(tsl::gfx::Renderer *r) { r->drawRect(getX(), getY(), getWidth(), getHeight(), C(4, 4, 6, 15)); }
    void drawHighlight(tsl::gfx::Renderer *r) { r->drawRect(getX(), getY() + 4, 4, getHeight() - 8, COL_ACC); }
};

// 文字の幅(透明で描いて測る。測れなかったときは、文字数からの見積もり)
static s32 textWidth(tsl::gfx::Renderer *r, const std::string &s, s32 size) {
    if (s.empty()) return 0;
    auto d = r->drawString(s.c_str(), false, 0, 0, size, tsl::Color(0, 0, 0, 0));
    s32 w = (s32)d.first;
    if (w <= 0 || w > 600) w = (s32)s.size() * size / 2;
    return w;
}

// ---- 設定の1行: 左に名前、右に値(または、スイッチ) ----
class NXRow : public NXItem {
public:
    explicit NXRow(const std::string &label) : m_label(label) {}

    void setValue(const std::string &v) { m_value = v; }

    // スイッチの行にする(A で切り替わる。変わったら cb を呼ぶ)
    void setToggle(bool on, std::function<void(bool)> cb) {
        m_toggle = true;
        m_on = on;
        m_cb = std::move(cb);
        setClickListener([this](u64 keys) {
            if (!(keys & HidNpadButton_A)) return false;
            m_on = !m_on;
            if (m_cb) m_cb(m_on);
            return true;
        });
    }

    void draw(tsl::gfx::Renderer *r) override {
        const s32 x = getX(), y = getY(), w = getWidth(), h = getHeight();
        r->drawString(m_label.c_str(), false, x + 18, y + h * 62 / 100, 22, COL_TEXT);
        if (m_toggle) {
            const s32 tw = 50, th = 24, tx = x + w - 16 - tw, ty = y + (h - th) / 2;
            r->drawRect(tx, ty, tw, th, m_on ? COL_ACC : C(4, 4, 6));
            const s32 ks = th - 6;
            r->drawRect(m_on ? tx + tw - ks - 3 : tx + 3, ty + 3, ks, ks, C(15, 15, 15));
        } else if (!m_value.empty()) {
            s32 vw = textWidth(r, m_value, 20);
            r->drawString(m_value.c_str(), false, x + w - 16 - vw, y + h * 62 / 100, 20, COL_VALUE);
        }
        r->drawRect(x + 8, y + h - 1, w - 16, 1, COL_LINE);
    }

private:
    std::string m_label, m_value;
    bool m_toggle = false, m_on = false;
    std::function<void(bool)> m_cb;
};

// ---- 直近の写真・動画の1行(左に時刻と大きさ、右にサムネ) ----
class MediaItem : public NXItem {
public:
    MediaItem(const std::string &time, const std::string &sub, bool warn, std::vector<u8> bmp)
        : m_time(time), m_sub(sub), m_warn(warn), m_bmp(std::move(bmp)) {}

    void draw(tsl::gfx::Renderer *r) override {
        const s32 x = getX(), y = getY(), w = getWidth(), h = getHeight();
        r->drawString(m_time.c_str(), false, x + 16, y + h * 43 / 100, 22, COL_TEXT);
        r->drawString(m_sub.c_str(), false, x + 16, y + h * 78 / 100, 17, m_warn ? COL_WARN : COL_SUB);
        if (!m_bmp.empty()) r->drawBitmap(x + w - THUMB_W - 12, y + (h - THUMB_H) / 2, THUMB_W, THUMB_H, m_bmp.data());
        r->drawRect(x + 8, y + h - 1, w - 16, 1, COL_LINE);
    }

private:
    std::string m_time, m_sub;
    bool m_warn;
    std::vector<u8> m_bmp;
};

// ---- 小見出し(灰色の文字と、そのあとに続く細い線) ----
static tsl::elm::Element *makeHeader(const std::string &text) {
    return new tsl::elm::CustomDrawer([text](tsl::gfx::Renderer *r, s32 x, s32 y, s32 w, s32 h) {
        s32 tw = textWidth(r, text, 17);
        r->drawString(text.c_str(), false, x + 6, y + h - 12, 17, COL_SUB);
        r->drawRect(x + 6 + tw + 12, y + h - 18, w - tw - 24, 1, C(4, 4, 6));
    });
}
#define HEADER_H 46

// ---- 送る依頼(常駐版 1.3.3 以降が、ovl_send.txt を読んで送る) ----
static bool requestSend(const std::string &path) {
    remove(OVL_RES_PATH);  // 前の結果を消す
    FILE *f = fopen(OVL_REQ_TMP, "w");
    if (!f) return false;
    bool ok = fprintf(f, "%s\n", path.c_str()) > 0;
    if (fclose(f) != 0) ok = false;
    if (!ok) { remove(OVL_REQ_TMP); return false; }
    remove(OVL_REQ_PATH);
    if (rename(OVL_REQ_TMP, OVL_REQ_PATH) != 0) { remove(OVL_REQ_TMP); return false; }
    return true;
}

// 結果: 0 まだ / 1 送れた / 2 大きすぎ / 3 失敗 / 4 送れないファイル / 6 通信できない
static int readSendResult() {
    FILE *f = fopen(OVL_RES_PATH, "r");
    if (!f) return 0;
    char line[64] = {0};
    if (!fgets(line, sizeof(line), f)) line[0] = 0;
    fclose(f);
    std::string t = trimLine(line);
    if (t == "ok") return 1;
    if (t == "big") return 2;
    if (t == "fail") return 3;
    if (t == "bad") return 4;
    if (t == "net") return 6;
    return 0;
}

// ---- 1件を大きく見て、送る画面 ----
// 写真: 大きく表示。動画: 中の絵(あれば)を表示。動画の再生は、オーバーレイでは(メモリの都合で)できない。
// 「送る」は、押せる行(A で送る、B で戻る)。
class ViewGui : public tsl::Gui {
public:
    explicit ViewGui(MediaInfo m) : m_m(std::move(m)) {}

    tsl::elm::Element *createUI() override {
        auto *frame = new NXFrame(timeLabel(m_m.name));
        auto *list = new tsl::elm::List();
        if (m_m.video) {
            if (!videoEmbeddedThumb(m_m.path, VIEW_W, VIEW_H, m_bmp)) videoMark(m_bmp, VIEW_W, VIEW_H);
        } else if (!jpegPhoto(m_m.path, VIEW_W, VIEW_H, m_bmp)) {
            fillRGBA(m_bmp, VIEW_W, VIEW_H, 40, 40, 40);
        }
        auto *pane = new tsl::elm::CustomDrawer([this](tsl::gfx::Renderer *r, s32 x, s32 y, s32 w, s32 h) {
            (void)h;
            const s32 bx = x + (w - VIEW_W) / 2, by = y + 14;
            r->drawBitmap(bx, by, VIEW_W, VIEW_H, m_bmp.data());
            r->drawRect(bx - 2, by - 2, VIEW_W + 4, 2, COL_ACC);
            r->drawRect(bx - 2, by + VIEW_H, VIEW_W + 4, 2, COL_ACC);
            r->drawRect(bx - 2, by, 2, VIEW_H, COL_ACC);
            r->drawRect(bx + VIEW_W, by, 2, VIEW_H, COL_ACC);
            r->drawString(sizeLabel(m_m).c_str(), false, bx, by + VIEW_H + 34, 22, COL_TEXT);
            if (m_m.video) r->drawString("※動画の再生は、アプリ版でできます", false, bx, by + VIEW_H + 62, 17, COL_SUB);
        });
        list->addItem(pane, VIEW_H + 90);

        m_send = new NXRow("Discordへ送る");
        m_send->setValue("A");
        m_send->setClickListener([this](u64 keys) {
            if (!(keys & HidNpadButton_A)) return false;
            startSend();
            return true;
        });
        list->addItem(m_send);
        frame->setNXContent(list);
        return frame;
    }

    void update() override {
        if (!m_pending) return;
        int r = readSendResult();
        if (r == 1) say("送りました");
        else if (r == 2) say("大きすぎて送れません");
        else if (r == 3) say("送信に失敗しました");
        else if (r == 4) say("送れないファイルです");
        else if (r == 6) say("通信できません(Wi-Fi)");
        else {
            u64 el = armGetSystemTick() - m_since;
            struct stat st;
            bool stillThere = stat(OVL_REQ_PATH, &st) == 0;
            if (stillThere && el > armNsToTicks(30000000000ULL)) {
                // 30秒たっても、依頼のファイルを常駐版が受け取っていない。
                // 「対応しています」の印(ovl_ready.txt)が無ければ、古い常駐版のまま(入れ替え+再起動が要る)
                struct stat rs;
                if (stat(OVL_READY_PATH, &rs) != 0) say("常駐版が古い(入替+再起動)");
                else say("常駐版が反応しません");
                remove(OVL_REQ_PATH);
                r = 5;
            } else if (el > armNsToTicks(240000000000ULL)) {
                say("結果が分かりません");
                r = 5;
            }
        }
        if (r != 0) {
            m_pending = false;
            remove(OVL_RES_PATH);
        }
    }

private:
    MediaInfo m_m;
    std::vector<u8> m_bmp;
    NXRow *m_send = nullptr;
    bool m_pending = false;
    u64 m_since = 0;

    void say(const char *s) {
        if (m_send) m_send->setValue(s);
    }

    void startSend() {
        if (m_pending) return;  // 送っている最中は、もう一度は送らない
        if (sysmoduleState() == 0) {
            say("常駐版が止まっています");
            return;
        }
        if (requestSend(m_m.path)) {
            m_pending = true;
            m_since = armGetSystemTick();
            say("送っています…");
        } else {
            say("依頼を書けません");
        }
    }
};

// ---- 直近の写真・動画の画面 ----
class RecentGui : public tsl::Gui {
public:
    explicit RecentGui(std::string album) : m_album(std::move(album)) {}

    tsl::elm::Element *createUI() override {
        auto *frame = new NXFrame("直近の写真・動画");
        auto *list = new tsl::elm::List();
        list->addItem(makeHeader("新しい順(最大" + std::to_string(RECENT_COUNT) + "件)  Aで開く"), HEADER_H);
        std::vector<MediaInfo> v = recentMedia(m_album, RECENT_COUNT);
        if (v.empty()) {
            list->addItem(new tsl::elm::CustomDrawer([](tsl::gfx::Renderer *r, s32 x, s32 y, s32 w, s32 h) {
                (void)w;
                (void)h;
                r->drawString("見つかりません", false, x + 16, y + 34, 22, COL_TEXT);
                r->drawString("アルバムが空か、場所が違います", false, x + 16, y + 64, 17, COL_SUB);
            }), 90);
        }
        for (auto &m : v) {
            std::vector<u8> bmp;
            makeThumb(m, bmp);
            bool over = m.size > (long long)FIXED_LIMIT_MB * 1048576LL;
            auto *item = new MediaItem(timeLabel(m.name), sizeLabel(m), over, std::move(bmp));
            MediaInfo mc = m;
            item->setClickListener([mc](u64 keys) {
                if (!(keys & HidNpadButton_A)) return false;
                tsl::changeTo<ViewGui>(mc);
                return true;
            });
            list->addItem(item);
        }
        frame->setNXContent(list);
        return frame;
    }

private:
    std::string m_album;
};

// ---- 画面 ----
class MainGui : public tsl::Gui {
public:
    MainGui() {}

    tsl::elm::Element *createUI() override {
        auto *frame = new NXFrame("v" OVL_VERSION);
        auto *list = new tsl::elm::List();

        loadCfg(m_cfg);
        // 送る上限は20MBに固定: ちがう値なら、開いたときにそろえる
        if (m_cfg.ok && m_cfg.limitMB != FIXED_LIMIT_MB && setKey("limit", std::to_string(FIXED_LIMIT_MB))) m_cfg.limitMB = FIXED_LIMIT_MB;
        m_state = sysmoduleState();
        m_note = m_cfg.ok ? "変えると、数秒で反映されます" : "";

        // 状態のカード
        list->addItem(new tsl::elm::CustomDrawer([this](tsl::gfx::Renderer *r, s32 x, s32 y, s32 w, s32 h) { drawCard(r, x, y, w, h); }), 156);

        if (!m_cfg.ok) {
            frame->setNXContent(list);
            return frame;
        }

        // アルバム
        list->addItem(makeHeader("アルバム"), HEADER_H);
        auto *recent = new NXRow("直近の写真・動画");
        recent->setValue(std::to_string(RECENT_COUNT) + "件  >");
        std::string album = m_cfg.album;
        recent->setClickListener([album](u64 keys) {
            if (!(keys & HidNpadButton_A)) return false;
            tsl::changeTo<RecentGui>(album);
            return true;
        });
        list->addItem(recent);

        // 送る・通知
        list->addItem(makeHeader("送る・通知"), HEADER_H);
        addToggle(list, "写真・動画の自動送信", "auto", &m_cfg.autoSend);
        addToggle(list, "ゲームの開始・終了の通知", "notify", &m_cfg.notify);

        // 送信先
        if (m_cfg.hookNames.size() > 1) {
            list->addItem(makeHeader("送信先"), HEADER_H);
            m_hook = new NXRow("いまの送信先");
            m_hook->setValue(m_cfg.hookNames[(size_t)(m_cfg.use - 1)]);
            m_hook->setClickListener([this](u64 keys) {
                if (!(keys & HidNpadButton_A)) return false;
                int next = m_cfg.use + 1;
                if (next > (int)m_cfg.hookNames.size()) next = 1;
                if (setKey("use", std::to_string(next))) {
                    m_cfg.use = next;
                    m_hook->setValue(m_cfg.hookNames[(size_t)(next - 1)]);
                    say("送信先を変えました");
                } else {
                    say("保存できませんでした");
                }
                return true;
            });
            list->addItem(m_hook);
        }

        // 動画・見た目
        list->addItem(makeHeader("動画・見た目"), HEADER_H);
        auto *lim = new NXRow("送る上限");
        lim->setValue(std::to_string(FIXED_LIMIT_MB) + " MB(固定)");
        list->addItem(lim);

        addToggle(list, "大きい動画を削って送る", "trim", &m_cfg.trim);

        m_icon = new NXRow("ゲームのアイコン  Aで切替");
        m_icon->setValue(iconText(m_cfg.icon));
        m_icon->setClickListener([this](u64 keys) {
            if (!(keys & HidNpadButton_A)) return false;
            int next = (m_cfg.icon + 1) % 4;
            if (setKey("icon", std::to_string(next))) {
                m_cfg.icon = next;
                m_icon->setValue(iconText(next));
                say("アイコンを変えました");
            } else {
                say("保存できませんでした");
            }
            return true;
        });
        list->addItem(m_icon);

        addToggle(list, "通知の下に NXShot を出す", "footer", &m_cfg.footer);

        frame->setNXContent(list);
        return frame;
    }

    void update() override {
        // 2秒ごとに、常駐版の状態を確かめ直す
        u64 now = armGetSystemTick();
        if (now - m_lastTick > armNsToTicks(2000000000ULL)) {
            m_lastTick = now;
            m_state = sysmoduleState();
        }
    }

private:
    Cfg m_cfg;
    NXRow *m_hook = nullptr, *m_icon = nullptr;
    u64 m_lastTick = 0;
    int m_state = -1;
    std::string m_note;

    static const char *iconText(int i) {
        static const char *N[] = {"なし", "左上", "右上", "フッター"};
        return (i >= 0 && i <= 3) ? N[i] : "左上";
    }

    static const char *onOff(bool b) { return b ? "オン" : "オフ"; }

    void say(const char *s) { m_note = s; }

    // 上のカード: 常駐版の状態、いまの送信先、設定のまとめ
    void drawCard(tsl::gfx::Renderer *r, s32 x, s32 y, s32 w, s32 h) {
        const s32 cx = x, cy = y + 4, cw = w, ch = h - 12;
        r->drawRect(cx, cy, cw, ch, COL_PANEL);
        for (int i = 0; i < 8; i++) r->drawRect(cx, cy + ch * i / 8, 4, ch / 8 + 1, gradColor(i, 8));  // 左の目印(ピンク→紫)
        if (!m_cfg.ok) {
            r->drawString("config.txt が読めません", false, cx + 22, cy + 50, 24, COL_BAD);
            r->drawString("先にアプリで設定してください", false, cx + 22, cy + 86, 18, COL_SUB);
            return;
        }
        tsl::Color sc = m_state == 1 ? COL_GOOD : (m_state == 0 ? COL_BAD : COL_WARN);
        r->drawRect(cx + 22, cy + 20, 10, 10, sc);
        r->drawString((std::string("常駐版 ") + stateText(m_state)).c_str(), false, cx + 40, cy + 30, 18, sc);
        r->drawString(m_cfg.hookNames[(size_t)(m_cfg.use - 1)].c_str(), false, cx + 22, cy + 64, 26, COL_TEXT);
        std::string l3 = std::string("上限 ") + std::to_string(FIXED_LIMIT_MB) + "MB  ・  自動送信 " + onOff(m_cfg.autoSend) + "  ・  通知 " + onOff(m_cfg.notify);
        r->drawString(l3.c_str(), false, cx + 22, cy + 92, 16, COL_SUB);
        r->drawString(m_note.c_str(), false, cx + 22, cy + 118, 17, COL_GOOD);
    }

    void addToggle(tsl::elm::List *list, const char *label, const char *key, bool *field) {
        auto *t = new NXRow(label);
        std::string k(key);
        t->setToggle(*field, [this, k, field](bool on) {
            bool ok = setKey(k.c_str(), on ? "1" : "0");
            if (ok) *field = on;
            say(ok ? "保存しました" : "保存できませんでした");
        });
        list->addItem(t);
    }
};

class NXShotOverlay : public tsl::Overlay {
public:
    void initServices() override {
        fsdevMountSdmc();
        g_pmOk = R_SUCCEEDED(pmdmntInitialize());
    }
    void exitServices() override {
        if (g_pmOk) pmdmntExit();
        fsdevUnmountAll();
    }
    std::unique_ptr<tsl::Gui> loadInitialGui() override { return initially<MainGui>(); }
};

int main(int argc, char **argv) {
    return tsl::loop<NXShotOverlay>(argc, argv);
}
