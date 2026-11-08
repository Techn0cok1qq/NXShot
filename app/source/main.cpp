// NXShot アプリ (.nro)
// 常駐版(sysmodule)とは別に、ホームブリューメニューから起動して使う。
//   ・Albumの古い写真・動画を、年・月でまとめて表示し(サムネ・動画プレビュー付き)、選んでDiscordのWebhookへ送る
//     (大きい動画は削って収める)
//   ・設定(config.txt。自動送信のオン/オフも)を、Switchの画面で変える
//   ・テスト送信 / 常駐版の状態とログの確認
// 依存: libnx + SDL2 + SDL2_ttf + SDL2_image + libcurl (+ あれば ffmpeg)。日本語はSwitch本体の共有フォントで表示する。
#include <switch.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <SDL2/SDL_image.h>
#include <curl/curl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdarg>
#include <cmath>
#include <cerrno>
#include <string>
#include <vector>
#include <set>
#include <unordered_map>
#include <algorithm>
#ifdef NXSHOT_FFMPEG  // Makefileが switch-ffmpeg を見つけたときだけ入る(動画のサムネとプレビュー)
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}
#endif
#include "../../sysmodule/source/mp4trim.h"

#define APP_VERSION_STR "1.2.1"
#define CONFIG_DIR "sdmc:/config/nxdiscord"
#define CONFIG_PATH "sdmc:/config/nxdiscord/config.txt"
#define SYS_LOG_PATH "sdmc:/config/nxdiscord/log.txt"
#define APP_LOG_PATH "sdmc:/config/nxdiscord/app_log.txt"
#define SENT_PATH "sdmc:/config/nxdiscord/app_sent.txt"
#define TRIM_TMP_PATH "sdmc:/config/nxdiscord/trim_tmp_app.mp4"
#define DEFAULT_ALBUM "sdmc:/Nintendo/Album"
#define DEFAULT_LIMIT_MB 10
#define HOOK_NAME "NX Switch"  // 名前に「discord」を含めるとDiscordに拒否される
#define SYS_TITLE_ID 0x4200000000000A7EULL
#define SYS_NSP_PATH "sdmc:/atmosphere/contents/4200000000000A7E/exefs.nsp"
#define SELF_DEFAULT_PATH "sdmc:/switch/NXShot/NXShot.nro"
#define UPDATE_REPO "Techn0cok1qq/NXShot"   // アップデートの取得元(GitHub Releases)
#define UPDATE_NRO_NAME "NXShot.nro"        // Releaseに付いているアプリ本体
#define UPDATE_NSP_NAME "nxdiscord.nsp"     // Releaseに付いている常駐版

// ====================================================================
// ログ(アプリ自身の動作記録。Webhook URLは書かない)
// ====================================================================
static bool g_logFirst = true;
static int g_logCount = 0;
static void applog(const char *fmt, ...) {
    if (g_logCount >= 300) return;
    FILE *f = fopen(APP_LOG_PATH, g_logFirst ? "w" : "a");
    if (!f) return;
    g_logFirst = false;
    g_logCount++;
    fprintf(f, "[%llus] ", (unsigned long long)(armTicksToNs(armGetSystemTick()) / 1000000000ULL));
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

// ====================================================================
// 設定 (config.txt。常駐版と同じ形式)
//   1行目: Webhook URL / limit=10 / trim=1 / auto=1 / album=sdmc:/...
// ====================================================================
struct Config {
    std::string hook;
    std::string album = DEFAULT_ALBUM;
    long limitMB = DEFAULT_LIMIT_MB;
    bool trim = true;
    bool autoSend = true;  // 常駐版が写真・動画を自動で送るか(auto=1/0)
    bool updCheck = true;  // 起動時にアップデートを確認するか(updcheck=1/0)
};
static Config g_cfg;

static std::string trimLine(const char *line) {
    std::string s(line);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && s[i] == ' ') i++;
    return s.substr(i);
}

static void loadConfig() {
    g_cfg = Config();
    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) return;
    char line[512];
    int n = 0;
    std::string album;
    while (n < 12 && fgets(line, sizeof(line), f)) {
        n++;
        std::string t = trimLine(line);
        if (n == 1) { g_cfg.hook = t; continue; }
        if (t.empty()) continue;
        if (t.compare(0, 6, "limit=") == 0) {
            long v = atol(trimLine(t.c_str() + 6).c_str());
            if (v >= 1 && v <= 500) g_cfg.limitMB = v;
        } else if (t.compare(0, 5, "trim=") == 0) {
            g_cfg.trim = atol(trimLine(t.c_str() + 5).c_str()) != 0;
        } else if (t.compare(0, 5, "auto=") == 0) {
            g_cfg.autoSend = atol(trimLine(t.c_str() + 5).c_str()) != 0;
        } else if (t.compare(0, 9, "updcheck=") == 0) {
            g_cfg.updCheck = atol(trimLine(t.c_str() + 9).c_str()) != 0;
        } else if (t.compare(0, 6, "album=") == 0) {
            album = trimLine(t.c_str() + 6);
        } else if (album.empty() && t.compare(0, 6, "sdmc:/") == 0) {
            album = t;
        }
    }
    fclose(f);
    if (!album.empty()) g_cfg.album = album;
    while (g_cfg.album.size() > 1 && g_cfg.album.back() == '/') g_cfg.album.pop_back();
}

static bool hookOk() { return g_cfg.hook.compare(0, 8, "https://") == 0; }

static bool saveConfig() {
    mkdir("sdmc:/config", 0777);
    mkdir(CONFIG_DIR, 0777);
    FILE *f = fopen(CONFIG_PATH, "w");
    if (!f) return false;
    fprintf(f, "%s\n", g_cfg.hook.c_str());
    fprintf(f, "limit=%ld\n", g_cfg.limitMB);
    fprintf(f, "trim=%d\n", g_cfg.trim ? 1 : 0);
    fprintf(f, "auto=%d\n", g_cfg.autoSend ? 1 : 0);
    fprintf(f, "updcheck=%d\n", g_cfg.updCheck ? 1 : 0);
    if (g_cfg.album != DEFAULT_ALBUM) fprintf(f, "album=%s\n", g_cfg.album.c_str());
    fclose(f);
    return true;
}

// Webhook URLを画面に出すときは、末尾の数文字だけ見せる
static std::string maskHook(const std::string &h) {
    if (h.empty()) return "未設定";
    size_t p = h.find("/webhooks/");
    std::string head = (p == std::string::npos) ? h.substr(0, std::min<size_t>(h.size(), 24)) : h.substr(0, p + 10);
    std::string tail = h.size() > 4 ? h.substr(h.size() - 4) : "";
    return head + "..." + tail;
}

// ====================================================================
// 送信済みの記録(このアプリで送ったファイル名)
// ====================================================================
static std::set<std::string> g_sent;

static void loadSent() {
    FILE *f = fopen(SENT_PATH, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        std::string t = trimLine(line);
        if (!t.empty()) g_sent.insert(t);
    }
    fclose(f);
}

static void addSent(const std::string &name) {
    if (g_sent.insert(name).second) {
        FILE *f = fopen(SENT_PATH, "a");
        if (f) { fprintf(f, "%s\n", name.c_str()); fclose(f); }
    }
}

// ====================================================================
// 画面まわり(SDL)
// ====================================================================
static SDL_Window *g_win = nullptr;
static SDL_Renderer *g_ren = nullptr;
static TTF_Font *fTitle = nullptr, *fBody = nullptr, *fSmall = nullptr;
static bool g_quit = false;

struct Tex { SDL_Texture *t; int w, h; };
static std::unordered_map<std::string, Tex> g_texCache;

static const SDL_Color C_BG = {20, 21, 28, 255};
static const SDL_Color C_BAR = {26, 28, 40, 255};
static const SDL_Color C_CARD = {34, 37, 54, 255};
static const SDL_Color C_CARD2 = {46, 50, 74, 255};
static const SDL_Color C_TEXT = {236, 238, 247, 255};
static const SDL_Color C_DIM = {146, 152, 176, 255};
static const SDL_Color C_ACC = {88, 101, 242, 255};
static const SDL_Color C_ACC2 = {140, 150, 255, 255};
static const SDL_Color C_OK = {87, 242, 135, 255};
static const SDL_Color C_NG = {240, 82, 90, 255};
static const SDL_Color C_WARN = {254, 218, 92, 255};
static const SDL_Color C_WHITE = {255, 255, 255, 255};
static SDL_Texture *g_bg = nullptr;  // 背景のグラデーション

static void fillRect(int x, int y, int w, int h, SDL_Color c) {
    SDL_SetRenderDrawColor(g_ren, c.r, c.g, c.b, c.a);
    SDL_Rect r = {x, y, w, h};
    SDL_RenderFillRect(g_ren, &r);
}

static void frameRect(int x, int y, int w, int h, SDL_Color c) {
    SDL_SetRenderDrawColor(g_ren, c.r, c.g, c.b, c.a);
    SDL_Rect r = {x, y, w, h};
    SDL_RenderDrawRect(g_ren, &r);
}

// ---- 角丸の四角・三角・太い線(SDLの基本の描画だけで作る) ----
// 角丸の四角で、row行目(0〜h-1)の左右が、角の丸みでどれだけ内側に寄るか
static int rrInset(int row, int h, int r) {
    int rr = row < h - 1 - row ? row : h - 1 - row;
    if (r <= 0 || rr >= r) return 0;
    double dy = r - (rr + 0.5);
    double dx = std::sqrt((double)r * r - dy * dy);
    int ins = (int)std::lround(r - dx);
    return ins < 0 ? 0 : ins;
}

static void fillRRect(int x, int y, int w, int h, int r, SDL_Color c) {
    if (w <= 0 || h <= 0) return;
    if (r * 2 > h) r = h / 2;
    if (r * 2 > w) r = w / 2;
    SDL_SetRenderDrawColor(g_ren, c.r, c.g, c.b, c.a);
    for (int row = 0; row < h; row++) {
        if (row >= r && row < h - r) {
            SDL_Rect m = {x, y + row, w, h - 2 * r};
            SDL_RenderFillRect(g_ren, &m);
            row = h - r - 1;
            continue;
        }
        int ins = rrInset(row, h, r);
        SDL_Rect s = {x + ins, y + row, w - 2 * ins, 1};
        SDL_RenderFillRect(g_ren, &s);
    }
}

// 角丸の枠線(中は塗らない。サムネの上にも重ねられる)
static void strokeRRect(int x, int y, int w, int h, int r, int t, SDL_Color c) {
    if (w <= 2 * t || h <= 2 * t) return;
    if (r * 2 > h) r = h / 2;
    if (r * 2 > w) r = w / 2;
    SDL_SetRenderDrawColor(g_ren, c.r, c.g, c.b, c.a);
    int ri = r - t > 0 ? r - t : 0;
    int ih = h - 2 * t;
    for (int row = 0; row < h; row++) {
        if (row >= r && row < h - r) {
            int len = h - 2 * r;
            SDL_Rect a = {x, y + row, t, len};
            SDL_Rect b = {x + w - t, y + row, t, len};
            SDL_RenderFillRect(g_ren, &a);
            SDL_RenderFillRect(g_ren, &b);
            row = h - r - 1;
            continue;
        }
        int oi = rrInset(row, h, r);
        if (row < t || row >= h - t) {
            SDL_Rect s = {x + oi, y + row, w - 2 * oi, 1};
            SDL_RenderFillRect(g_ren, &s);
            continue;
        }
        int ii = t + rrInset(row - t, ih, ri);
        if (ii < oi) ii = oi;
        if (ii - oi <= 0) continue;
        SDL_Rect a = {x + oi, y + row, ii - oi, 1};
        SDL_Rect b = {x + w - ii, y + row, ii - oi, 1};
        SDL_RenderFillRect(g_ren, &a);
        SDL_RenderFillRect(g_ren, &b);
    }
}

static void fillTri(int x0, int y0, int x1, int y1, int x2, int y2, SDL_Color c) {
    SDL_SetRenderDrawColor(g_ren, c.r, c.g, c.b, c.a);
    int xs[3] = {x0, x1, x2}, ys[3] = {y0, y1, y2};
    int miny = std::min(y0, std::min(y1, y2)), maxy = std::max(y0, std::max(y1, y2));
    for (int y = miny; y <= maxy; y++) {
        double lo = 1e9, hi = -1e9;
        for (int i = 0; i < 3; i++) {
            int j = (i + 1) % 3;
            int ya = ys[i], yb = ys[j];
            if (ya == yb) {
                if (y == ya) {
                    lo = std::min(lo, (double)std::min(xs[i], xs[j]));
                    hi = std::max(hi, (double)std::max(xs[i], xs[j]));
                }
                continue;
            }
            if ((y < ya && y < yb) || (y > ya && y > yb)) continue;
            double t = (double)(y - ya) / (double)(yb - ya);
            double x = xs[i] + t * (xs[j] - xs[i]);
            lo = std::min(lo, x);
            hi = std::max(hi, x);
        }
        if (hi >= lo) SDL_RenderDrawLine(g_ren, (int)std::floor(lo), y, (int)std::ceil(hi), y);
    }
}

