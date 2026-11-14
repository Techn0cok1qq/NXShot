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

// ---- 画面の枠(「NXShot」の文字を、アイコンのグラデーションにする) ----
// 本来の題は空にして、同じ場所に、1文字ずつ色を変えた「NXShot」を重ねて描く。
// 色は、アイコンの「N」(ピンク→紫)から、右上の丸(青みのある紫)へ
static tsl::Color gradColor(int i, int n) {
    // 色の目印(RGB各16段階): ピンク → 紫 → 青紫
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

class NXFrame : public tsl::elm::OverlayFrame {
public:
    explicit NXFrame(const std::string &subtitle) : tsl::elm::OverlayFrame("", subtitle) {}

    void draw(tsl::gfx::Renderer *renderer) override {
        tsl::elm::OverlayFrame::draw(renderer);
        // 1文字ずつ描くと、文字の幅の取り方によっては N X S が重なって崩れる。
        // なので「NXShot」は必ず1度に(文字の間隔は、ふつうの文字と同じに)描き、
        // 色の変化は、縦に細く切った帯ごとに、画面の描く範囲を絞って(scissor)つける。
        const char *TITLE = "NXShot";
        const s32 X0 = 20, Y = 50, SZ = 32;
        // いちど透明で描いて、文字の形を準備しつつ、幅を測る(測れなかったときは、標準の幅を使う)
        auto dim = renderer->drawString(TITLE, false, X0, Y, SZ, tsl::Color(0, 0, 0, 0));
        s32 W = (s32)dim.first;
        if (W < 80 || W > 220) W = 125;
        const int BANDS = 12;
        for (int i = 0; i < BANDS; i++) {
            s32 x0 = X0 + W * i / BANDS;
            s32 x1 = (i == BANDS - 1) ? X0 + W + 40 : X0 + W * (i + 1) / BANDS;
            renderer->enableScissoring(x0, 10, x1 - x0, 62);
            renderer->drawString(TITLE, false, X0, Y, SZ, gradColor(i, BANDS));
        }
        renderer->disableScissoring();
    }
};

// ---- 直近の写真・動画の1行(左に時刻と大きさ、右にサムネ) ----
class MediaItem : public tsl::elm::ListItem {
public:
    MediaItem(const std::string &time, const std::string &sub, bool warn, std::vector<u8> bmp)
        : tsl::elm::ListItem(""), m_time(time), m_sub(sub), m_warn(warn), m_bmp(std::move(bmp)) {}

    void draw(tsl::gfx::Renderer *r) override {
        const s32 x = getX(), y = getY(), w = getWidth(), h = getHeight();
        r->drawString(m_time.c_str(), false, x + 16, y + h * 43 / 100, 22, tsl::Color(15, 15, 15, 15));
        r->drawString(m_sub.c_str(), false, x + 16, y + h * 78 / 100, 17, m_warn ? tsl::Color(15, 12, 4, 15) : tsl::Color(10, 10, 10, 15));
        if (!m_bmp.empty()) r->drawBitmap(x + w - THUMB_W - 12, y + (h - THUMB_H) / 2, THUMB_W, THUMB_H, m_bmp.data());
        r->drawRect(x, y + h - 1, w, 1, tsl::Color(5, 5, 5, 15));
    }

private:
    std::string m_time, m_sub;
    bool m_warn;
    std::vector<u8> m_bmp;
};

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

// 結果: 0 まだ / 1 送れた / 2 大きすぎ / 3 失敗 / 4 送れないファイル
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
    return 0;
}

// ---- 1件を大きく見て、送る画面 ----
// 写真: 大きく表示。動画: 中の絵(あれば)を表示。動画の再生は、オーバーレイでは(メモリの都合で)できない。
class ViewGui : public tsl::Gui {
public:
    explicit ViewGui(MediaInfo m) : m_m(std::move(m)) {}

    tsl::elm::Element *createUI() override {
        auto *frame = new NXFrame(timeLabel(m_m.name));
        if (m_m.video) {
            if (!videoEmbeddedThumb(m_m.path, VIEW_W, VIEW_H, m_bmp)) videoMark(m_bmp, VIEW_W, VIEW_H);
        } else if (!jpegPhoto(m_m.path, VIEW_W, VIEW_H, m_bmp)) {
            fillRGBA(m_bmp, VIEW_W, VIEW_H, 40, 40, 40);
        }
        m_hint = "A: Discordへ送る    B: もどる";
        auto *pane = new tsl::elm::CustomDrawer([this](tsl::gfx::Renderer *r, s32 x, s32 y, s32 w, s32 h) {
            (void)h;
            const s32 bx = x + (w - VIEW_W) / 2;
            r->drawBitmap(bx, y + 14, VIEW_W, VIEW_H, m_bmp.data());
            const s32 ty = y + 14 + VIEW_H + 34;
            r->drawString(sizeLabel(m_m).c_str(), false, bx, ty, 22, tsl::Color(15, 15, 15, 15));
            if (m_m.video) r->drawString("※動画の再生は、アプリ版でできます", false, bx, ty + 30, 17, tsl::Color(10, 10, 10, 15));
            r->drawString(m_msg.c_str(), false, bx, ty + 66, 20, m_msgColor);
            r->drawString(m_hint.c_str(), false, bx, ty + 104, 18, tsl::Color(11, 11, 11, 15));
        });
        frame->setContent(pane);
        return frame;
    }

