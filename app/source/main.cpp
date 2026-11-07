// NXDiscord アプリ (.nro)
// 常駐版(sysmodule)とは別に、ホームブリューメニューから起動して使う。
//   ・Albumの古い写真・動画を、選んでDiscordのWebhookへ送る(大きい動画は削って収める)
//   ・設定(config.txt)を、Switchの画面で変える
//   ・テスト送信 / 常駐版の状態とログの確認
// 依存: libnx + SDL2 + SDL2_ttf + libcurl。日本語はSwitch本体の共有フォントで表示する。
#include <switch.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <curl/curl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdarg>
#include <string>
#include <vector>
#include <set>
#include <unordered_map>
#include <algorithm>
#include "../../sysmodule/source/mp4trim.h"

#define APP_VERSION_STR "1.0.0"
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
//   1行目: Webhook URL / limit=10 / trim=1 / album=sdmc:/...
// ====================================================================
struct Config {
    std::string hook;
    std::string album = DEFAULT_ALBUM;
    long limitMB = DEFAULT_LIMIT_MB;
    bool trim = true;
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
static const SDL_Color C_BAR = {32, 34, 46, 255};
static const SDL_Color C_TEXT = {235, 237, 245, 255};
static const SDL_Color C_DIM = {150, 155, 175, 255};
static const SDL_Color C_ACC = {88, 101, 242, 255};
static const SDL_Color C_OK = {87, 242, 135, 255};
static const SDL_Color C_NG = {237, 66, 69, 255};
static const SDL_Color C_WARN = {254, 231, 92, 255};

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
    SDL_SetRenderDrawColor(g_ren, C_BG.r, C_BG.g, C_BG.b, 255);
    SDL_RenderClear(g_ren);
}

static void endFrame() {
    SDL_RenderPresent(g_ren);
}

static void drawHeader(const std::string &sub) {
    fillRect(0, 0, 1280, 88, C_BAR);
    drawText(48, 20, "NXDiscord", fTitle, C_TEXT);
    drawText(1232, 34, sub, fBody, C_DIM, 1);
}

static void drawFooter(const std::string &hint) {
    fillRect(0, 656, 1280, 64, C_BAR);
    drawText(48, 674, hint, fSmall, C_DIM);
}