static void thickLine(int x0, int y0, int x1, int y1, int t, SDL_Color c) {
    SDL_SetRenderDrawColor(g_ren, c.r, c.g, c.b, c.a);
    for (int dx = -(t / 2); dx <= t / 2; dx++)
        for (int dy = -(t / 2); dy <= t / 2; dy++) SDL_RenderDrawLine(g_ren, x0 + dx, y0 + dy, x1 + dx, y1 + dy);
}

// 背景: 縦に暗くなるグラデーションに、右上と左下にうっすら色をのせる(小さい画像を引き伸ばして使う)
static void makeBackground() {
    const int W = 96, H = 54;
    std::vector<unsigned char> px((size_t)W * H * 4);
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            double u = (double)x / (W - 1), v = (double)y / (H - 1);
            double r = 27 - 13 * v, g = 29 - 14 * v, b = 43 - 21 * v;
            double dx1 = (u - 0.88) * 1.78, dy1 = v;
            double k1 = std::max(0.0, 1.0 - std::sqrt(dx1 * dx1 + dy1 * dy1) / 0.75);
            k1 = k1 * k1 * 0.30;
            r += (88 - r) * k1; g += (101 - g) * k1; b += (242 - b) * k1;
            double dx2 = u * 1.78, dy2 = 1.0 - v;
            double k2 = std::max(0.0, 1.0 - std::sqrt(dx2 * dx2 + dy2 * dy2) / 0.7);
            k2 = k2 * k2 * 0.16;
            r += (170 - r) * k2; g += (70 - g) * k2; b += (200 - b) * k2;
            unsigned char *q = &px[((size_t)y * W + x) * 4];
            q[0] = (unsigned char)std::min(255.0, std::max(0.0, r));
            q[1] = (unsigned char)std::min(255.0, std::max(0.0, g));
            q[2] = (unsigned char)std::min(255.0, std::max(0.0, b));
            q[3] = 255;
        }
    }
    g_bg = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, W, H);
    if (g_bg) SDL_UpdateTexture(g_bg, nullptr, px.data(), W * 4);
}

static void drawBackground() {
    if (g_bg) {
        SDL_RenderCopy(g_ren, g_bg, nullptr, nullptr);
    } else {
        SDL_SetRenderDrawColor(g_ren, C_BG.r, C_BG.g, C_BG.b, 255);
        SDL_RenderClear(g_ren);
    }
}

// 幅に収まるよう、末尾を「…」で省く(UTF-8の文字の途中では切らない)
static std::string fitText(TTF_Font *f, const std::string &s, int maxW) {
    int w = 0, h = 0;
    if (TTF_SizeUTF8(f, s.c_str(), &w, &h) == 0 && w <= maxW) return s;
    std::string t = s;
    while (!t.empty()) {
        while (!t.empty() && ((unsigned char)t.back() & 0xC0) == 0x80) t.pop_back();  // 続きのバイトを先に落とす
        if (!t.empty()) t.pop_back();                                                   // 最後に先頭のバイト
        std::string u = t + "...";
        if (TTF_SizeUTF8(f, u.c_str(), &w, &h) == 0 && w <= maxW) return u;
    }
    return "";
}

// align: 0=左揃え 1=右揃え(xが右端) 2=中央(xが中心)
static void drawText(int x, int y, const std::string &s, TTF_Font *f, SDL_Color c, int align = 0) {
    if (s.empty() || !f) return;
    char key[64];
    snprintf(key, sizeof(key), "%p|%02x%02x%02x|", (void *)f, c.r, c.g, c.b);
    std::string k = std::string(key) + s;
    auto it = g_texCache.find(k);
    if (it == g_texCache.end()) {
        if (g_texCache.size() > 500) {
            for (auto &p : g_texCache) SDL_DestroyTexture(p.second.t);
            g_texCache.clear();
        }
        SDL_Surface *sf = TTF_RenderUTF8_Blended(f, s.c_str(), c);
        if (!sf) return;
        Tex t;
        t.t = SDL_CreateTextureFromSurface(g_ren, sf);
        t.w = sf->w;
        t.h = sf->h;
        SDL_FreeSurface(sf);
        if (!t.t) return;
        it = g_texCache.emplace(k, t).first;
    }
    int dx = x;
    if (align == 1) dx = x - it->second.w;
    else if (align == 2) dx = x - it->second.w / 2;
    SDL_Rect dst = {dx, y, it->second.w, it->second.h};
    SDL_RenderCopy(g_ren, it->second.t, nullptr, &dst);
}

// ---- 入力(コントローラのボタンを毎フレーム読む。libnxのpad APIを使う) ----
enum { BT_A = 0, BT_B = 1, BT_X = 2, BT_Y = 3, BT_L = 6, BT_R = 7, BT_ZL = 8, BT_ZR = 9,
       BT_PLUS = 10, BT_MINUS = 11, BT_LEFT = 12, BT_UP = 13, BT_RIGHT = 14, BT_DOWN = 15 };

static PadState g_pad;
static u64 g_mask[16];
static bool g_cur[16], g_prev[16], g_press[16];
static Uint32 g_holdStart[16], g_lastRep[16];

static void initInput() {
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&g_pad);
    g_mask[BT_A] = HidNpadButton_A;
    g_mask[BT_B] = HidNpadButton_B;
    g_mask[BT_X] = HidNpadButton_X;
    g_mask[BT_Y] = HidNpadButton_Y;
    g_mask[BT_L] = HidNpadButton_L;
    g_mask[BT_R] = HidNpadButton_R;
    g_mask[BT_ZL] = HidNpadButton_ZL;
    g_mask[BT_ZR] = HidNpadButton_ZR;
    g_mask[BT_PLUS] = HidNpadButton_Plus;
    g_mask[BT_MINUS] = HidNpadButton_Minus;
    // 左スティックも十字キーとして使えるようにする
    g_mask[BT_LEFT] = HidNpadButton_Left | HidNpadButton_StickLLeft;
    g_mask[BT_UP] = HidNpadButton_Up | HidNpadButton_StickLUp;
    g_mask[BT_RIGHT] = HidNpadButton_Right | HidNpadButton_StickLRight;
    g_mask[BT_DOWN] = HidNpadButton_Down | HidNpadButton_StickLDown;
}

static void updateInput() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) g_quit = true;
    }
    padUpdate(&g_pad);
    u64 held = padGetButtons(&g_pad);
    Uint32 now = SDL_GetTicks();
    for (int b = 0; b < 16; b++) {
        g_prev[b] = g_cur[b];
        g_cur[b] = g_mask[b] != 0 && (held & g_mask[b]) != 0;
        g_press[b] = g_cur[b] && !g_prev[b];
        if (g_press[b]) { g_holdStart[b] = now; g_lastRep[b] = now; }
        // 十字キーは押しっぱなしで繰り返す
        bool dir = (b == BT_UP || b == BT_DOWN || b == BT_LEFT || b == BT_RIGHT);
        if (dir && g_cur[b] && g_prev[b] && now - g_holdStart[b] > 350 && now - g_lastRep[b] > 70) {
            g_press[b] = true;
            g_lastRep[b] = now;
        }
    }
}

static bool pressed(int b) { return g_press[b]; }

static void beginFrame() {
    updateInput();
    drawBackground();
}

static void endFrame() {
    SDL_RenderPresent(g_ren);
}

static int textW(TTF_Font *f, const std::string &s) {
    int w = 0, h = 0;
    if (!f || TTF_SizeUTF8(f, s.c_str(), &w, &h) != 0) return 0;
    return w;
}

// y を「文字の縦の真ん中」で指定して描く
static void drawTextMid(int x, int cy, const std::string &s, TTF_Font *f, SDL_Color c, int align = 0) {
    if (!f) return;
    drawText(x, cy - TTF_FontHeight(f) / 2, s, f, c, align);
}

static void drawCard(int x, int y, int w, int h) {
    fillRRect(x, y, w, h, 12, SDL_Color{34, 37, 54, 215});
    strokeRRect(x, y, w, h, 12, 1, SDL_Color{255, 255, 255, 24});
}

static void drawProgress(int x, int y, int w, int h, double frac) {
    fillRRect(x, y, w, h, h / 2, SDL_Color{255, 255, 255, 30});
    if (frac <= 0) return;
    int fw = (int)(w * std::min(1.0, frac));
    if (fw < h) fw = h;
    fillRRect(x, y, fw, h, h / 2, C_ACC);
}

static void drawHeader(const std::string &sub) {
    fillRect(0, 0, 1280, 72, SDL_Color{16, 17, 26, 215});
    fillRect(0, 72, 1280, 2, SDL_Color{88, 101, 242, 110});
    fillRRect(36, 15, 42, 42, 11, C_ACC);
    drawTextMid(57, 36, "N", fTitle, C_WHITE, 2);
    drawTextMid(94, 36, "NXShot", fTitle, C_TEXT);
    drawTextMid(1244, 36, sub, fBody, C_DIM, 1);
}

// フッター: 「A: 決定」のような部分を、ボタンの形の札つきで並べる(2つ以上の空白で区切る)
static void drawFooter(const std::string &hint) {
    fillRect(0, 664, 1280, 56, SDL_Color{16, 17, 26, 225});
    fillRect(0, 664, 1280, 1, SDL_Color{255, 255, 255, 28});
    std::vector<std::string> segs;
    std::string cur;
    for (size_t i = 0; i < hint.size();) {
        if (hint[i] == ' ' && i + 1 < hint.size() && hint[i + 1] == ' ') {
            if (!cur.empty()) segs.push_back(cur);
            cur.clear();
            while (i < hint.size() && hint[i] == ' ') i++;
        } else {
            cur += hint[i++];
        }
    }
    if (!cur.empty()) segs.push_back(cur);
    int x = 36, cy = 692;
    for (auto &seg : segs) {
        if (x > 1220) break;
        size_t p = seg.find(": ");
        bool chip = p != std::string::npos && p <= 12 && seg.find('(') > p;
        if (chip) {
            std::string key = seg.substr(0, p), label = seg.substr(p + 2);
            int kw = std::max(28, textW(fSmall, key) + 18);
            fillRRect(x, cy - 13, kw, 26, 7, SDL_Color{70, 75, 104, 255});
            drawTextMid(x + kw / 2, cy, key, fSmall, C_WHITE, 2);
            x += kw + 8;
            std::string lb = fitText(fSmall, label, 1244 - x);
            drawTextMid(x, cy, lb, fSmall, C_TEXT);
            x += textW(fSmall, lb) + 26;
        } else {
            std::string lb = fitText(fSmall, seg, 1244 - x);
            drawTextMid(x, cy, lb, fSmall, C_DIM);
            x += textW(fSmall, lb) + 26;
        }
    }
}

// ---- 簡単なメッセージ画面(Aで閉じる) ----
static void showMessage(const std::string &title, const std::vector<std::string> &lines, bool good) {
    while (!g_quit && appletMainLoop()) {
        beginFrame();
        drawHeader("お知らせ");
        int h = 96 + (int)lines.size() * 36 + 24;
        if (h < 180) h = 180;
        drawCard(36, 100, 1208, h);
        SDL_Color tc = good ? C_OK : C_WARN;
        fillRRect(64, 128, 8, 36, 4, tc);
        drawText(88, 124, title, fTitle, tc);
        int y = 188;
        for (auto &l : lines) {
            drawText(88, y, fitText(fBody, l, 1120), fBody, C_TEXT);
            y += 36;
        }
        drawFooter("A: 閉じる");
        endFrame();
        if (pressed(BT_A) || pressed(BT_B)) return;
        SDL_Delay(16);
    }
}

// 「〜しています...」の1枚画面(その場で1回だけ描く)
static void drawStatusScreen(const std::string &sub, const std::string &msg) {
    drawBackground();
    drawHeader(sub);
    drawCard(36, 100, 1208, 96);
    drawTextMid(64, 148, fitText(fBody, msg, 1150), fBody, C_TEXT);
    SDL_RenderPresent(g_ren);
}

// ---- Switchのキーボードで文字を入力する ----
static bool kbdInput(const char *header, const char *guide, const std::string &init, int maxLen,
                     bool numeric, std::string &out) {
    SwkbdConfig kbd;
    Result rc = swkbdCreate(&kbd, 0);
    if (R_FAILED(rc)) { applog("swkbdCreate rc=0x%x", (unsigned)rc); return false; }
    swkbdConfigMakePresetDefault(&kbd);
    swkbdConfigSetHeaderText(&kbd, header);
    swkbdConfigSetGuideText(&kbd, guide);
    swkbdConfigSetInitialText(&kbd, init.c_str());
    swkbdConfigSetStringLenMax(&kbd, (u32)maxLen);
    if (numeric) swkbdConfigSetType(&kbd, SwkbdType_NumPad);
    static char buf[512];
    buf[0] = 0;
    rc = swkbdShow(&kbd, buf, sizeof(buf));
    swkbdClose(&kbd);
    if (R_FAILED(rc)) { applog("swkbdShow rc=0x%x", (unsigned)rc); return false; }
    out = buf;
    return true;
}

// ====================================================================
// 通信(libcurl)
// ====================================================================
static bool g_curlReady = false;
static bool g_nifmOk = false;

static bool networkUp() {
    if (!g_nifmOk) return true;
    NifmInternetConnectionType type;
    u32 strength = 0;
    NifmInternetConnectionStatus status;
    if (R_FAILED(nifmGetInternetConnectionStatus(&type, &strength, &status))) return false;
    return status == NifmInternetConnectionStatus_Connected;
}