    bool handleInput(u64 keysDown, u64, const HidTouchState &, HidAnalogStickState, HidAnalogStickState) override {
        if (keysDown & HidNpadButton_B) {
            tsl::goBack();
            return true;
        }
        if (keysDown & HidNpadButton_A) {
            if (m_pending) return true;  // 送っている最中は、もう一度は送らない
            if (sysmoduleState() == 0) {
                setMsg("常駐版が止まっています", true);
                return true;
            }
            if (requestSend(m_m.path)) {
                m_pending = true;
                m_since = armGetSystemTick();
                setMsg("送っています…(しばらくかかります)", false);
            } else {
                setMsg("依頼を書けませんでした", true);
            }
            return true;
        }
        return false;
    }

    void update() override {
        if (!m_pending) return;
        int r = readSendResult();
        if (r == 1) setMsg("送りました", false);
        else if (r == 2) setMsg("大きすぎて送れませんでした", true);
        else if (r == 3) setMsg("送れませんでした(通信か送信先)", true);
        else if (r == 4) setMsg("このファイルは送れません", true);
        else if (armGetSystemTick() - m_since > armNsToTicks(180000000000ULL)) {
            setMsg("返事がありません。常駐版を1.3.3にしてください", true);
            r = 5;
        }
        if (r != 0) {
            m_pending = false;
            remove(OVL_RES_PATH);
        }
    }

private:
    MediaInfo m_m;
    std::vector<u8> m_bmp;
    std::string m_msg, m_hint;
    tsl::Color m_msgColor = tsl::Color(15, 15, 15, 15);
    bool m_pending = false;
    u64 m_since = 0;

    void setMsg(const char *s, bool bad) {
        m_msg = s;
        m_msgColor = bad ? tsl::Color(15, 8, 6, 15) : tsl::Color(8, 15, 9, 15);
    }
};

// ---- 直近の写真・動画の画面 ----
class RecentGui : public tsl::Gui {
public:
    explicit RecentGui(std::string album) : m_album(std::move(album)) {}

    tsl::elm::Element *createUI() override {
        auto *frame = new NXFrame("直近の写真・動画");
        auto *list = new tsl::elm::List();
        list->addItem(new tsl::elm::CategoryHeader("新しい順(最大" + std::to_string(RECENT_COUNT) + "件)  Aで開く"));
        std::vector<MediaInfo> v = recentMedia(m_album, RECENT_COUNT);
        if (v.empty()) {
            auto *none = new tsl::elm::ListItem("見つかりません");
            none->setValue("アルバムが空か、場所が違います");
            list->addItem(none);
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
        frame->setContent(list);
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

        // 状態
        list->addItem(new tsl::elm::CategoryHeader("状態"));
        m_status = new tsl::elm::ListItem("常駐版");
        m_status->setValue(stateText(sysmoduleState()));
        list->addItem(m_status);

        if (!m_cfg.ok) {
            auto *err = new tsl::elm::ListItem("config.txt が読めません");
            err->setValue("先にアプリで設定");
            list->addItem(err);
            frame->setContent(list);
            return frame;
        }

        m_msg = new tsl::elm::ListItem("保存");
        m_msg->setValue("変えると、数秒で反映");
        list->addItem(m_msg);

        // 直近の写真・動画
        list->addItem(new tsl::elm::CategoryHeader("アルバム"));
        auto *recent = new tsl::elm::ListItem("直近の写真・動画  Aで開く");
        recent->setValue(std::to_string(RECENT_COUNT) + "件");
        std::string album = m_cfg.album;
        recent->setClickListener([album](u64 keys) {
            if (!(keys & HidNpadButton_A)) return false;
            tsl::changeTo<RecentGui>(album);
            return true;
        });
        list->addItem(recent);

        // 送る・通知
        list->addItem(new tsl::elm::CategoryHeader("送る・通知"));
        addToggle(list, "写真・動画の自動送信", "auto", m_cfg.autoSend);
        addToggle(list, "ゲームの開始・終了の通知", "notify", m_cfg.notify);

        // 送信先
        if (m_cfg.hookNames.size() > 1) {
            list->addItem(new tsl::elm::CategoryHeader("送信先"));
            m_hook = new tsl::elm::ListItem("いまの送信先");
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
        list->addItem(new tsl::elm::CategoryHeader("動画・見た目"));
        auto *lim = new tsl::elm::ListItem("送る上限");
        lim->setValue(std::to_string(FIXED_LIMIT_MB) + " MB(固定)");
        list->addItem(lim);

        addToggle(list, "大きい動画を削って送る", "trim", m_cfg.trim);

        m_icon = new tsl::elm::ListItem("ゲームのアイコン  Aで切替");
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

        addToggle(list, "通知の下に NXShot を出す", "footer", m_cfg.footer);

        frame->setContent(list);
        return frame;
    }

    void update() override {
        // 2秒ごとに、常駐版の状態を確かめ直す
        u64 now = armGetSystemTick();
        if (m_status && now - m_lastTick > armNsToTicks(2000000000ULL)) {
            m_lastTick = now;
            m_status->setValue(stateText(sysmoduleState()));
        }
    }

private:
    Cfg m_cfg;
    tsl::elm::ListItem *m_status = nullptr, *m_msg = nullptr, *m_hook = nullptr, *m_icon = nullptr;
    u64 m_lastTick = 0;

    static const char *iconText(int i) {
        static const char *N[] = {"なし", "左上", "右上", "フッター"};
        return (i >= 0 && i <= 3) ? N[i] : "左上";
    }

    void say(const char *s) {
        if (m_msg) m_msg->setValue(s);
    }

    void addToggle(tsl::elm::List *list, const char *label, const char *key, bool initial) {
        auto *t = new tsl::elm::ToggleListItem(label, initial);
        std::string k(key);
        t->setStateChangedListener([this, k](bool on) {
            say(setKey(k.c_str(), on ? "1" : "0") ? "保存しました" : "保存できませんでした");
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