// ---- 簡単なメッセージ画面(Aで閉じる) ----
static void showMessage(const std::string &title, const std::vector<std::string> &lines, bool good) {
    while (!g_quit && appletMainLoop()) {
        beginFrame();
        drawHeader("お知らせ");
        drawText(80, 130, title, fTitle, good ? C_OK : C_WARN);
        int y = 210;
        for (auto &l : lines) {
            drawText(80, y, fitText(fBody, l, 1120), fBody, C_TEXT);
            y += 42;
        }
        drawFooter("A: 閉じる");
        endFrame();
        if (pressed(BT_A) || pressed(BT_B)) return;
        SDL_Delay(16);
    }
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
    SDL_SetRenderDrawColor(g_ren, C_BG.r, C_BG.g, C_BG.b, 255);
    SDL_RenderClear(g_ren);
    drawHeader("送信中");
    drawText(80, 120, g_view.title, fTitle, C_TEXT);
    drawText(80, 190, fitText(fBody, g_view.file, 1120), fBody, C_DIM);
    drawText(80, 238, fitText(fBody, g_view.status, 1120), fBody, C_TEXT);
    fillRect(80, 296, 1120, 22, C_BAR);
    int w = (int)(1120 * std::min(1.0, std::max(0.0, g_view.frac)));
    fillRect(80, 296, w, 22, C_ACC);
    int y = 350;
    size_t start = g_view.results.size() > 7 ? g_view.results.size() - 7 : 0;
    for (size_t i = start; i < g_view.results.size(); i++) {
        SDL_Color c = g_view.results[i].second == 0 ? C_OK : (g_view.results[i].second == 1 ? C_WARN : C_NG);
        drawText(80, y, fitText(fSmall, g_view.results[i].first, 1120), fSmall, c);
        y += 36;
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
    curl_easy_setopt(c, CURLOPT_USERAGENT, "NXDiscord-App/1.0");
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

struct Day {
    std::string path;
    std::string label;
    int images = 0, videos = 0;
};

static void scanDays(std::vector<Day> &days) {
    days.clear();
    for (auto &y : digitDirsNewestFirst(g_cfg.album)) {
        std::string py = g_cfg.album + "/" + y;
        for (auto &m : digitDirsNewestFirst(py)) {
            std::string pm = py + "/" + m;
            for (auto &d : digitDirsNewestFirst(pm)) {
                Day day;
                day.path = pm + "/" + d;
                day.label = y + "/" + m + "/" + d;
                std::vector<std::string> dirs, files;
                listDir(day.path, dirs, files);
                for (auto &f : files) {
                    int k = mediaKind(f);
                    if (k == MK_IMAGE) day.images++;
                    else if (k == MK_VIDEO) day.videos++;
                }
                if (day.images + day.videos > 0) days.push_back(day);
            }
        }
    }
}

struct Item {
    std::string name, full;
    int kind = MK_NONE;
    long long size = 0;
    bool sel = false;
    bool sent = false;
};

static void loadItems(const Day &day, std::vector<Item> &items) {
    items.clear();
    std::vector<std::string> dirs, files;
    listDir(day.path, dirs, files);
    std::sort(files.begin(), files.end(), [](const std::string &a, const std::string &b) { return a > b; });
    for (auto &f : files) {
        int k = mediaKind(f);
        if (k == MK_NONE) continue;
        Item it;
        it.name = f;
        it.full = day.path + "/" + f;
        it.kind = k;
        struct stat st;
        it.size = (stat(it.full.c_str(), &st) == 0) ? (long long)st.st_size : 0;
        it.sent = g_sent.count(f) > 0;
        items.push_back(it);
    }
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

static int sendOne(const Item &it, std::string &msg) {
    bool isVideo = it.kind == MK_VIDEO;
    std::string sendPath = it.full;
    std::string note;
    bool trimmed = false;
    long long limitBytes = (long long)g_cfg.limitMB * 1048576LL - 65536;

    if (it.size > limitBytes) {
        mp4trim::Info ti;
        mp4trim::Result tr = mp4trim::ERR_FORMAT;
        if (isVideo && g_cfg.trim) {
            g_view.status = "動画を削って、上限に収めています...";
            g_view.frac = 0;
            drawSending();
            applog("trim: 開始 %s (%lld bytes, 上限 %lld)", it.name.c_str(), it.size, limitBytes);
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
            msg = "上限(" + std::to_string(g_cfg.limitMB) + "MB)を超えるため送りませんでした(" + fmtMB(it.size) + ")";
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
    long long sendSize = (stat(sendPath.c_str(), &sst) == 0) ? (long long)sst.st_size : it.size;
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
// 画面: 日付の一覧 → ファイルの一覧
// ====================================================================
static void screenFiles(const Day &day) {
    std::vector<Item> items;
    loadItems(day, items);
    int cur = 0, top = 0;
    const int ROWS = 14;
    while (!g_quit && appletMainLoop()) {
        beginFrame();
        drawHeader(day.label);
        int selCount = 0;
        for (auto &it : items) if (it.sel) selCount++;
        char hd[200];
        snprintf(hd, sizeof(hd), "%zu件   選んだ数: %d   上限: %ldMB   動画を削る: %s", items.size(), selCount, g_cfg.limitMB,
                 g_cfg.trim ? "する" : "しない");
        drawText(48, 104, hd, fSmall, C_DIM);

        if (items.empty()) drawText(80, 200, "この日の写真・動画はありません", fBody, C_DIM);
        for (int r = 0; r < ROWS; r++) {
            int i = top + r;
            if (i >= (int)items.size()) break;
            Item &it = items[i];
            int y = 140 + r * 36;
            if (i == cur) fillRect(40, y - 2, 1200, 36, C_ACC);
            // チェックボックス
            frameRect(60, y + 5, 20, 20, C_TEXT);
            if (it.sel) fillRect(64, y + 9, 12, 12, C_OK);
            drawText(100, y, timeLabel(it.name, false), fBody, C_TEXT);
            drawText(260, y, it.kind == MK_VIDEO ? "動画" : "写真", fBody, it.kind == MK_VIDEO ? C_WARN : C_TEXT);
            drawText(370, y, fmtMB(it.size), fBody, C_TEXT);
            bool over = it.size > (long long)g_cfg.limitMB * 1048576LL - 65536;
            if (over) {
                if (it.kind == MK_VIDEO && g_cfg.trim) drawText(500, y, "削って送る", fBody, C_WARN);
                else drawText(500, y, "上限超え(送れません)", fBody, C_NG);
            }
            if (it.sent) drawText(1210, y, "送信済", fBody, C_OK, 1);
        }
        drawFooter("上下: 選ぶ   A: チェック   Y: 全部チェック/外す   X: 送る   B: 戻る");
        endFrame();

        if (pressed(BT_B)) return;
        if (!items.empty()) {
            if (pressed(BT_DOWN)) { if (cur < (int)items.size() - 1) cur++; }
            if (pressed(BT_UP)) { if (cur > 0) cur--; }
            if (pressed(BT_R)) cur = std::min((int)items.size() - 1, cur + ROWS);
            if (pressed(BT_L)) cur = std::max(0, cur - ROWS);
            if (cur < top) top = cur;
            if (cur >= top + ROWS) top = cur - ROWS + 1;
            if (pressed(BT_A)) items[cur].sel = !items[cur].sel;
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
        SDL_Delay(16);
    }
}

static void screenDays() {
    beginFrame();
    drawHeader("写真・動画を送る");
    drawText(80, 200, "Albumを読み込んでいます...", fBody, C_TEXT);
    endFrame();
    std::vector<Day> days;
    scanDays(days);
    applog("days: %zu (album=%s)", days.size(), g_cfg.album.c_str());
    int cur = 0, top = 0;
    const int ROWS = 12;
    while (!g_quit && appletMainLoop()) {
        beginFrame();
        drawHeader("日付を選ぶ");
        if (days.empty()) {
            drawText(80, 160, "写真・動画が見つかりません", fTitle, C_WARN);
            drawText(80, 230, "保存先が「SDカード」か確認してください", fBody, C_TEXT);
            drawText(80, 276, "Albumの場所: " + g_cfg.album, fSmall, C_DIM);
        }
        for (int r = 0; r < ROWS; r++) {
            int i = top + r;
            if (i >= (int)days.size()) break;
            int y = 124 + r * 44;
            if (i == cur) fillRect(40, y - 4, 1200, 44, C_ACC);
            drawText(80, y, days[i].label, fBody, C_TEXT);
            char cnt[64];
            snprintf(cnt, sizeof(cnt), "写真 %d  動画 %d", days[i].images, days[i].videos);
            drawText(1200, y, cnt, fBody, C_TEXT, 1);
        }
        drawFooter("上下: 選ぶ   A: 開く   B: 戻る");
        endFrame();

        if (pressed(BT_B)) return;
        if (!days.empty()) {
            if (pressed(BT_DOWN)) { if (cur < (int)days.size() - 1) cur++; }
            if (pressed(BT_UP)) { if (cur > 0) cur--; }
            if (pressed(BT_R)) cur = std::min((int)days.size() - 1, cur + ROWS);
            if (pressed(BT_L)) cur = std::max(0, cur - ROWS);
            if (cur < top) top = cur;
            if (cur >= top + ROWS) top = cur - ROWS + 1;
            if (pressed(BT_A)) screenFiles(days[cur]);
        }
        SDL_Delay(16);
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
    const int N = 5;
    while (!g_quit && appletMainLoop()) {
        beginFrame();
        drawHeader("設定");
        const char *names[N] = {"Webhook URL", "送れるファイルの上限", "大きい動画を削って送る", "Albumの場所", "戻る"};
        std::string vals[N];
        vals[0] = maskHook(g_cfg.hook);
        vals[1] = std::to_string(g_cfg.limitMB) + " MB";
        vals[2] = g_cfg.trim ? "する" : "しない";
        vals[3] = g_cfg.album == DEFAULT_ALBUM ? "標準(SDカードのAlbum)" : g_cfg.album;
        vals[4] = "";
        for (int i = 0; i < N; i++) {
            int y = 130 + i * 76;
            if (i == cur) fillRect(40, y - 8, 1200, 68, C_ACC);
            drawText(80, y + 4, names[i], fBody, C_TEXT);
            if (!vals[i].empty()) drawText(1200, y + 4, fitText(fBody, vals[i], 700), fBody, i == 0 && !hookOk() ? C_WARN : C_TEXT, 1);
        }
        std::string hint;
        if (cur == 0) hint = "A: 入力(長いURLは、PCで config.txt を書き換えるほうが楽です)";
        else if (cur == 1) hint = "左右: 1MBずつ  L/R: 10MBずつ  A: 数字で入力  (ブーストなしのサーバーは約10MB)";
        else if (cur == 2) hint = "A: する/しない を切り替え(しない: 大きい動画は送りません)";
        else if (cur == 3) hint = "A: 入力(空にすると標準に戻る)。emuMMCのときだけ変えます";
        else hint = "A または B: 戻る";
        drawText(48, 560, fitText(fSmall, hint, 1180), fSmall, C_DIM);
        if (SDL_GetTicks() < toastUntil) drawText(1232, 110, toast, fSmall, C_OK, 1);
        drawFooter("上下: 選ぶ   A: 変える   B: 戻る(変えた内容は、すぐ保存されます)");
        endFrame();

        if (pressed(BT_B)) return;
        if (pressed(BT_DOWN)) cur = (cur + 1) % N;
        if (pressed(BT_UP)) cur = (cur + N - 1) % N;
        if (cur == 1) {
            if (pressed(BT_RIGHT) && g_cfg.limitMB < 500) { g_cfg.limitMB++; changed(); }
            if (pressed(BT_LEFT) && g_cfg.limitMB > 1) { g_cfg.limitMB--; changed(); }
            if (pressed(BT_R)) { g_cfg.limitMB = std::min(500L, g_cfg.limitMB + 10); changed(); }
            if (pressed(BT_L)) { g_cfg.limitMB = std::max(1L, g_cfg.limitMB - 10); changed(); }
        }
        if (pressed(BT_A)) {
            std::string out;
            if (cur == 0) {
                if (kbdInput("Webhook URL", "https://discord.com/api/webhooks/...", g_cfg.hook, 300, false, out)) {
                    if (out.compare(0, 8, "https://") == 0) { g_cfg.hook = out; changed(); }
                    else showMessage("URLが正しくありません", {"https:// で始まるURLを入れてください"}, false);
                }
            } else if (cur == 1) {
                if (kbdInput("上限(MB)", "1〜500", std::to_string(g_cfg.limitMB), 3, true, out)) {
                    long v = atol(out.c_str());
                    if (v >= 1 && v <= 500) { g_cfg.limitMB = v; changed(); }
                    else showMessage("数が正しくありません", {"1から500までの数を入れてください"}, false);
                }
            } else if (cur == 2) {
                g_cfg.trim = !g_cfg.trim;
                changed();
            } else if (cur == 3) {
                std::string init = g_cfg.album == DEFAULT_ALBUM ? "" : g_cfg.album;
                if (kbdInput("Albumの場所", "sdmc:/Nintendo/Album", init, 200, false, out)) {
                    while (out.size() > 1 && out.back() == '/') out.pop_back();
                    g_cfg.album = out.empty() ? std::string(DEFAULT_ALBUM) : out;
                    changed();
                }
            } else {
                return;
            }
        }
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

    int top = lines.size() > 12 ? (int)lines.size() - 12 : 0;
    while (!g_quit && appletMainLoop()) {
        beginFrame();
        drawHeader("常駐版の状態");
        drawText(80, 110, "常駐版: " + running, fBody, running == "動いています" ? C_OK : C_WARN);
        drawText(520, 110, "版: " + ver, fBody, C_TEXT);
        drawText(760, 110, std::string("Wi-Fi: ") + (networkUp() ? "つながっています" : "つながっていません"), fBody, networkUp() ? C_OK : C_WARN);
        drawText(80, 154, std::string("Webhook: ") + (hookOk() ? "設定済み" : "未設定"), fBody, hookOk() ? C_OK : C_WARN);
        if (!hasLog) drawText(80, 230, "常駐版のログ(log.txt)がありません。常駐版が、まだ動いていないかもしれません。", fSmall, C_WARN);
        for (int r = 0; r < 12; r++) {
            int i = top + r;
            if (i >= (int)lines.size()) break;
            drawText(80, 210 + r * 34, fitText(fSmall, lines[i], 1120), fSmall, C_TEXT);
        }
        drawFooter("上下: ログをめくる   B: 戻る");
        endFrame();

        if (pressed(BT_B)) return;
        if (pressed(BT_UP) && top > 0) top--;
        if (pressed(BT_DOWN) && top + 12 < (int)lines.size()) top++;
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
                       "\",\"embeds\":[{\"title\":\"🟢 NXDiscordアプリからのテスト\",\"description\":\"このメッセージが届けば、Webhookは正しく動いています。\",\"color\":5763719}]}";
    long code = postJson(g_cfg.hook, json);
    if (code >= 200 && code < 300) showMessage("送れました", {"Discordのチャンネルを見てください(http " + std::to_string(code) + ")"}, true);
    else showMessage("送れませんでした", {httpHint(code)}, false);
}

// ====================================================================
// メニュー
// ====================================================================
static void screenMenu() {
    const char *items[] = {"写真・動画を選んで送る", "設定", "テスト送信", "常駐版の状態とログ", "終了"};
    const int N = 5;
    int cur = 0;
    while (!g_quit && appletMainLoop()) {
        beginFrame();
        drawHeader(std::string("アプリ v") + APP_VERSION_STR);
        for (int i = 0; i < N; i++) {
            int y = 126 + i * 72;
            if (i == cur) fillRect(40, y - 8, 760, 64, C_ACC);
            drawText(80, y + 2, items[i], fBody, C_TEXT);
        }
        // 右側の状態パネル
        fillRect(840, 118, 400, 330, C_BAR);
        drawText(864, 134, "いまの状態", fBody, C_TEXT);
        drawText(864, 188, std::string("Webhook: ") + (hookOk() ? "設定済み" : "未設定"), fSmall, hookOk() ? C_OK : C_WARN);
        drawText(864, 228, std::string("Wi-Fi: ") + (networkUp() ? "つながっています" : "つながっていません"), fSmall, networkUp() ? C_OK : C_WARN);
        drawText(864, 268, "上限: " + std::to_string(g_cfg.limitMB) + " MB", fSmall, C_TEXT);
        drawText(864, 308, std::string("大きい動画を削る: ") + (g_cfg.trim ? "する" : "しない"), fSmall, C_TEXT);
        drawText(864, 348, "送信済み(記録): " + std::to_string(g_sent.size()) + "件", fSmall, C_TEXT);
        if (!hookOk()) drawText(80, 520, "はじめに「設定」で、Webhook URLを入れてください", fBody, C_WARN);
        drawFooter("上下: 選ぶ   A: 決定   +: 終了   (起動後に撮ったものは、常駐版が自動で送ります)");
        endFrame();

        if (pressed(BT_DOWN)) cur = (cur + 1) % N;
        if (pressed(BT_UP)) cur = (cur + N - 1) % N;
        if (pressed(BT_A)) {
            switch (cur) {
                case 0:
                    if (!hookOk()) showMessage("先に設定が必要です", {"「設定」で、Webhook URLを入れてください"}, false);
                    else screenDays();
                    break;
                case 1: screenSettings(); break;
                case 2: doTestSend(); break;
                case 3: screenStatus(); break;
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
    (void)argc;
    (void)argv;
    mkdir("sdmc:/config", 0777);
    mkdir(CONFIG_DIR, 0777);
    applog("app: start v%s", APP_VERSION_STR);

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
    PlFontData fd;
    bool fontOk = ttfOk && plOk && R_SUCCEEDED(plGetSharedFontByType(&fd, PlSharedFontType_Standard));
    applog("init: font=%d", fontOk ? 1 : 0);

    if (fontOk) {
        g_win = SDL_CreateWindow("NXDiscord", 0, 0, 1280, 720, 0);
        if (g_win) g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        if (g_ren) SDL_RenderSetLogicalSize(g_ren, 1280, 720);
        fTitle = TTF_OpenFontRW(SDL_RWFromMem(fd.address, fd.size), 1, 40);
        fBody = TTF_OpenFontRW(SDL_RWFromMem(fd.address, fd.size), 1, 28);
        fSmall = TTF_OpenFontRW(SDL_RWFromMem(fd.address, fd.size), 1, 22);
        applog("init: window=%d renderer=%d fonts=%d%d%d", g_win ? 1 : 0, g_ren ? 1 : 0, fTitle ? 1 : 0, fBody ? 1 : 0,
               fSmall ? 1 : 0);
    }

    if (g_win && g_ren && fTitle && fBody && fSmall) {
        initInput();
        screenMenu();
    } else {
        applog("画面を作れませんでした");
    }

    for (auto &p : g_texCache) SDL_DestroyTexture(p.second.t);
    g_texCache.clear();
    if (fTitle) TTF_CloseFont(fTitle);
    if (fBody) TTF_CloseFont(fBody);
    if (fSmall) TTF_CloseFont(fSmall);
    if (g_ren) SDL_DestroyRenderer(g_ren);
    if (g_win) SDL_DestroyWindow(g_win);
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