static char g_errbuf[CURL_ERROR_SIZE];
static size_t discardCb(char *, size_t size, size_t nmemb, void *) { return size * nmemb; }

// 送信中の表示用
struct SendView {
    std::string title;                                  // 「送信中 2/5」
    std::string file;                                   // ファイル名
    std::string status;                                 // いまの状態
    double frac = 0;                                    // アップロードの進み(0〜1)
    std::vector<std::pair<std::string, int>> results;   // 結果の一覧 (文, 0=ok 1=skip 2=ng)
};
static SendView g_view;
static bool g_cancel = false;

static void drawSending() {
    drawBackground();
    drawHeader("送信中");
    drawCard(36, 92, 1208, 200);
    drawText(64, 104, g_view.title, fTitle, C_TEXT);
    drawText(64, 156, fitText(fBody, g_view.file, 1150), fBody, C_DIM);
    drawText(64, 192, fitText(fBody, g_view.status, 1150), fBody, C_TEXT);
    drawProgress(64, 252, 1152, 14, g_view.frac);
    int y = 312;
    size_t start = g_view.results.size() > 10 ? g_view.results.size() - 10 : 0;
    for (size_t i = start; i < g_view.results.size(); i++) {
        SDL_Color c = g_view.results[i].second == 0 ? C_OK : (g_view.results[i].second == 1 ? C_WARN : C_NG);
        drawText(64, y, fitText(fSmall, g_view.results[i].first, 1150), fSmall, c);
        y += 30;
    }
    drawFooter("B: 中止(いま送っているファイルを止めます)");
    SDL_RenderPresent(g_ren);
}

static int xferCb(void *, curl_off_t, curl_off_t, curl_off_t ultotal, curl_off_t ulnow) {
    static Uint32 last = 0;
    Uint32 now = SDL_GetTicks();
    if (now - last < 100) return 0;
    last = now;
    if (ultotal > 0) g_view.frac = (double)ulnow / (double)ultotal;
    updateInput();
    if (g_quit || pressed(BT_B) || g_cur[BT_B]) { g_cancel = true; return 1; }
    drawSending();
    return 0;
}

static void setCommon(CURL *c, const std::string &url) {
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_USERAGENT, "NXShot-App/" APP_VERSION_STR);
    // SwitchにはCA証明書の束が無いため、サーバー証明書の検証は行わない
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_FORBID_REUSE, 1L);
    curl_easy_setopt(c, CURLOPT_BUFFERSIZE, 16384L);
    curl_easy_setopt(c, CURLOPT_UPLOAD_BUFFERSIZE, 16384L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, discardCb);
    g_errbuf[0] = 0;
    curl_easy_setopt(c, CURLOPT_ERRORBUFFER, g_errbuf);
}

static std::string jsonEscape(const std::string &s) {
    std::string out;
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); out += b; }
                else out += (char)c;
        }
    }
    return out;
}

// 戻り値: HTTPステータス。通信そのものに失敗したら -1
static long postJson(const std::string &url, const std::string &json) {
    if (!g_curlReady) return -1;
    CURL *c = curl_easy_init();
    if (!c) return -1;
    struct curl_slist *h = curl_slist_append(nullptr, "Content-Type: application/json");
    setCommon(c, url);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, json.c_str());
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)json.size());
    long code = -1;
    CURLcode rc = curl_easy_perform(c);
    if (rc == CURLE_OK) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    else applog("postJson: curl error %d (%s) %s", (int)rc, curl_easy_strerror(rc), g_errbuf);
    curl_slist_free_all(h);
    curl_easy_cleanup(c);
    return code;
}

// ファイル付きで送る。途中経過を画面に出し、Bで中止できる
static long postFile(const std::string &url, const std::string &payload, const std::string &path,
                     const char *attachName, const char *mime, long timeoutSec) {
    if (!g_curlReady) return -1;
    CURL *c = curl_easy_init();
    if (!c) return -1;
    setCommon(c, url);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, timeoutSec);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, xferCb);
    struct curl_slist *h = curl_slist_append(nullptr, "Expect:");
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);

    curl_mime *m = curl_mime_init(c);
    curl_mimepart *p = curl_mime_addpart(m);
    curl_mime_name(p, "payload_json");
    curl_mime_data(p, payload.c_str(), payload.size());
    curl_mime_type(p, "application/json");
    p = curl_mime_addpart(m);
    curl_mime_name(p, "files[0]");
    curl_mime_filedata(p, path.c_str());
    curl_mime_filename(p, attachName);
    curl_mime_type(p, mime);
    curl_easy_setopt(c, CURLOPT_MIMEPOST, m);

    long code = -1;
    CURLcode rc = curl_easy_perform(c);
    if (rc == CURLE_OK) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    else applog("postFile: curl error %d (%s) %s", (int)rc, curl_easy_strerror(rc), g_errbuf);
    applog("postFile: http %ld", code);
    curl_mime_free(m);
    curl_slist_free_all(h);
    curl_easy_cleanup(c);
    return code;
}

static std::string httpHint(long code) {
    switch (code) {
        case -1: return "通信できませんでした(Wi-Fiを確認してください)";
        case 401:
        case 404: return "Webhook URLが違うか、削除されています";
        case 413: return "Discordに大きすぎると断られました(上限を下げてください)";
        case 429: return "短時間に送りすぎです。少し待ってください";
        default: break;
    }
    char b[64];
    snprintf(b, sizeof(b), "http %ld", code);
    return b;
}

// ====================================================================
// Album(写真・動画の一覧)
// ====================================================================
enum { MK_NONE = 0, MK_IMAGE = 1, MK_VIDEO = 2 };

static int mediaKind(const std::string &n) {
    size_t dot = n.rfind('.');
    if (dot == std::string::npos) return MK_NONE;
    std::string e = n.substr(dot + 1);
    for (auto &c : e) c = (char)tolower((unsigned char)c);
    if (e == "jpg" || e == "jpeg") return MK_IMAGE;
    if (e == "mp4") return MK_VIDEO;
    return MK_NONE;
}

static void listDir(const std::string &path, std::vector<std::string> &dirs, std::vector<std::string> &files) {
    DIR *d = opendir(path.c_str());
    if (!d) return;
    while (dirent *e = readdir(d)) {
        std::string n = e->d_name;
        if (n == "." || n == "..") continue;
        if (e->d_type == DT_DIR) dirs.push_back(n);
        else files.push_back(n);
    }
    closedir(d);
}

static bool allDigits(const std::string &s) {
    if (s.empty()) return false;
    for (char c : s) if (c < '0' || c > '9') return false;
    return true;
}

static std::vector<std::string> digitDirsNewestFirst(const std::string &path) {
    std::vector<std::string> dirs, files, out;
    listDir(path, dirs, files);
    for (auto &d : dirs) if (allDigits(d)) out.push_back(d);
    std::sort(out.begin(), out.end(), [](const std::string &a, const std::string &b) { return a > b; });
    return out;
}

// 画面では日ごとに分けず、年 → 月 でまとめる(中のフォルダは日ごと)
struct Month {
    std::string name;                 // "11"
    std::vector<std::string> days;    // 日のフォルダ(新しい順)
    int images = 0, videos = 0;
};

struct Year {
    std::string name;                 // "2026"
    std::vector<Month> months;        // 新しい月が先
    int images = 0, videos = 0;
};

static void scanLibrary(std::vector<Year> &years) {
    years.clear();
    for (auto &y : digitDirsNewestFirst(g_cfg.album)) {
        std::string py = g_cfg.album + "/" + y;
        Year Y;
        Y.name = y;
        for (auto &m : digitDirsNewestFirst(py)) {
            std::string pm = py + "/" + m;
            Month M;
            M.name = m;
            for (auto &d : digitDirsNewestFirst(pm)) {
                std::string dayPath = pm + "/" + d;
                std::vector<std::string> dirs, files;
                listDir(dayPath, dirs, files);
                int im = 0, vi = 0;
                for (auto &f : files) {
                    int k = mediaKind(f);
                    if (k == MK_IMAGE) im++;
                    else if (k == MK_VIDEO) vi++;
                }
                if (im + vi > 0) {
                    M.days.push_back(dayPath);
                    M.images += im;
                    M.videos += vi;
                }
            }
            if (!M.days.empty()) {
                Y.images += M.images;
                Y.videos += M.videos;
                Y.months.push_back(M);
            }
        }
        if (!Y.months.empty()) years.push_back(Y);
    }
}

struct Item {
    std::string name, full;
    int kind = MK_NONE;
    long long size = -1;  // -1: まだ調べていない(たくさんあるときに、開くのが遅くならないよう、見えるときに調べる)
    bool sel = false;
    bool sent = false;
};

static long long itemSize(Item &it) {
    if (it.size < 0) {
        struct stat st;
        it.size = (stat(it.full.c_str(), &st) == 0) ? (long long)st.st_size : 0;
    }
    return it.size;
}

// 複数の日のフォルダから、写真・動画をまとめて集める(新しい順)
static void loadItems(const std::vector<std::string> &dayPaths, std::vector<Item> &items) {
    items.clear();
    for (auto &dp : dayPaths) {
        std::vector<std::string> dirs, files;
        listDir(dp, dirs, files);
        for (auto &f : files) {
            int k = mediaKind(f);
            if (k == MK_NONE) continue;
            Item it;
            it.name = f;
            it.full = dp + "/" + f;
            it.kind = k;
            it.sent = g_sent.count(f) > 0;
            items.push_back(it);
        }
    }
    std::sort(items.begin(), items.end(), [](const Item &a, const Item &b) { return a.name > b.name; });
}

// ファイル名(20261106185116...)から日時の文字を作る
static std::string timeLabel(const std::string &name, bool withDate) {
    if (name.size() < 14) return name;
    for (int i = 0; i < 14; i++) if (name[i] < '0' || name[i] > '9') return name;
    char b[64];
    if (withDate)
        snprintf(b, sizeof(b), "%.4s/%.2s/%.2s %.2s:%.2s", name.c_str(), name.c_str() + 4, name.c_str() + 6,
                 name.c_str() + 8, name.c_str() + 10);
    else
        snprintf(b, sizeof(b), "%.2s:%.2s:%.2s", name.c_str() + 8, name.c_str() + 10, name.c_str() + 12);
    return b;
}

static std::string fmtMB(long long bytes) {
    char b[32];
    snprintf(b, sizeof(b), "%.1fMB", bytes / 1048576.0);
    return b;
}

// ====================================================================
// 1ファイルを送る
// ====================================================================
enum { SR_OK = 0, SR_SKIP = 1, SR_FAIL = 2, SR_CANCEL = 3 };

static int sendOne(Item &it, std::string &msg) {
    bool isVideo = it.kind == MK_VIDEO;
    long long itSize = itemSize(it);
    std::string sendPath = it.full;
    std::string note;
    bool trimmed = false;
    long long limitBytes = (long long)g_cfg.limitMB * 1048576LL - 65536;

    if (itSize > limitBytes) {
        mp4trim::Info ti;
        mp4trim::Result tr = mp4trim::ERR_FORMAT;
        if (isVideo && g_cfg.trim) {
            g_view.status = "動画を削って、上限に収めています...";
            g_view.frac = 0;
            drawSending();
            applog("trim: 開始 %s (%lld bytes, 上限 %lld)", it.name.c_str(), itSize, limitBytes);
            tr = mp4trim::trim(it.full.c_str(), TRIM_TMP_PATH, limitBytes, &ti);
            applog("trim: result=%d new=%lld cut=%.2fs", (int)tr, (long long)ti.newSize, ti.cutSeconds);
        }
        if (tr == mp4trim::OK) {
            sendPath = TRIM_TMP_PATH;
            trimmed = true;
            char nb[200];
            snprintf(nb, sizeof(nb), "容量の都合で、最初の%.1f秒を削って送りました(%.1fMB→%.1fMB)", ti.cutSeconds,
                     ti.origSize / 1048576.0, ti.newSize / 1048576.0);
            note = nb;
        } else {
            remove(TRIM_TMP_PATH);
            msg = "上限(" + std::to_string(g_cfg.limitMB) + "MB)を超えるため送りませんでした(" + fmtMB(itSize) + ")";
            return SR_SKIP;
        }
    }

    std::string payload = std::string("{\"username\":\"") + HOOK_NAME + "\",\"embeds\":[{\"title\":\"" +
                          (isVideo ? "🎬 動画" : "📸 スクリーンショット") + "\"";
    std::string desc = "あとから送信(撮影 " + timeLabel(it.name, true) + ")";
    if (!note.empty()) desc += "\n" + note;
    payload += ",\"description\":\"" + jsonEscape(desc) + "\",\"color\":15277667";
    if (!isVideo) payload += ",\"image\":{\"url\":\"attachment://screenshot.jpg\"}";
    payload += "}]}";

    struct stat sst;
    long long sendSize = (stat(sendPath.c_str(), &sst) == 0) ? (long long)sst.st_size : itSize;
    long timeoutSec = 60 + (long)(sendSize / 1048576) * 8;

    long code = -1;
    for (int attempt = 1; attempt <= 3; attempt++) {
        g_view.status = std::string(isVideo ? "動画" : "写真") + "を送っています... (" + fmtMB(sendSize) + ")" +
                        (attempt > 1 ? " やり直し " + std::to_string(attempt) + "回目" : "");
        g_view.frac = 0;
        drawSending();
        code = postFile(g_cfg.hook, payload, sendPath, isVideo ? "video.mp4" : "screenshot.jpg",
                        isVideo ? "video/mp4" : "image/jpeg", timeoutSec);
        if (g_cancel || g_quit) break;
        if (code >= 200 && code < 300) break;
        if (code != -1) break;  // 通信エラー以外は、やり直しても同じ
    }
    if (trimmed) remove(TRIM_TMP_PATH);

    if (g_cancel || g_quit) { msg = "中止しました"; return SR_CANCEL; }
    if (code >= 200 && code < 300) {
        addSent(it.name);
        msg = note.empty() ? "送りました" : "送りました(最初を削りました)";
        return SR_OK;
    }
    msg = httpHint(code);
    return SR_FAIL;
}

// 選んだファイルを順に送る画面
static void runSend(std::vector<Item> &items, const std::vector<int> &idx) {
    g_view = SendView();
    g_cancel = false;
    int ok = 0, skip = 0, ng = 0;
    for (size_t n = 0; n < idx.size() && !g_quit; n++) {
        Item &it = items[idx[n]];
        g_view.title = "送信中 " + std::to_string(n + 1) + " / " + std::to_string(idx.size());
        g_view.file = it.name;
        g_view.frac = 0;
        std::string msg;
        int r = sendOne(it, msg);
        std::string line = timeLabel(it.name, false) + "  " + msg;
        g_view.results.push_back({line, r == SR_OK ? 0 : (r == SR_FAIL ? 2 : 1)});
        if (r == SR_OK) { ok++; it.sent = true; it.sel = false; }
        else if (r == SR_SKIP) skip++;
        else ng++;
        if (r == SR_CANCEL) break;
        if (r == SR_FAIL) break;  // 失敗したら、続けずに止める(同じ原因で失敗し続けるため)
    }
    // まとめ
    std::vector<std::string> lines;
    char b[200];
    snprintf(b, sizeof(b), "送れた: %d件  送れなかった: %d件  失敗/中止: %d件", ok, skip, ng);
    lines.push_back(b);
    size_t start = g_view.results.size() > 8 ? g_view.results.size() - 8 : 0;
    for (size_t i = start; i < g_view.results.size(); i++) lines.push_back(g_view.results[i].first);
    showMessage(ng == 0 ? "送信が終わりました" : "送信を止めました", lines, ng == 0);
}

// ====================================================================
// サムネイル(写真はjpegを縮小。動画は最初のコマを取り出す)
// ====================================================================
#define THUMB_W 256
#define THUMB_H 144

struct Thumb {
    SDL_Texture *t = nullptr;
    int w = 0, h = 0;
    bool failed = false;
};
static std::unordered_map<std::string, Thumb> g_thumbs;

// 縦横比を保って、maxW x maxH に収まる大きさを出す(拡大はしない)
static void fitSize(int sw, int sh, int maxW, int maxH, int &ow, int &oh) {
    if (sw <= 0 || sh <= 0) { ow = 1; oh = 1; return; }
    double sc = std::min((double)maxW / sw, (double)maxH / sh);
    if (sc > 1.0) sc = 1.0;
    ow = std::max(1, (int)(sw * sc));
    oh = std::max(1, (int)(sh * sc));
}

// RGBA(1画素4バイト)の画像を、ブロック平均で縮小する(dw<=sw, dh<=sh)
static void boxScaleRGBA(const unsigned char *src, int sw, int sh, int pitch, unsigned char *dst, int dw, int dh) {
    for (int dy = 0; dy < dh; dy++) {
        int y0 = (int)((long long)dy * sh / dh);
        int y1 = (int)((long long)(dy + 1) * sh / dh);
        if (y0 >= sh) y0 = sh - 1;
        if (y1 <= y0) y1 = y0 + 1;
        if (y1 > sh) y1 = sh;
        for (int dx = 0; dx < dw; dx++) {
            int x0 = (int)((long long)dx * sw / dw);
            int x1 = (int)((long long)(dx + 1) * sw / dw);
            if (x0 >= sw) x0 = sw - 1;
            if (x1 <= x0) x1 = x0 + 1;
            if (x1 > sw) x1 = sw;
            unsigned r = 0, g = 0, b = 0, n = 0;
            for (int y = y0; y < y1; y++) {
                const unsigned char *p = src + (size_t)y * pitch + (size_t)x0 * 4;
                for (int x = x0; x < x1; x++, p += 4) { r += p[0]; g += p[1]; b += p[2]; n++; }
            }
            unsigned char *q = dst + ((size_t)dy * dw + dx) * 4;
            q[0] = (unsigned char)(r / n);
            q[1] = (unsigned char)(g / n);
            q[2] = (unsigned char)(b / n);
            q[3] = 255;
        }
    }
}

static SDL_Texture *textureFromRGBA(const unsigned char *px, int w, int h, int pitch) {
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_RGBA32);
    if (!s) return nullptr;
    for (int y = 0; y < h; y++) memcpy((unsigned char *)s->pixels + (size_t)y * s->pitch, px + (size_t)y * pitch, (size_t)w * 4);
    SDL_Texture *t = SDL_CreateTextureFromSurface(g_ren, s);
    SDL_FreeSurface(s);
    return t;
}

static bool thumbFromSurface(SDL_Surface *s0, Thumb &out) {
    SDL_Surface *s = SDL_ConvertSurfaceFormat(s0, SDL_PIXELFORMAT_RGBA32, 0);
    if (!s) return false;
    if (s->w <= 0 || s->h <= 0) { SDL_FreeSurface(s); return false; }
    int tw, tH;
    fitSize(s->w, s->h, THUMB_W, THUMB_H, tw, tH);
    std::vector<unsigned char> buf((size_t)tw * tH * 4);
    boxScaleRGBA((const unsigned char *)s->pixels, s->w, s->h, s->pitch, buf.data(), tw, tH);
    SDL_FreeSurface(s);
    out.t = textureFromRGBA(buf.data(), tw, tH, tw * 4);
    out.w = tw;
    out.h = tH;
    return out.t != nullptr;
}

#ifdef NXSHOT_FFMPEG
// ---- 動画を読む(ffmpeg。音は使わず、映像だけ) ----
struct VDec {
    AVFormatContext *fmt = nullptr;
    AVCodecContext *ctx = nullptr;
    SwsContext *sws = nullptr;
    AVFrame *frame = nullptr;
    AVPacket *pkt = nullptr;
    int vs = -1;
    double tb = 0;

    static void errLog(const char *what, const char *path, int r) {
        char eb[128] = "";
        if (r < 0) av_strerror(r, eb, sizeof eb);
        applog("ffmpeg: %s %s (%d: %s)", what, path, r, eb);
    }

    bool open(const char *path, int threads) {
        // "sdmc:/..." のままだと ffmpeg が "sdmc" をプロトコル名と勘違いして開けないので、file: をつける
        std::string url = std::string("file:") + path;
        int r = avformat_open_input(&fmt, url.c_str(), nullptr, nullptr);
        if (r < 0) { fmt = nullptr; errLog("ファイルを開けません", path, r); return false; }
        r = avformat_find_stream_info(fmt, nullptr);
        if (r < 0) { errLog("動画の情報を読めません", path, r); return false; }
        vs = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (vs < 0) { errLog("映像がありません", path, vs); return false; }
        for (unsigned i = 0; i < fmt->nb_streams; i++)
            if ((int)i != vs) fmt->streams[i]->discard = AVDISCARD_ALL;  // 音声は読まない
        AVStream *st = fmt->streams[vs];
        const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
        if (!codec) { applog("ffmpeg: デコーダがありません codec_id=%d %s", (int)st->codecpar->codec_id, path); return false; }
        ctx = avcodec_alloc_context3(codec);
        if (!ctx) return false;
        r = avcodec_parameters_to_context(ctx, st->codecpar);
        if (r < 0) { errLog("パラメータ設定に失敗", path, r); return false; }
        if (threads > 1) { ctx->thread_count = threads; ctx->thread_type = FF_THREAD_FRAME; }
        r = avcodec_open2(ctx, codec, nullptr);
        if (r < 0) { errLog("デコーダを開けません", path, r); return false; }
        frame = av_frame_alloc();
        pkt = av_packet_alloc();
        if (!frame || !pkt) return false;
        tb = av_q2d(st->time_base);
        return ctx->width > 0 && ctx->height > 0;
    }

    // 次の1コマを frame に入れる。 1: 取れた  0: 最後まで読んだ  -1: 失敗
    int next() {
        for (;;) {
            int r = avcodec_receive_frame(ctx, frame);
            if (r == 0) return 1;
            if (r == AVERROR_EOF) return 0;
            if (r != AVERROR(EAGAIN)) return -1;
            if (av_read_frame(fmt, pkt) < 0) {
                avcodec_send_packet(ctx, nullptr);  // 残りを吐き出させる
                continue;
            }
            if (pkt->stream_index == vs) {
                int sr = avcodec_send_packet(ctx, pkt);
                av_packet_unref(pkt);
                if (sr < 0 && sr != AVERROR(EAGAIN)) return -1;
            } else {
                av_packet_unref(pkt);
            }
        }
    }

    double ptsSec() const {
        int64_t ts = frame->best_effort_timestamp;
        if (ts == AV_NOPTS_VALUE) return -1;
        return ts * tb;
    }

    bool toRGBA(int ow, int oh, std::vector<unsigned char> &buf) {
        sws = sws_getCachedContext(sws, frame->width, frame->height, (AVPixelFormat)frame->format, ow, oh,
                                   AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws) return false;
        buf.resize((size_t)ow * oh * 4);
        uint8_t *dst[4] = {buf.data(), nullptr, nullptr, nullptr};
        int dstStride[4] = {ow * 4, 0, 0, 0};
        sws_scale(sws, frame->data, frame->linesize, 0, frame->height, dst, dstStride);
        return true;
    }

    void rewind() {
        av_seek_frame(fmt, vs, 0, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(ctx);
    }

    void close() {
        if (sws) { sws_freeContext(sws); sws = nullptr; }
        if (frame) av_frame_free(&frame);
        if (pkt) av_packet_free(&pkt);
        if (ctx) avcodec_free_context(&ctx);
        if (fmt) avformat_close_input(&fmt);
    }
};

static void loadThumbVideo(const std::string &path, Thumb &out) {
    VDec d;
    if (d.open(path.c_str(), 1) && d.next() == 1) {
        int tw, tH;
        fitSize(d.frame->width, d.frame->height, THUMB_W, THUMB_H, tw, tH);
        std::vector<unsigned char> buf;
        if (d.toRGBA(tw, tH, buf)) {
            out.t = textureFromRGBA(buf.data(), tw, tH, tw * 4);
            out.w = tw;
            out.h = tH;
        }
    }
    d.close();
    if (!out.t) { out.failed = true; applog("thumb: 動画の最初のコマを取れません %s", path.c_str()); }
}
#endif

static void loadThumbImage(const std::string &path, Thumb &out) {
    SDL_Surface *s = IMG_Load(path.c_str());
    if (!s) { out.failed = true; applog("thumb: jpgを読めません %s (%s)", path.c_str(), IMG_GetError()); return; }
    if (!thumbFromSurface(s, out)) out.failed = true;
    SDL_FreeSurface(s);
}

static void thumbLoad(const Item &it) {
    Thumb th;
    if (it.kind == MK_IMAGE) {
        loadThumbImage(it.full, th);
    } else {
#ifdef NXSHOT_FFMPEG
        loadThumbVideo(it.full, th);
#else
        th.failed = true;  // この版には動画を読む部品が入っていない
#endif
    }
    g_thumbs[it.full] = th;
}

// たくさんたまったら、いま見ているあたり以外を捨てる(メモリを使いすぎないように)
static void thumbEvict(const std::vector<Item> &items, int from, int to) {
    if (g_thumbs.size() <= 160) return;
    std::set<std::string> keep;
    for (int i = std::max(0, from); i < to && i < (int)items.size(); i++) keep.insert(items[i].full);
    for (auto it = g_thumbs.begin(); it != g_thumbs.end();) {
        if (keep.count(it->first)) { ++it; continue; }
        if (it->second.t) SDL_DestroyTexture(it->second.t);
        it = g_thumbs.erase(it);
    }
}

// ====================================================================
// 画面: 写真・動画の一覧(サムネ付きの格子)
// ====================================================================
#define GRID_COLS 5
#define GRID_ROWS 3
#define TILE_W 216
#define TILE_H 122
#define GRID_X0 44
#define GRID_Y0 100
#define GRID_PX 244
#define GRID_PY 172

static void fillCircle(int cx, int cy, int r, SDL_Color c) {
    SDL_SetRenderDrawColor(g_ren, c.r, c.g, c.b, c.a);
    for (int dy = -r; dy <= r; dy++) {
        int dx = (int)std::sqrt((double)(r * r - dy * dy));
        SDL_RenderDrawLine(g_ren, cx - dx, cy + dy, cx + dx, cy + dy);
    }
}

// 右向きの三角(再生マーク)
static void fillPlayMark(int cx, int cy, int h, SDL_Color c) {
    SDL_SetRenderDrawColor(g_ren, c.r, c.g, c.b, c.a);
    int w = h * 7 / 8;
    int x0 = cx - w / 3;
    for (int i = 0; i < w; i++) {
        int hh = (h / 2) * (w - i) / w;
        SDL_RenderDrawLine(g_ren, x0 + i, cy - hh, x0 + i, cy + hh);
    }
}

static std::string shortTime(const std::string &name) {
    if (name.size() < 14) return name;
    for (int i = 0; i < 12; i++) if (name[i] < '0' || name[i] > '9') return name;
    char b[32];
    snprintf(b, sizeof(b), "%.2s/%.2s %.2s:%.2s", name.c_str() + 4, name.c_str() + 6, name.c_str() + 8, name.c_str() + 10);
    return b;
}

static bool overLimit(Item &it) { return itemSize(it) > (long long)g_cfg.limitMB * 1048576LL - 65536; }

static void drawTile(Item &it, int x, int y, bool cursor) {
    // 札(カード)の上にサムネをのせる
    fillRRect(x - 8, y - 8, TILE_W + 16, TILE_H + 40, 10, cursor ? C_CARD2 : SDL_Color{30, 32, 46, 225});
    fillRect(x, y, TILE_W, TILE_H, SDL_Color{12, 13, 20, 255});
    auto f = g_thumbs.find(it.full);
    bool have = f != g_thumbs.end() && f->second.t;
    if (have) {
        int dw, dh;
        fitSize(f->second.w, f->second.h, TILE_W, TILE_H, dw, dh);
        SDL_Rect dst = {x + (TILE_W - dw) / 2, y + (TILE_H - dh) / 2, dw, dh};
        SDL_RenderCopy(g_ren, f->second.t, nullptr, &dst);
    } else if (f != g_thumbs.end() && f->second.failed) {
        drawTextMid(x + TILE_W / 2, y + TILE_H / 2, "表示できません", fSmall, C_DIM, 2);
    } else {
        drawTextMid(x + TILE_W / 2, y + TILE_H / 2, "読み込み中...", fSmall, C_DIM, 2);
    }
    if (it.kind == MK_VIDEO) {
        if (have) {
            fillCircle(x + TILE_W / 2, y + TILE_H / 2, 22, SDL_Color{0, 0, 0, 150});
            fillPlayMark(x + TILE_W / 2, y + TILE_H / 2, 24, SDL_Color{255, 255, 255, 255});
        }
        fillRRect(x + 6, y + TILE_H - 28, 48, 22, 6, SDL_Color{0, 0, 0, 190});
        drawTextMid(x + 30, y + TILE_H - 17, "動画", fSmall, C_WARN, 2);
    }
    // チェック
    fillRRect(x + 6, y + 6, 24, 24, 6, SDL_Color{0, 0, 0, 160});
    strokeRRect(x + 6, y + 6, 24, 24, 6, 2, C_WHITE);
    if (it.sel) fillRRect(x + 11, y + 11, 14, 14, 4, C_OK);
    if (it.sent) {
        fillRRect(x + TILE_W - 62, y + 6, 56, 22, 6, SDL_Color{0, 0, 0, 190});
        drawTextMid(x + TILE_W - 34, y + 17, "送信済", fSmall, C_OK, 2);
    }
    if (cursor) strokeRRect(x - 10, y - 10, TILE_W + 20, TILE_H + 44, 12, 3, C_ACC2);
    // 下の文字: 日時と大きさ。上限を超えるものは色を変える
    int ly = y + TILE_H + 14;
    drawTextMid(x, ly, shortTime(it.name), fSmall, C_TEXT);
    SDL_Color sc = C_DIM;
    if (overLimit(it)) sc = (it.kind == MK_VIDEO && g_cfg.trim) ? C_WARN : C_NG;
    drawTextMid(x + TILE_W, ly, fmtMB(itemSize(it)), fSmall, sc, 1);
}

static void clearScreen() {
    drawBackground();
}

static void previewInfoBar(Item &it) {
    std::string d = timeLabel(it.name, true) + "   " + (it.kind == MK_VIDEO ? "動画" : "写真") + "   " + fmtMB(itemSize(it));
    if (it.sel) d += "   [チェック済み]";
    drawText(48, 618, fitText(fSmall, d, 1180), fSmall, it.sel ? C_OK : C_TEXT);
}

// 写真を大きく見る
static void previewPhoto(Item &it) {
    SDL_Surface *s = IMG_Load(it.full.c_str());
    int sw = s ? s->w : 0, sh = s ? s->h : 0;
    SDL_Texture *tex = s ? SDL_CreateTextureFromSurface(g_ren, s) : nullptr;
    if (s) SDL_FreeSurface(s);
    if (!tex) { showMessage("写真を開けませんでした", {it.name}, false); return; }
    while (!g_quit && appletMainLoop()) {
        updateInput();
        clearScreen();
        drawHeader("写真");
        int dw, dh;
        fitSize(sw, sh, 1200, 508, dw, dh);
        SDL_Rect dst = {40 + (1200 - dw) / 2, 100 + (508 - dh) / 2, dw, dh};
        SDL_RenderCopy(g_ren, tex, nullptr, &dst);
        previewInfoBar(it);
        drawFooter("A: チェックを付ける/外す   B: 戻る");
        endFrame();
        if (pressed(BT_B)) break;
        if (pressed(BT_A)) it.sel = !it.sel;
        SDL_Delay(8);
    }
    SDL_DestroyTexture(tex);
}

// 動画を再生して見る(音は出ません。最後まで行くとくり返します)
static void previewVideo(Item &it) {
#ifdef NXSHOT_FFMPEG
    VDec d;
    if (!d.open(it.full.c_str(), 3)) {
        d.close();
        showMessage("動画を開けませんでした", {it.name, "対応していない形式かもしれません"}, false);
        return;
    }
    int ow, oh;
    fitSize(d.ctx->width, d.ctx->height, 1200, 508, ow, oh);
    ow &= ~1;
    oh &= ~1;
    if (ow < 2) ow = 2;
    if (oh < 2) oh = 2;
    SDL_Texture *tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, ow, oh);
    if (!tex) { d.close(); showMessage("動画を表示できませんでした", {it.name}, false); return; }
    std::vector<unsigned char> buf;
    bool first = true;
    Uint32 t0 = 0;
    double base = 0;
    long frameNo = 0;
    int errors = 0;
    bool haveFrame = false;
    while (!g_quit && appletMainLoop()) {
        updateInput();
        if (pressed(BT_B)) break;
        if (pressed(BT_A)) it.sel = !it.sel;

        int r = d.next();
        if (r == 0) {  // 最後まで来たので、最初に戻る
            d.rewind();
            first = true;
            continue;
        }
        if (r < 0) {
            if (++errors > 5) break;
            d.rewind();
            first = true;
            continue;
        }
        double pts = d.ptsSec();
        if (pts < 0) pts = frameNo / 30.0;
        frameNo++;
        if (first) { first = false; t0 = SDL_GetTicks(); base = pts; frameNo = 0; }
        double now = (SDL_GetTicks() - t0) / 1000.0;
        double due = pts - base;
        if (haveFrame && due < now - 0.08) continue;  // 間に合わないコマは飛ばす(時間どおりに進める)
        if (due > now) {
            double wait = std::min(0.1, due - now);
            SDL_Delay((Uint32)(wait * 1000));
        }
        if (!d.toRGBA(ow, oh, buf)) break;
        SDL_UpdateTexture(tex, nullptr, buf.data(), ow * 4);
        haveFrame = true;
        clearScreen();
        drawHeader("動画");
        SDL_Rect dst = {40 + (1200 - ow) / 2, 100 + (508 - oh) / 2, ow, oh};
        SDL_RenderCopy(g_ren, tex, nullptr, &dst);
        previewInfoBar(it);
        drawFooter("A: チェックを付ける/外す   B: 戻る   (音は出ません)");
        endFrame();
    }
    SDL_DestroyTexture(tex);
    d.close();
#else
    (void)it;
    showMessage("動画のプレビューは、この版には入っていません", {"ビルドに switch-ffmpeg が必要です"}, false);
#endif
}

static void screenPreview(Item &it) {
    if (it.kind == MK_VIDEO) previewVideo(it);
    else previewPhoto(it);
}

static void screenGrid(const std::string &title, const std::vector<std::string> &dayPaths) {
    beginFrame();
    drawHeader(title);
    drawText(80, 200, "読み込んでいます...", fBody, C_TEXT);
    endFrame();
    std::vector<Item> items;
    loadItems(dayPaths, items);
    applog("grid: %s %zu件", title.c_str(), items.size());
    const int PAGE = GRID_COLS * GRID_ROWS;
    int cur = 0, topRow = 0;
    while (!g_quit && appletMainLoop()) {
        beginFrame();
        int n = (int)items.size();
        int selCount = 0;
        for (auto &it : items) if (it.sel) selCount++;
        drawHeader(title + "   " + std::to_string(n) + "件");
        if (n == 0) drawText(80, 200, "写真・動画はありません", fBody, C_DIM);

        int first = topRow * GRID_COLS;
        std::vector<int> missing;
        for (int k = 0; k < PAGE; k++) {
            int i = first + k;
            if (i >= n) break;
            int x = GRID_X0 + (k % GRID_COLS) * GRID_PX;
            int y = GRID_Y0 + (k / GRID_COLS) * GRID_PY;
            drawTile(items[i], x, y, i == cur);
            if (!g_thumbs.count(items[i].full)) {
                if (i == cur) missing.insert(missing.begin(), i);  // いま選んでいるものを先に
                else missing.push_back(i);
            }
        }
        if (n > 0) {
            Item &c = items[cur];
            long long sz = itemSize(c);
            std::string d = timeLabel(c.name, true) + "   " + (c.kind == MK_VIDEO ? "動画" : "写真") + "   " + fmtMB(sz);
            SDL_Color dc = C_TEXT;
            if (overLimit(c)) {
                if (c.kind == MK_VIDEO && g_cfg.trim) { d += "   → 最初を削って送ります"; dc = C_WARN; }
                else { d += "   → 上限を超えるので送れません"; dc = C_NG; }
            }
            drawText(48, 618, fitText(fSmall, d, 800), fSmall, dc);
            char rb[120];
            snprintf(rb, sizeof(rb), "選んだ数: %d   上限: %ldMB", selCount, g_cfg.limitMB);
            drawText(1232, 618, rb, fSmall, C_DIM, 1);
        }
        drawFooter("十字: 動く  A: チェック  ZR: 大きく見る/動画を再生  Y: 全部  X: 送る  B: 戻る");
        endFrame();

        if (pressed(BT_B)) return;
        if (n > 0) {
            if (pressed(BT_RIGHT)) cur = std::min(n - 1, cur + 1);
            if (pressed(BT_LEFT)) cur = std::max(0, cur - 1);
            if (pressed(BT_DOWN) && cur / GRID_COLS < (n - 1) / GRID_COLS) cur = std::min(n - 1, cur + GRID_COLS);
            if (pressed(BT_UP)) cur = std::max(0, cur - GRID_COLS);
            if (pressed(BT_R)) cur = std::min(n - 1, cur + PAGE);
            if (pressed(BT_L)) cur = std::max(0, cur - PAGE);
            int row = cur / GRID_COLS;
            if (row < topRow) topRow = row;
            if (row >= topRow + GRID_ROWS) topRow = row - GRID_ROWS + 1;
            if (pressed(BT_A)) items[cur].sel = !items[cur].sel;
            if (pressed(BT_ZR)) screenPreview(items[cur]);
            if (pressed(BT_Y)) {
                bool any = false;
                for (auto &it : items) if (!it.sel) { any = true; break; }
                for (auto &it : items) it.sel = any;
            }
            if (pressed(BT_X)) {
                std::vector<int> idx;
                for (size_t i = 0; i < items.size(); i++) if (items[i].sel) idx.push_back((int)i);
                if (idx.empty()) idx.push_back(cur);  // 何も選んでいなければ、いま選んでいる1件
                if (!hookOk()) {
                    showMessage("Webhook URLが未設定です", {"設定画面で、Webhook URLを入れてください"}, false);
                } else if (!networkUp()) {
                    showMessage("Wi-Fiにつながっていません", {"つながってから、もう一度送ってください"}, false);
                } else {
                    runSend(items, idx);
                }
            }
        }

        // 見えているもののサムネを、少しずつ作る(1回の描画で長く止まらないよう、30msまで)
        Uint32 t0 = SDL_GetTicks();
        for (int i : missing) {
            thumbLoad(items[i]);
            if (SDL_GetTicks() - t0 > 30) break;
        }
        thumbEvict(items, first - PAGE * 2, first + PAGE * 3);
    }
}

// ====================================================================
// 画面: 年 → 月 を選ぶ
// ====================================================================
struct ListRow { std::string left, right; };

// A: 選んだ番号を返す  B: -1
static int pickList(const std::string &sub, const std::vector<ListRow> &rows, int &cur, const char *hint) {
    const int ROWS = 10, PITCH = 54, RH = 48, Y0 = 92;
    int n = (int)rows.size();
    if (cur >= n) cur = 0;
    int top = cur >= ROWS ? cur - ROWS + 1 : 0;
    while (!g_quit && appletMainLoop()) {
        beginFrame();
        drawHeader(sub);
        for (int r = 0; r < ROWS; r++) {
            int i = top + r;
            if (i >= n) break;
            int y = Y0 + r * PITCH;
            bool sel = i == cur;
            if (sel) {
                fillRRect(36, y, 1196, RH, 10, C_ACC);
                strokeRRect(36, y, 1196, RH, 10, 2, C_ACC2);
            } else {
                fillRRect(36, y, 1196, RH, 10, SDL_Color{34, 37, 54, 200});
            }
            drawTextMid(64, y + RH / 2, rows[i].left, fBody, sel ? C_WHITE : C_TEXT);
            drawTextMid(1206, y + RH / 2, rows[i].right, fBody, sel ? C_WHITE : C_DIM, 1);
        }
        if (n > ROWS) {  // 右端の、どのあたりを見ているかの目印
            int track = ROWS * PITCH - 6;
            int th = std::max(30, track * ROWS / n);
            int ty = Y0 + (track - th) * top / (n - ROWS);
            fillRRect(1244, Y0, 6, track, 3, SDL_Color{255, 255, 255, 25});
            fillRRect(1244, ty, 6, th, 3, C_ACC2);
        }
        drawFooter(hint);
        endFrame();

        if (pressed(BT_B)) return -1;
        if (n > 0) {
            if (pressed(BT_DOWN)) { if (cur < n - 1) cur++; }
            if (pressed(BT_UP)) { if (cur > 0) cur--; }
            if (pressed(BT_R)) cur = std::min(n - 1, cur + ROWS);
            if (pressed(BT_L)) cur = std::max(0, cur - ROWS);
            if (cur < top) top = cur;
            if (cur >= top + ROWS) top = cur - ROWS + 1;
            if (pressed(BT_A)) return cur;
        }
        SDL_Delay(16);
    }
    return -1;
}

static std::string countText(int images, int videos) {
    char b[80];
    snprintf(b, sizeof(b), "写真 %d  動画 %d", images, videos);
    return b;
}

static void screenMonths(const Year &y) {
    std::vector<ListRow> rows;
    rows.push_back({y.name + "年 ぜんぶ", countText(y.images, y.videos)});
    for (auto &m : y.months) rows.push_back({std::to_string(atoi(m.name.c_str())) + "月", countText(m.images, m.videos)});
    int cur = 0;
    while (!g_quit) {
        int i = pickList(y.name + "年  月を選ぶ", rows, cur, "上下: 選ぶ   A: 開く   B: 戻る");
        if (i < 0) return;
        if (i == 0) {
            std::vector<std::string> all;
            for (auto &m : y.months) all.insert(all.end(), m.days.begin(), m.days.end());
            screenGrid(y.name + "年(ぜんぶ)", all);
        } else {
            const Month &m = y.months[i - 1];
            screenGrid(y.name + "年" + std::to_string(atoi(m.name.c_str())) + "月", m.days);
        }
    }
}

static void screenYears() {
    beginFrame();
    drawHeader("写真・動画を送る");
    drawText(80, 200, "Albumを読み込んでいます...", fBody, C_TEXT);
    endFrame();
    std::vector<Year> years;
    scanLibrary(years);
    applog("years: %zu (album=%s)", years.size(), g_cfg.album.c_str());
    if (years.empty()) {
        showMessage("写真・動画が見つかりません", {"保存先が「SDカード」か確認してください", "Albumの場所: " + g_cfg.album}, false);
        return;
    }
    std::vector<ListRow> rows;
    for (auto &y : years) rows.push_back({y.name + "年", countText(y.images, y.videos)});
    int cur = 0;
    while (!g_quit) {
        int i = pickList("年を選ぶ", rows, cur, "上下: 選ぶ   A: 開く   B: 戻る");
        if (i < 0) return;
        screenMonths(years[i]);
    }
}

// ====================================================================
// 画面: 設定
// ====================================================================
static void screenSettings() {
    int cur = 0;
    std::string toast;
    Uint32 toastUntil = 0;
    auto changed = [&]() {
        bool ok = saveConfig();
        toast = ok ? "保存しました" : "保存できませんでした";
        toastUntil = SDL_GetTicks() + 1800;
    };
    const int N = 7;
    while (!g_quit && appletMainLoop()) {
        beginFrame();
        drawHeader("設定");
        const char *names[N] = {"Webhook URL", "写真・動画の自動送信", "送れるファイルの上限", "大きい動画を削って送る",
                                "Albumの場所", "起動時にアップデートを確認", "戻る"};
        std::string vals[N];
        vals[0] = maskHook(g_cfg.hook);
        vals[1] = g_cfg.autoSend ? "オン" : "オフ";
        vals[2] = std::to_string(g_cfg.limitMB) + " MB";
        vals[3] = g_cfg.trim ? "する" : "しない";
        vals[4] = g_cfg.album == DEFAULT_ALBUM ? "標準(SDカードのAlbum)" : g_cfg.album;
        vals[5] = g_cfg.updCheck ? "する" : "しない";
        vals[6] = "";
        for (int i = 0; i < N; i++) {
            int y = 92 + i * 58;
            bool sel = i == cur;
            if (sel) {
                fillRRect(36, y, 1208, 50, 10, C_ACC);
                strokeRRect(36, y, 1208, 50, 10, 2, C_ACC2);
            } else {
                fillRRect(36, y, 1208, 50, 10, SDL_Color{34, 37, 54, 200});
            }
            drawTextMid(64, y + 25, names[i], fBody, sel ? C_WHITE : C_TEXT);
            SDL_Color vc = sel ? C_WHITE : C_DIM;
            if (i == 0 && !hookOk()) vc = C_WARN;
            if (i == 1) vc = g_cfg.autoSend ? C_OK : C_WARN;
            if (!vals[i].empty()) drawTextMid(1216, y + 25, fitText(fBody, vals[i], 700), fBody, vc, 1);
        }
        std::string hint;
        if (cur == 0) hint = "A: 入力(長いURLは、PCで config.txt を書き換えるほうが楽です)";
        else if (cur == 1) hint = "A: オン/オフ を切り替え(オフの間は、撮ったものを自動では送りません。ゲームの開始・終了の通知は続きます)";
        else if (cur == 2) hint = "左右: 1MBずつ  L/R: 10MBずつ  A: 数字で入力  (ブーストなしのサーバーは約10MB)";
        else if (cur == 3) hint = "A: する/しない を切り替え(しない: 大きい動画は送りません)";
        else if (cur == 4) hint = "A: 入力(空にすると標準に戻る)。emuMMCのときだけ変えます";
        else if (cur == 5) hint = "A: する/しない を切り替え(する: アプリを開いたとき、新しい版がないかGitHubで確かめます)";
        else hint = "A または B: 戻る";
        drawText(48, 508, fitText(fSmall, hint, 1180), fSmall, C_TEXT);
        drawText(48, 540, "変えた内容は、すぐ保存されます。常駐版は5秒ごとに読み直します", fSmall, C_DIM);
        if (SDL_GetTicks() < toastUntil) drawText(1244, 540, toast, fSmall, C_OK, 1);
        drawFooter("上下: 選ぶ  A: 変える  B: 戻る");
        endFrame();

        if (pressed(BT_B)) return;
        if (pressed(BT_DOWN)) cur = (cur + 1) % N;
        if (pressed(BT_UP)) cur = (cur + N - 1) % N;
        if (cur == 2) {
            if (pressed(BT_RIGHT) && g_cfg.limitMB < 500) { g_cfg.limitMB++; changed(); }
            if (pressed(BT_LEFT) && g_cfg.limitMB > 1) { g_cfg.limitMB--; changed(); }
            if (pressed(BT_R)) { g_cfg.limitMB = std::min(500L, g_cfg.limitMB + 10); changed(); }
            if (pressed(BT_L)) { g_cfg.limitMB = std::max(1L, g_cfg.limitMB - 10); changed(); }
        }
        if (cur == 1 && (pressed(BT_RIGHT) || pressed(BT_LEFT))) { g_cfg.autoSend = !g_cfg.autoSend; changed(); }
        if (pressed(BT_A)) {
            std::string out;
            if (cur == 0) {
                if (kbdInput("Webhook URL", "https://discord.com/api/webhooks/...", g_cfg.hook, 300, false, out)) {
                    if (out.compare(0, 8, "https://") == 0) { g_cfg.hook = out; changed(); }
                    else showMessage("URLが正しくありません", {"https:// で始まるURLを入れてください"}, false);
                }
            } else if (cur == 1) {
                g_cfg.autoSend = !g_cfg.autoSend;
                changed();
            } else if (cur == 2) {
                if (kbdInput("上限(MB)", "1〜500", std::to_string(g_cfg.limitMB), 3, true, out)) {
                    long v = atol(out.c_str());
                    if (v >= 1 && v <= 500) { g_cfg.limitMB = v; changed(); }
                    else showMessage("数が正しくありません", {"1から500までの数を入れてください"}, false);
                }
            } else if (cur == 3) {
                g_cfg.trim = !g_cfg.trim;
                changed();
            } else if (cur == 4) {
                std::string init = g_cfg.album == DEFAULT_ALBUM ? "" : g_cfg.album;
                if (kbdInput("Albumの場所", "sdmc:/Nintendo/Album", init, 200, false, out)) {
                    while (out.size() > 1 && out.back() == '/') out.pop_back();
                    g_cfg.album = out.empty() ? std::string(DEFAULT_ALBUM) : out;
                    changed();
                }
            } else if (cur == 5) {
                g_cfg.updCheck = !g_cfg.updCheck;
                changed();
            } else {
                return;
            }
        }
        if (cur == 5 && (pressed(BT_RIGHT) || pressed(BT_LEFT))) { g_cfg.updCheck = !g_cfg.updCheck; changed(); }
        SDL_Delay(16);
    }
}

// ====================================================================
// 画面: 常駐版の状態とログ
// ====================================================================
static void screenStatus() {
    std::vector<std::string> lines;
    std::string ver = "不明";
    FILE *f = fopen(SYS_LOG_PATH, "r");
    bool hasLog = f != nullptr;
    if (f) {
        char line[400];
        while (fgets(line, sizeof(line), f)) {
            std::string t = trimLine(line);
            if (t.empty()) continue;
            lines.push_back(t);
            size_t p = t.find("main: start v");
            if (p != std::string::npos) ver = t.substr(p + 13);
        }
        fclose(f);
    }
    u64 pid = 0;
    Result rc = pmdmntGetProcessId(&pid, SYS_TITLE_ID);
    std::string running = R_SUCCEEDED(rc) && pid != 0 ? "動いています" : (R_SUCCEEDED(rc) ? "止まっています" : "確認できませんでした");

    const int LOGROWS = 17;
    int top = (int)lines.size() > LOGROWS ? (int)lines.size() - LOGROWS : 0;
    while (!g_quit && appletMainLoop()) {
        beginFrame();
        drawHeader("常駐版の状態");
        bool up = networkUp();
        struct Chip { const char *label; std::string value; SDL_Color c; };
        Chip chips[4] = {
            {"常駐版", running, running == "動いています" ? C_OK : C_WARN},
            {"常駐版の版", ver, C_TEXT},
            {"Wi-Fi", up ? "つながっています" : "つながっていません", up ? C_OK : C_WARN},
            {"Webhook", hookOk() ? "設定済み" : "未設定", hookOk() ? C_OK : C_WARN},
        };
        for (int i = 0; i < 4; i++) {
            int x = 36 + i * 306;
            drawCard(x, 90, 296, 68);
            drawText(x + 18, 96, chips[i].label, fSmall, C_DIM);
            drawText(x + 18, 120, fitText(fBody, chips[i].value, 262), fBody, chips[i].c);
        }
        drawCard(36, 170, 1208, 482);
        if (!hasLog) drawText(60, 190, "常駐版のログ(log.txt)がありません。常駐版が、まだ動いていないかもしれません。", fSmall, C_WARN);
        for (int r = 0; r < LOGROWS; r++) {
            int i = top + r;
            if (i >= (int)lines.size()) break;
            drawText(60, 184 + r * 26, fitText(fSmall, lines[i], 1160), fSmall, C_TEXT);
        }
        drawFooter("上下: ログをめくる  B: 戻る");
        endFrame();

        if (pressed(BT_B)) return;
        if (pressed(BT_UP) && top > 0) top--;
        if (pressed(BT_DOWN) && top + LOGROWS < (int)lines.size()) top++;
        SDL_Delay(16);
    }
}

// ====================================================================
// テスト送信
// ====================================================================
static void doTestSend() {
    if (!hookOk()) {
        showMessage("Webhook URLが未設定です", {"設定画面で、Webhook URLを入れてください"}, false);
        return;
    }
    if (!networkUp()) {
        showMessage("Wi-Fiにつながっていません", {"つながってから、もう一度試してください"}, false);
        return;
    }
    beginFrame();
    drawHeader("テスト送信");
    drawText(80, 200, "送っています...", fBody, C_TEXT);
    endFrame();
    std::string json = std::string("{\"username\":\"") + HOOK_NAME +
                       "\",\"embeds\":[{\"title\":\"🟢 NXShotアプリからのテスト\",\"description\":\"このメッセージが届けば、Webhookは正しく動いています。\",\"color\":5763719}]}";
    long code = postJson(g_cfg.hook, json);
    if (code >= 200 && code < 300) showMessage("送れました", {"Discordのチャンネルを見てください(http " + std::to_string(code) + ")"}, true);
    else showMessage("送れませんでした", {httpHint(code)}, false);
}

// ====================================================================
// アップデート(GitHubのReleasesから、アプリ本体と常駐版を更新する)
// ====================================================================
struct UpdInfo {
    bool checked = false;  // 確認した
    bool ok = false;       // 確認に成功した
    bool newer = false;    // 新しい版がある
    bool hasNro = false;   // Releaseにアプリ本体が付いている
    bool hasNsp = false;   // Releaseに常駐版が付いている
    std::string tag;       // 例: v1.2.2
    std::string err;
};
static UpdInfo g_upd;
static std::string g_selfPath = SELF_DEFAULT_PATH;  // 動いているNXShot.nroの場所

static bool fileExists(const std::string &path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

// ホームブリューメニューが教えてくれる起動パス(argv[0])から、自分のnroの場所を決める
static void initSelfPath(int argc, char **argv) {
    if (argc < 1 || !argv || !argv[0]) return;
    std::string a = argv[0];
    if (a.size() >= 2 && a.front() == '"' && a.back() == '"') a = a.substr(1, a.size() - 2);
    if (!a.empty() && a[0] == '/') a = "sdmc:" + a;
    if (a.compare(0, 6, "sdmc:/") != 0 || a.size() < 10) return;
    std::string ext = a.substr(a.size() - 4);
    for (auto &ch : ext) ch = (char)tolower((unsigned char)ch);
    if (ext != ".nro" || !fileExists(a)) return;
    g_selfPath = a;
}

static size_t strWriteCb(char *p, size_t sz, size_t n, void *u) {
    std::string *o = (std::string *)u;
    if (o->size() + sz * n > (size_t)(1 << 20)) return 0;
    o->append(p, sz * n);
    return sz * n;
}

// 文字として取る。戻り値: HTTPの番号 / 通信できなければ -1 (理由は err)
static long httpGetString(const std::string &url, std::string &out, long timeoutSec, std::string &err) {
    CURL *c = curl_easy_init();
    if (!c) { err = "通信の準備に失敗しました"; return -1; }
    setCommon(c, url);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, strWriteCb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &out);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 6L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, timeoutSec);
    struct curl_slist *h = curl_slist_append(nullptr, "Accept: application/vnd.github+json");
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    CURLcode rc = curl_easy_perform(c);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(h);
    curl_easy_cleanup(c);
    if (rc != CURLE_OK) {
        err = std::string("通信に失敗しました(") + curl_easy_strerror(rc) + ")";
        applog("update: curl error %d (%s) %s", (int)rc, curl_easy_strerror(rc), g_errbuf);
        return -1;
    }
    return code;
}

// JSONから "key": "文字" の文字を取り出す(最初に出てくるもの)
static std::string jsonStrField(const std::string &j, const char *key) {
    std::string k = std::string("\"") + key + "\"";
    size_t p = j.find(k);
    if (p == std::string::npos) return "";
    p = j.find(':', p + k.size());
    if (p == std::string::npos) return "";
    p = j.find('"', p);
    if (p == std::string::npos) return "";
    size_t e = j.find('"', p + 1);
    if (e == std::string::npos) return "";
    return j.substr(p + 1, e - p - 1);
}

static bool parseVer(const std::string &s, int v[3]) {
    const char *p = s.c_str();
    if (*p == 'v' || *p == 'V') p++;
    v[0] = v[1] = v[2] = 0;
    int n = 0;
    while (*p >= '0' && *p <= '9' && n < 3) {
        v[n++] = atoi(p);
        while (*p >= '0' && *p <= '9') p++;
        if (*p == '.') p++;
        else break;
    }
    return n > 0;
}

// a が b より新しい版か
static bool verNewer(const std::string &a, const std::string &b) {
    int x[3], y[3];
    if (!parseVer(a, x) || !parseVer(b, y)) return false;
    for (int i = 0; i < 3; i++) {
        if (x[i] != y[i]) return x[i] > y[i];
    }
    return false;
}

// GitHubで最新のReleaseを調べる。結果は g_upd に入る
static bool updCheck(long timeoutSec) {
    g_upd = UpdInfo();
    g_upd.checked = true;
    if (!g_curlReady) { g_upd.err = "通信の準備ができていません"; return false; }
    std::string body, err;
    long code = httpGetString(std::string("https://api.github.com/repos/") + UPDATE_REPO + "/releases/latest", body,
                              timeoutSec, err);
    if (code < 0) { g_upd.err = err; return false; }
    if (code == 404) { g_upd.err = "公開されたリリースが見つかりませんでした"; return false; }
    if (code == 403) { g_upd.err = "GitHubの回数制限に当たりました。しばらくしてからもう一度試してください"; return false; }
    if (code != 200) { g_upd.err = "GitHubが http " + std::to_string(code) + " を返しました"; return false; }
    g_upd.tag = jsonStrField(body, "tag_name");
    if (g_upd.tag.empty()) { g_upd.err = "版の情報を読めませんでした"; return false; }
    g_upd.hasNro = body.find(std::string("/") + UPDATE_NRO_NAME + "\"") != std::string::npos;
    g_upd.hasNsp = body.find(std::string("/") + UPDATE_NSP_NAME + "\"") != std::string::npos;
    g_upd.newer = verNewer(g_upd.tag, APP_VERSION_STR);
    g_upd.ok = true;
    applog("update: latest=%s newer=%d nro=%d nsp=%d", g_upd.tag.c_str(), g_upd.newer ? 1 : 0, g_upd.hasNro ? 1 : 0,
           g_upd.hasNsp ? 1 : 0);
    return true;
}

// ---- ダウンロード ----
struct DlView {
    std::string title;
    double frac = 0;
    long long got = 0, total = 0;
};
static DlView g_dl;
static bool g_dlCancel = false;

static void drawDownloading() {
    drawBackground();
    drawHeader("アップデート");
    drawCard(36, 100, 1208, 170);
    drawText(64, 116, fitText(fBody, g_dl.title, 1150), fBody, C_TEXT);
    std::string sz = g_dl.total > 0 ? fmtMB(g_dl.got) + " / " + fmtMB(g_dl.total) : fmtMB(g_dl.got);
    drawText(64, 156, sz, fSmall, C_DIM);
    char pc[16];
    snprintf(pc, sizeof(pc), "%d%%", (int)(std::min(1.0, g_dl.frac) * 100));
    drawText(1216, 156, pc, fSmall, C_TEXT, 1);
    drawProgress(64, 204, 1152, 14, g_dl.frac);
    drawFooter("B: 中止");
    SDL_RenderPresent(g_ren);
}

static size_t fileWriteCb(char *p, size_t sz, size_t n, void *u) { return fwrite(p, sz, n, (FILE *)u); }

static int dlProgressCb(void *, curl_off_t dltotal, curl_off_t dlnow, curl_off_t, curl_off_t) {
    static Uint32 last = 0;
    Uint32 now = SDL_GetTicks();
    if (now - last < 100) return 0;
    last = now;
    g_dl.got = dlnow;
    g_dl.total = dltotal;
    g_dl.frac = dltotal > 0 ? (double)dlnow / (double)dltotal : 0;
    updateInput();
    if (g_quit || g_cur[BT_B]) { g_dlCancel = true; return 1; }
    drawDownloading();
    return 0;
}

static bool downloadFile(const std::string &url, const std::string &path, std::string &err) {
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) { err = "保存先を開けません(" + path + ")"; return false; }
    CURL *c = curl_easy_init();
    if (!c) { fclose(f); remove(path.c_str()); err = "通信の準備に失敗しました"; return false; }
    setCommon(c, url);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, fileWriteCb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, f);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 600L);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, dlProgressCb);
    g_dlCancel = false;
    g_dl.frac = 0;
    g_dl.got = g_dl.total = 0;
    CURLcode rc = curl_easy_perform(c);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(c);
    fclose(f);
    if (rc != CURLE_OK) {
        remove(path.c_str());
        if (g_dlCancel) err = "中止しました";
        else if (rc == CURLE_HTTP_RETURNED_ERROR) err = "ダウンロードできませんでした(http " + std::to_string(code) + ")";
        else err = std::string("ダウンロードに失敗しました(") + curl_easy_strerror(rc) + ")";
        applog("update: download %s rc=%d code=%ld", url.c_str(), (int)rc, code);
        return false;
    }
    return true;
}

// ファイルの大きさと、決まった位置の目印(NROなら0x10に "NRO0"、NSPなら先頭に "PFS0")を確かめる
static bool fileHasMagic(const std::string &path, long off, const char *magic, size_t mlen, long minSize) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    bool ok = sz >= minSize;
    if (ok) {
        char b[8] = {0};
        fseek(f, off, SEEK_SET);
        ok = fread(b, 1, mlen, f) == mlen && memcmp(b, magic, mlen) == 0;
    }
    fclose(f);
    return ok;
}

// 新しいファイルを本番の名前にする。古いほうは、いったん .bak に退避して、成功したら消す
static bool swapFile(const std::string &tmp, const std::string &dst, std::string &err) {
    std::string bak = dst + ".bak";
    bool had = fileExists(dst);
    remove(bak.c_str());
    if (had && rename(dst.c_str(), bak.c_str()) != 0) {
        err = "いまのファイルを置き換えられません(" + std::string(strerror(errno)) + ")";
        return false;
    }
    if (rename(tmp.c_str(), dst.c_str()) != 0) {
        err = "新しいファイルを置けません(" + std::string(strerror(errno)) + ")";
        if (had) rename(bak.c_str(), dst.c_str());  // 元に戻す
        return false;
    }
    if (had) remove(bak.c_str());
    return true;
}

static void updFail(const std::string &err, const std::vector<std::string> &extra = {}) {
    std::vector<std::string> lines = {err};
    for (auto &e : extra) lines.push_back(e);
    showMessage("更新できませんでした", lines, false);
}

static void screenUpdate() {
    if (!g_curlReady || !networkUp()) {
        showMessage("Wi-Fiにつながっていません", {"つながってから、もう一度試してください"}, false);
        return;
    }
    drawStatusScreen("アップデート", "GitHubで最新の版を確認しています...");
    if (!updCheck(10)) {
        showMessage("確認できませんでした", {g_upd.err}, false);
        return;
    }
    if (!g_upd.newer) {
        showMessage("最新の版です", {"いまの版: v" APP_VERSION_STR, "GitHubの最新: " + g_upd.tag}, true);
        return;
    }
    if (!g_upd.hasNro) {
        showMessage("更新ファイルが見つかりません", {"リリース " + g_upd.tag + " に " + UPDATE_NRO_NAME + " が付いていません"}, false);
        return;
    }
    bool sysPresent = fileExists(SYS_NSP_PATH);
    bool doSys = sysPresent && g_upd.hasNsp;

    // 確認
    for (;;) {
        if (g_quit || !appletMainLoop()) return;
        beginFrame();
        drawHeader("アップデート");
        drawCard(36, 100, 1208, 300);
        fillRRect(64, 128, 8, 36, 4, C_OK);
        drawText(88, 124, "新しい版があります", fTitle, C_OK);
        drawText(88, 190, "いまの版: v" APP_VERSION_STR, fBody, C_DIM);
        drawText(88, 228, "新しい版: " + g_upd.tag, fBody, C_TEXT);
        drawText(88, 280, "更新するもの", fSmall, C_DIM);
        drawText(88, 308, std::string("・アプリ(") + UPDATE_NRO_NAME + ")", fBody, C_TEXT);
        if (doSys) drawText(88, 346, "・常駐版(Switchを再起動すると、新しくなります)", fBody, C_TEXT);
        else if (!sysPresent) drawText(88, 346, "・常駐版は入っていないので、更新しません", fBody, C_DIM);
        else drawText(88, 346, "・常駐版は、このリリースに付いていないので、更新しません", fBody, C_DIM);
        drawFooter("A: 更新する  B: やめる");
        endFrame();
        if (pressed(BT_B)) return;
        if (pressed(BT_A)) break;
        SDL_Delay(16);
    }

    std::string base = std::string("https://github.com/") + UPDATE_REPO + "/releases/download/" + g_upd.tag + "/";
    std::string nroNew = g_selfPath + ".new";
    std::string nspNew = std::string(SYS_NSP_PATH) + ".new";
    std::string err;

    g_dl = DlView();
    g_dl.title = std::string("アプリ(") + UPDATE_NRO_NAME + ")をダウンロードしています";
    drawDownloading();
    bool ok = downloadFile(base + UPDATE_NRO_NAME, nroNew, err);
    if (ok && !fileHasMagic(nroNew, 0x10, "NRO0", 4, 100000)) { ok = false; err = "ダウンロードしたファイルが、アプリではありませんでした"; }
    if (!ok) {
        remove(nroNew.c_str());
        updFail(err, {"いまのアプリは、そのまま使えます"});
        return;
    }
    if (doSys) {
        g_dl = DlView();
        g_dl.title = std::string("常駐版(") + UPDATE_NSP_NAME + ")をダウンロードしています";
        drawDownloading();
        ok = downloadFile(base + UPDATE_NSP_NAME, nspNew, err);
        if (ok && !fileHasMagic(nspNew, 0, "PFS0", 4, 4096)) { ok = false; err = "ダウンロードしたファイルが、常駐版ではありませんでした"; }
        if (!ok) {
            remove(nroNew.c_str());
            remove(nspNew.c_str());
            updFail(err, {"いまのアプリと常駐版は、そのまま使えます"});
            return;
        }
    }

    // 入れ替え(常駐版 → アプリ の順)
    drawStatusScreen("アップデート", "入れ替えています...");
    if (doSys && !swapFile(nspNew, SYS_NSP_PATH, err)) {
        remove(nroNew.c_str());
        remove(nspNew.c_str());
        updFail(err, {"いまのアプリと常駐版は、そのまま使えます"});
        return;
    }
    if (!swapFile(nroNew, g_selfPath, err)) {
        std::vector<std::string> ex = {"新しいアプリは " + nroNew + " に置いてあります"};
        ex.push_back("PCやファイル管理で、NXShot.nro と入れ替えてください");
        if (doSys) ex.push_back("常駐版は、すでに新しくなっています(再起動で反映)");
        updFail(err, ex);
        return;
    }
    applog("update: done -> %s (sys=%d)", g_upd.tag.c_str(), doSys ? 1 : 0);

    std::vector<std::string> lines = {std::string("アプリを ") + g_upd.tag + " にしました"};
    if (doSys) lines.push_back("常駐版も新しくしました(Switchを再起動すると、新しい版で動きます)");
    bool relaunch = envHasNextLoad();
    lines.push_back(relaunch ? "Aを押すと、新しいアプリを開き直します" : "いったん終了します。ホームブリューメニューから、開き直してください");
    showMessage("更新しました", lines, true);
    if (relaunch) envSetNextLoad(g_selfPath.c_str(), g_selfPath.c_str());
    g_quit = true;
}

// ---- メニューの小さな絵(基本図形で描く) ----
// kind: 0=写真 1=設定 2=送信 3=状態 4=アップデート 5=終了   bg: 絵の下の色(穴あけ用)
static void drawIcon(int kind, int cx, int cy, SDL_Color c, SDL_Color bg) {
    switch (kind) {
        case 0:  // 写真: 枠 + 太陽 + 山
            strokeRRect(cx - 13, cy - 10, 26, 20, 5, 2, c);
            fillCircle(cx + 5, cy - 3, 3, c);
            fillTri(cx - 9, cy + 7, cx - 3, cy - 1, cx + 3, cy + 7, c);
            fillTri(cx - 1, cy + 7, cx + 4, cy + 1, cx + 9, cy + 7, c);
            break;
        case 1:  // 設定: 歯車
            fillRect(cx - 2, cy - 13, 4, 6, c);
            fillRect(cx - 2, cy + 7, 4, 6, c);
            fillRect(cx - 13, cy - 2, 6, 4, c);
            fillRect(cx + 7, cy - 2, 6, 4, c);
            fillCircle(cx, cy, 9, c);
            fillCircle(cx, cy, 4, bg);
            break;
        case 2:  // 送信: 紙ひこうき
            fillTri(cx - 12, cy - 1, cx + 12, cy - 11, cx + 3, cy + 11, c);
            fillTri(cx - 2, cy + 1, cx + 12, cy - 11, cx + 3, cy + 11, bg);
            thickLine(cx - 2, cy + 1, cx + 11, cy - 10, 3, c);
            break;
        case 3:  // 状態: 脈
            thickLine(cx - 13, cy, cx - 6, cy, 3, c);
            thickLine(cx - 6, cy, cx - 3, cy - 9, 3, c);
            thickLine(cx - 3, cy - 9, cx + 2, cy + 9, 3, c);
            thickLine(cx + 2, cy + 9, cx + 5, cy, 3, c);
            thickLine(cx + 5, cy, cx + 13, cy, 3, c);
            break;
        case 4:  // アップデート: 下向きの矢印と台
            fillRect(cx - 3, cy - 12, 6, 11, c);
            fillTri(cx - 10, cy - 2, cx + 10, cy - 2, cx, cy + 9, c);
            fillRect(cx - 12, cy + 12, 24, 3, c);
            break;
        default:  // 終了: 電源
            fillCircle(cx, cy + 1, 11, c);
            fillCircle(cx, cy + 1, 7, bg);
            fillRect(cx - 4, cy - 14, 8, 12, bg);
            fillRect(cx - 2, cy - 13, 4, 12, c);
            break;
    }
}

// ====================================================================
// メニュー
// ====================================================================
static void screenMenu() {
    const char *items[] = {"写真・動画を選んで送る", "設定", "テスト送信", "常駐版の状態とログ", "アップデート", "終了"};
    const int N = 6;
    int cur = 0;
    while (!g_quit && appletMainLoop()) {
        beginFrame();
        drawHeader(std::string("v") + APP_VERSION_STR);
        for (int i = 0; i < N; i++) {
            int y = 92 + i * 72;
            bool sel = i == cur;
            if (sel) {
                fillRRect(36, y, 770, 62, 12, C_ACC);
                strokeRRect(36, y, 770, 62, 12, 2, C_ACC2);
            } else {
                fillRRect(36, y, 770, 62, 12, SDL_Color{34, 37, 54, 200});
            }
            SDL_Color tileBg = sel ? SDL_Color{255, 255, 255, 255} : SDL_Color{52, 57, 86, 255};
            fillRRect(50, y + 9, 44, 44, 11, tileBg);
            drawIcon(i, 72, y + 31, sel ? C_ACC : C_ACC2, tileBg);
            drawTextMid(112, y + 31, items[i], fBody, sel ? C_WHITE : C_TEXT);
            if (i == 4 && g_upd.ok && g_upd.newer) {
                fillRRect(660, y + 17, 130, 28, 14, C_WARN);
                drawTextMid(725, y + 31, g_upd.tag + " あり", fSmall, SDL_Color{40, 36, 10, 255}, 2);
            }
        }
        // 右側の状態パネル
        drawCard(836, 92, 408, 392);
        drawText(860, 106, "いまの状態", fBody, C_TEXT);
        bool up = networkUp();
        struct Row { const char *label; std::string value; SDL_Color c; };
        Row rows[7] = {
            {"Webhook", hookOk() ? "設定済み" : "未設定", hookOk() ? C_OK : C_WARN},
            {"Wi-Fi", up ? "つながっています" : "つながっていません", up ? C_OK : C_WARN},
            {"自動送信", g_cfg.autoSend ? "オン" : "オフ", g_cfg.autoSend ? C_OK : C_WARN},
            {"上限", std::to_string(g_cfg.limitMB) + " MB", C_TEXT},
            {"大きい動画を削る", g_cfg.trim ? "する" : "しない", C_TEXT},
            {"送信済み(記録)", std::to_string(g_sent.size()) + "件", C_TEXT},
            {"アプリの版", std::string("v") + APP_VERSION_STR, C_TEXT},
        };
        for (int i = 0; i < 7; i++) {
            int cy = 168 + i * 44;
            fillRect(860, cy + 22, 360, 1, SDL_Color{255, 255, 255, 18});
            drawTextMid(860, cy, rows[i].label, fSmall, C_DIM);
            drawTextMid(1220, cy, rows[i].value, fSmall, rows[i].c, 1);
        }
        if (!hookOk()) drawText(40, 540, "はじめに「設定」で、Webhook URLを入れてください", fSmall, C_WARN);
        if (g_upd.ok && g_upd.newer) {
            drawCard(36, 570, 770, 72);
            fillRRect(56, 586, 6, 40, 3, C_WARN);
            drawText(78, 578, "新しい版 " + g_upd.tag + " があります", fBody, C_WARN);
            drawText(78, 610, "「アップデート」を選ぶと、アプリと常駐版を新しくできます", fSmall, C_TEXT);
        }
        drawFooter(g_cfg.autoSend ? "上下: 選ぶ  A: 決定  +: 終了  (起動後に撮ったものは、常駐版が自動で送ります)"
                                  : "上下: 選ぶ  A: 決定  +: 終了  (自動送信はオフです。設定でオンにできます)");
        endFrame();

        if (pressed(BT_DOWN)) cur = (cur + 1) % N;
        if (pressed(BT_UP)) cur = (cur + N - 1) % N;
        if (pressed(BT_A)) {
            switch (cur) {
                case 0:
                    if (!hookOk()) showMessage("先に設定が必要です", {"「設定」で、Webhook URLを入れてください"}, false);
                    else screenYears();
                    break;
                case 1: screenSettings(); break;
                case 2: doTestSend(); break;
                case 3: screenStatus(); break;
                case 4: screenUpdate(); break;
                default: return;
            }
        }
        if (pressed(BT_PLUS)) return;
        SDL_Delay(16);
    }
}

// ====================================================================
// 起動と後始末
// ====================================================================
int main(int argc, char **argv) {
    mkdir("sdmc:/config", 0777);
    mkdir(CONFIG_DIR, 0777);
    initSelfPath(argc, argv);
    applog("app: start v%s (%s)", APP_VERSION_STR, g_selfPath.c_str());

    Result rc = socketInitializeDefault();
    applog("init: socket rc=0x%x", (unsigned)rc);
    rc = nifmInitialize(NifmServiceType_User);
    g_nifmOk = R_SUCCEEDED(rc);
    applog("init: nifm rc=0x%x", (unsigned)rc);
    rc = sslInitialize(3);  // curlのHTTPSはlibnxのsslサービスを使う
    bool sslOk = R_SUCCEEDED(rc);
    applog("init: ssl rc=0x%x", (unsigned)rc);
    rc = pmdmntInitialize();
    bool pmOk = R_SUCCEEDED(rc);
    applog("init: pmdmnt rc=0x%x", (unsigned)rc);
    rc = plInitialize(PlServiceType_User);
    bool plOk = R_SUCCEEDED(rc);
    applog("init: pl rc=0x%x", (unsigned)rc);

    CURLcode crc = curl_global_init(CURL_GLOBAL_DEFAULT);
    g_curlReady = (crc == CURLE_OK);
    applog("init: curl=%d (%s)", (int)crc, curl_easy_strerror(crc));

    loadConfig();
    loadSent();

    bool sdlOk = SDL_Init(SDL_INIT_VIDEO) == 0;
    applog("init: SDL=%d", sdlOk ? 1 : 0);
    bool ttfOk = sdlOk && TTF_Init() == 0;
    int imgFlags = sdlOk ? IMG_Init(IMG_INIT_JPG) : 0;
    bool imgOk = (imgFlags & IMG_INIT_JPG) != 0;
    applog("init: IMG_jpg=%d", imgOk ? 1 : 0);
#ifdef NXSHOT_FFMPEG
    av_log_set_level(AV_LOG_QUIET);
    applog("init: ffmpeg=1");
#else
    applog("init: ffmpeg=0");
#endif
    if (sdlOk) SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");  // サムネを縮小して出すとき、ぎざぎざにならないように
    PlFontData fd;
    bool fontOk = ttfOk && plOk && R_SUCCEEDED(plGetSharedFontByType(&fd, PlSharedFontType_Standard));
    applog("init: font=%d", fontOk ? 1 : 0);

    if (fontOk) {
        g_win = SDL_CreateWindow("NXShot", 0, 0, 1280, 720, 0);
        if (g_win) g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        if (g_ren) SDL_RenderSetLogicalSize(g_ren, 1280, 720);
        if (g_ren) SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);
        fTitle = TTF_OpenFontRW(SDL_RWFromMem(fd.address, fd.size), 1, 30);
        fBody = TTF_OpenFontRW(SDL_RWFromMem(fd.address, fd.size), 1, 21);
        fSmall = TTF_OpenFontRW(SDL_RWFromMem(fd.address, fd.size), 1, 17);
        applog("init: window=%d renderer=%d fonts=%d%d%d", g_win ? 1 : 0, g_ren ? 1 : 0, fTitle ? 1 : 0, fBody ? 1 : 0,
               fSmall ? 1 : 0);
    }

    if (g_win && g_ren && fTitle && fBody && fSmall) {
        initInput();
        makeBackground();
        if (g_cfg.updCheck && g_curlReady && networkUp()) {
            drawStatusScreen("起動中", "アップデートを確認しています...");
            updCheck(5);
        }
        screenMenu();
    } else {
        applog("画面を作れませんでした");
    }

    if (g_bg) SDL_DestroyTexture(g_bg);
    for (auto &p : g_texCache) SDL_DestroyTexture(p.second.t);
    g_texCache.clear();
    for (auto &p : g_thumbs) if (p.second.t) SDL_DestroyTexture(p.second.t);
    g_thumbs.clear();
    if (fTitle) TTF_CloseFont(fTitle);
    if (fBody) TTF_CloseFont(fBody);
    if (fSmall) TTF_CloseFont(fSmall);
    if (g_ren) SDL_DestroyRenderer(g_ren);
    if (g_win) SDL_DestroyWindow(g_win);
    if (imgOk) IMG_Quit();
    if (ttfOk) TTF_Quit();
    if (sdlOk) SDL_Quit();
    if (g_curlReady) curl_global_cleanup();
    if (plOk) plExit();
    if (pmOk) pmdmntExit();
    if (sslOk) sslExit();
    if (g_nifmOk) nifmExit();
    socketExit();
    return 0;
}
