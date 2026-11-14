// NXShot 常駐版 sysmodule (PC不要版)
// ・Switch単体で、DiscordのWebhookへ直接HTTPS送信する
//   - ゲームの開始 / 終了(プレイ時間つき)の通知
//   - SDカードのAlbumに増えた新しいスクリーンショット(jpg)と動画(mp4)の自動投稿
//     (動画が上限を超えるときは、最初のほうを削って収めて送る)
// 依存: libnx + libcurl(switch-curl。HTTPSはlibnxのsslサービス経由)
#include <switch.h>
#include <curl/curl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <memory>
#include <unistd.h>
#include <cstdarg>
#include "mp4trim.h"

#define INNER_HEAP_SIZE 0x400000  // 4MB(TLS+アップロード用。2MBだと足りない恐れがある)
#define CONFIG_PATH "sdmc:/config/nxdiscord/config.txt"
#define DEFAULT_ALBUM "sdmc:/Nintendo/Album"
#define POLL_NS 5'000'000'000ULL  // 5秒ごとに確認
#define DEFAULT_LIMIT_MB 10      // 送信できるファイルの上限(MB)の初期値。config.txtのlimit=で変更可
#define VERSION "1.3.3"
#define TRIM_TMP_PATH "sdmc:/config/nxdiscord/trim_tmp.mp4"
#define LOG_PATH "sdmc:/config/nxdiscord/log.txt"
#define HOOK_NAME "NX Switch"  // 名前に「discord」を含めるとDiscordに拒否される

// ---- ログ(SDカードの config/nxdiscord/log.txt。調査用。Webhook URLは書かない) ----
static int g_logCount = 0;
static bool g_logFirst = true;
static void nxlog(const char *fmt, ...) {
    if (g_logCount >= 400) return;
    FILE *f = fopen(LOG_PATH, g_logFirst ? "w" : "a");
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

static bool g_nifmOk = false;
static bool g_sslOk = false;

extern "C" {
u32 __nx_applet_type = AppletType_None;
u32 __nx_fs_num_sessions = 1;
size_t nx_inner_heap_size = INNER_HEAP_SIZE;
char nx_inner_heap[INNER_HEAP_SIZE];

void __libnx_initheap(void) {
    extern char *fake_heap_start;
    extern char *fake_heap_end;
    fake_heap_start = nx_inner_heap;
    fake_heap_end = nx_inner_heap + nx_inner_heap_size;
}

void __appInit(void) {
    Result rc = smInitialize();
    if (R_FAILED(rc)) diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_SM));

    rc = setsysInitialize();
    if (R_SUCCEEDED(rc)) {
        SetSysFirmwareVersion fw;
        rc = setsysGetFirmwareVersion(&fw);
        if (R_SUCCEEDED(rc)) hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro));
        setsysExit();
    }

    rc = fsInitialize();
    if (R_FAILED(rc)) diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_FS));
    fsdevMountSdmc();
    nxlog("appInit: sdmc ok");

    Result rcNifm = nifmInitialize(NifmServiceType_User);
    nxlog("appInit: nifm rc=0x%x", (unsigned)rcNifm);
    g_nifmOk = R_SUCCEEDED(rcNifm);

    pmdmntInitialize();
    pminfoInitialize();
    nsInitialize();
    Result rcRng = csrngInitialize();  // TLSの乱数に使う
    nxlog("appInit: csrng rc=0x%x", (unsigned)rcRng);

    static const SocketInitConfig cfg = {
        .tcp_tx_buf_size = 0x4000,
        .tcp_rx_buf_size = 0x4000,
        .tcp_tx_buf_max_size = 0x10000,
        .tcp_rx_buf_max_size = 0x10000,
        .udp_tx_buf_size = 0x200,
        .udp_rx_buf_size = 0x200,
        .sb_efficiency = 2,
        .num_bsd_sessions = 3,
        .bsd_service_type = BsdServiceType_Auto,
    };
    Result rcSock = socketInitialize(&cfg);
    nxlog("appInit: socket rc=0x%x", (unsigned)rcSock);

    // curlのHTTPSはlibnxのsslサービスを使う。smの接続が生きているうちに初期化しておく
    Result rcSsl = sslInitialize(3);
    nxlog("appInit: ssl rc=0x%x", (unsigned)rcSsl);
    g_sslOk = R_SUCCEEDED(rcSsl);
    // smExit() はここで呼ばない(curlが後からsslを初期化するときにsmが必要なため)。終了時に呼ぶ
}

void __appExit(void) {
    if (g_sslOk) sslExit();
    socketExit();
    if (g_nifmOk) nifmExit();
    csrngExit();
    nsExit();
    pminfoExit();
    pmdmntExit();
    fsdevUnmountAll();
    fsExit();
    smExit();
}
}

// ---- 設定 ----
// config.txt
//   1行目: DiscordのWebhook URL(必須)
//   2行目以降(省略可):
//     limit=10            送信できるファイルの上限(MB)
//     trim=1              上限を超える動画の最初を削って送る(0で無効)
//     album=sdmc:/...     Albumフォルダの場所(通常は書かない)
//     auto=1              写真・動画の自動送信(0でオフ。ゲームの開始/終了の通知は続く)
//     notify=1            ゲームの開始/終了の通知(0でオフ)
//     footer=1            Discordの通知の下に「NXShot v...」を出す(0でオフ)
//     icon=1              通知にゲームのアイコンを付ける(0: なし 1: 左上(ゲーム名の横) 2: 右上(大きめ) 3: 一番下(フッターの横))
//     hook=名前|URL       送信先を足す(何行でも。1行目も「名前|URL」と書ける)
//     use=1               いま使う送信先(1行目が1、hook= の行が2, 3, ...)
struct Config {
    std::string hook;      // いま使う送信先のURL(hooks と use から決まる)
    std::string hookName;  // その名前(無ければ空)
    std::string album;
    long limitMB = DEFAULT_LIMIT_MB;
    bool trim = true;
    bool autoSend = true;
    bool notify = true;
    bool footer = true;
    int icon = 1;  // ゲームのアイコン: 0 なし / 1 左上 / 2 右上 / 3 フッター
};

static std::string trimLine(const char *line) {
    std::string s(line);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && s[i] == ' ') i++;
    return s.substr(i);
}

// 「名前|URL」か「URL」を、名前とURLに分ける
static void parseHookEntry(const std::string &s, std::string &name, std::string &url) {
    size_t p = s.find('|');
    if (s.compare(0, 8, "https://") == 0 || p == std::string::npos) {
        name.clear();
        url = s;
        return;
    }
    name = trimLine(s.substr(0, p).c_str());
    url = trimLine(s.substr(p + 1).c_str());
}

static bool loadConfig(Config &c) {
    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) return false;
    char line[512];
    int n = 0;
    bool ok = false;
    std::vector<std::pair<std::string, std::string>> hooks;  // {名前, URL}。1行目が先頭
    long use = 1;
    while (n < 60 && fgets(line, sizeof(line), f)) {
        n++;
        std::string t = trimLine(line);
        if (n == 1) {
            std::string nm, url;
            parseHookEntry(t, nm, url);
            hooks.push_back({nm, url});
            ok = true;
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
            std::string nm, url;
            parseHookEntry(trimLine(t.c_str() + 5), nm, url);
            if (url.compare(0, 8, "https://") == 0) hooks.push_back({nm, url});
        } else if (t.compare(0, 4, "use=") == 0) {
            use = atol(trimLine(t.c_str() + 4).c_str());
        } else if (t.compare(0, 6, "album=") == 0) {
            c.album = trimLine(t.c_str() + 6);
        } else if (c.album.empty() && t.compare(0, 6, "sdmc:/") == 0) {
            c.album = t;  // 古い書き方(2行目にAlbumの場所だけ)
        }
    }
    fclose(f);
    if (!ok) return false;
    if (c.album.empty()) c.album = DEFAULT_ALBUM;
    while (c.album.size() > 1 && c.album.back() == '/') c.album.pop_back();
    if (use < 1 || use > (long)hooks.size()) use = 1;
    c.hookName = hooks[(size_t)(use - 1)].first;
    c.hook = hooks[(size_t)(use - 1)].second;
    return c.hook.compare(0, 8, "https://") == 0;
}

// ---- Switchの情報 ----
static bool getRunningProgramId(u64 &programId) {
    u64 pid = 0;
    if (R_FAILED(pmdmntGetApplicationProcessId(&pid))) return false;
    return R_SUCCEEDED(pminfoGetProgramId(&programId, pid));
}

// iconOut があれば、ゲームのアイコン(JPEG)も取り出す(同じデータに入っている)
static bool getTitleName(u64 programId, std::string &name, std::shared_ptr<const std::string> *iconOut = nullptr) {
    static NsApplicationControlData data;  // 静的確保でスタックを節約
    u64 actualSize = 0;
    if (R_FAILED(nsGetApplicationControlData(NsApplicationControlSource_Storage, programId, &data,
                                             sizeof(data), &actualSize)))
        return false;
    if (actualSize < sizeof(data.nacp)) return false;

    NacpLanguageEntry *entry = nullptr;
    if (R_FAILED(nacpGetLanguageEntry(&data.nacp, &entry)) || !entry || entry->name[0] == '\0') {
        for (int i = 0; i < 16; i++) {
            if (data.nacp.lang[i].name[0] != '\0') { entry = &data.nacp.lang[i]; break; }
        }
    }
    if (!entry) return false;
    name.assign(entry->name);
    if (iconOut) {
        iconOut->reset();
        size_t isz = (size_t)actualSize - sizeof(data.nacp);
        if (isz > sizeof(data.icon)) isz = sizeof(data.icon);
        // JPEGの目印(FF D8)があるときだけ使う
        if (isz > 4 && data.icon[0] == 0xFF && data.icon[1] == 0xD8)
            *iconOut = std::make_shared<const std::string>(reinterpret_cast<const char *>(data.icon), isz);
    }
    return !name.empty();
}

static u64 nowSec() { return armTicksToNs(armGetSystemTick()) / 1000000000ULL; }

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
                else out += (char)c;  // UTF-8はそのまま
        }
    }
    return out;
}

static std::string fmtDuration(u64 sec) {
    unsigned long long h = sec / 3600, m = (sec % 3600) / 60, s = sec % 60;
    char b[64];
    if (h > 0) snprintf(b, sizeof(b), "%llu時間%llu分", h, m);
    else if (m > 0) snprintf(b, sizeof(b), "%llu分", m);
    else snprintf(b, sizeof(b), "%llu秒", s);
    return b;
}

// ---- ネットワーク / curl の準備 ----
static bool g_curlReady = false;
static int g_curlTries = 0;

// Wi-Fiにつながっているか(起動直後やスリープ復帰直後はまだつながっていない)
static bool networkUp() {
    if (!g_nifmOk) return true;  // nifmが使えないときは判断できないので試す
    NifmInternetConnectionType type;
    u32 strength = 0;
    NifmInternetConnectionStatus status;
    if (R_FAILED(nifmGetInternetConnectionStatus(&type, &strength, &status))) return false;
    return status == NifmInternetConnectionStatus_Connected;
}

static void initCurl() {
    if (g_curlReady || g_curlTries >= 6) return;
    g_curlTries++;
    nxlog("curl: global_init try %d", g_curlTries);
    // HTTPSにはSSLの初期化が必須。SSLなし(CURL_GLOBAL_NOTHING)では通信が固まるので使わない
    CURLcode rc = curl_global_init(CURL_GLOBAL_DEFAULT);
    nxlog("curl: global_init=%d (%s)", (int)rc, curl_easy_strerror(rc));
    if (rc == CURLE_OK) {
        g_curlReady = true;
        curl_version_info_data *vi = curl_version_info(CURLVERSION_NOW);
        if (vi) nxlog("curl: version=%s ssl=%s", vi->version ? vi->version : "?", vi->ssl_version ? vi->ssl_version : "none");
        return;
    }
    if (g_curlTries >= 6) nxlog("curl: 初期化をあきらめました。通信できません");
}

// ---- Discord(HTTPS) ----
static char g_errbuf[CURL_ERROR_SIZE];

static size_t discardCb(char *, size_t size, size_t nmemb, void *) { return size * nmemb; }

static void setCommon(CURL *c, const std::string &url) {
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_USERAGENT, "NXDiscord/2.0");
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

// 戻り値: HTTPステータス。通信そのものに失敗したら -1
static long postJson(const std::string &url, const std::string &json) {
    if (!g_curlReady) return -1;
    nxlog("postJson: easy_init");
    CURL *c = curl_easy_init();
    nxlog("postJson: easy_init=%p", (void *)c);
    if (!c) return -1;
    struct curl_slist *h = curl_slist_append(nullptr, "Content-Type: application/json");
    setCommon(c, url);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, json.c_str());
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)json.size());
    long code = -1;
    nxlog("postJson: perform");
    CURLcode rc = curl_easy_perform(c);
    nxlog("postJson: perform rc=%d", (int)rc);
    if (rc == CURLE_OK) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    else nxlog("postJson: curl error %d (%s) %s", (int)rc, curl_easy_strerror(rc), g_errbuf);
    if (rc == CURLE_OK && code >= 300) nxlog("postJson: http %ld", code);
    curl_slist_free_all(h);
    curl_easy_cleanup(c);
    return code;
}

// アイコン(JPEG)を添えて、JSONの通知を送る。icon が空なら、ふつうのJSON送信
static long postJsonWithIcon(const std::string &url, const std::string &json, const std::shared_ptr<const std::string> &icon) {
    if (!icon || icon->empty()) return postJson(url, json);
    if (!g_curlReady) return -1;
    CURL *c = curl_easy_init();
    if (!c) return -1;
    setCommon(c, url);
    struct curl_slist *h = curl_slist_append(nullptr, "Expect:");
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_mime *m = curl_mime_init(c);
    curl_mimepart *p = curl_mime_addpart(m);
    curl_mime_name(p, "payload_json");
    curl_mime_data(p, json.c_str(), json.size());
    curl_mime_type(p, "application/json");
    p = curl_mime_addpart(m);
    curl_mime_name(p, "files[0]");
    curl_mime_data(p, icon->data(), icon->size());
    curl_mime_filename(p, "icon.jpg");
    curl_mime_type(p, "image/jpeg");
    curl_easy_setopt(c, CURLOPT_MIMEPOST, m);
    long code = -1;
    CURLcode rc = curl_easy_perform(c);
    if (rc == CURLE_OK) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    else nxlog("postJsonWithIcon: curl error %d (%s) %s", (int)rc, curl_easy_strerror(rc), g_errbuf);
    if (rc == CURLE_OK && code >= 300) nxlog("postJsonWithIcon: http %ld", code);
    curl_mime_free(m);
    curl_slist_free_all(h);
    curl_easy_cleanup(c);
    return code;
}

static long postShot(const std::string &url, const std::string &payload, const std::string &path,
                     const char *attachName, const char *mime, long timeoutSec,
                     const std::shared_ptr<const std::string> &icon = nullptr) {
    if (!g_curlReady) return -1;
    nxlog("postShot: easy_init (%s)", path.c_str());
    CURL *c = curl_easy_init();
    if (!c) { nxlog("postShot: easy_init failed"); return -1; }
    setCommon(c, url);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, timeoutSec);
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
    if (icon && !icon->empty()) {  // ゲームのアイコン(通知の中で attachment://icon.jpg として使う)
        p = curl_mime_addpart(m);
        curl_mime_name(p, "files[1]");
        curl_mime_data(p, icon->data(), icon->size());
        curl_mime_filename(p, "icon.jpg");
        curl_mime_type(p, "image/jpeg");
    }
    curl_easy_setopt(c, CURLOPT_MIMEPOST, m);

    long code = -1;
    nxlog("postShot: perform");
    CURLcode rc = curl_easy_perform(c);
    if (rc == CURLE_OK) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    else nxlog("postShot: curl error %d (%s) %s", (int)rc, curl_easy_strerror(rc), g_errbuf);
    nxlog("postShot: http %ld", code);
    curl_mime_free(m);
    curl_slist_free_all(h);
    curl_easy_cleanup(c);
    return code;
}

// 送信待ちの通知(ネットワークが無いときのために溜めておく)
struct QItem {
    std::string json;
    std::shared_ptr<const std::string> icon;  // 通知に添えるゲームのアイコン(無ければ空)
};
static std::vector<QItem> g_queue;

static std::string g_curTitle;  // いま遊んでいるゲーム名(スクショの説明に使う)
static std::shared_ptr<const std::string> g_curIcon;  // いま遊んでいるゲームのアイコン(JPEG)。取れなければ空
static bool g_footerOn = true;  // 通知の下に「NXShot v...」を出すか(config.txt の footer=)
static int g_iconMode = 1;      // ゲームのアイコン: 0 なし / 1 左上 / 2 右上 / 3 フッター(config.txt の icon=)

static bool iconActive() { return g_curIcon && g_iconMode != 0; }

// 通知の中の「アイコンの指定」と「下の小さい文字」。アイコンの置き場所は icon= で決まる
//  1: 名前の横(左上)  2: 右上(大きめ)  3: 一番下(フッターの横)
static std::string footerAndIconJson() {
    std::string j;
    if (iconActive() && g_iconMode == 1)
        j += ",\"author\":{\"name\":\"" + jsonEscape(g_curTitle.empty() ? std::string("NXShot") : g_curTitle) +
             "\",\"icon_url\":\"attachment://icon.jpg\"}";
    if (iconActive() && g_iconMode == 2) j += ",\"thumbnail\":{\"url\":\"attachment://icon.jpg\"}";
    if (iconActive() && g_iconMode == 3) {
        std::string text = g_footerOn ? std::string("NXShot v" VERSION) : (g_curTitle.empty() ? std::string("NXShot") : g_curTitle);
        j += ",\"footer\":{\"text\":\"" + jsonEscape(text) + "\",\"icon_url\":\"attachment://icon.jpg\"}";
    } else if (g_footerOn) {
        j += ",\"footer\":{\"text\":\"NXShot v" VERSION "\"}";
    }
    return j;
}

// 通知の文の先頭にゲーム名を付ける。名前つきのアイコン(左上)を出すときは、重ならないよう付けない
static std::string withTitle(const std::string &msg) {
    if (g_curTitle.empty() || (iconActive() && g_iconMode == 1)) return msg;
    return msg.empty() ? g_curTitle : g_curTitle + "\n" + msg;
}

static std::string embedJson(const char *title, const std::string &desc, int color) {
    std::string j = std::string("{\"username\":\"") + HOOK_NAME + "\",\"embeds\":[{\"title\":\"" + title + "\"";
    if (!desc.empty()) j += ",\"description\":\"" + jsonEscape(desc) + "\"";
    j += ",\"color\":" + std::to_string(color) + footerAndIconJson() + "}]}";
    return j;
}

static void enqueue(const std::string &json) {
    if (g_queue.size() >= 20) g_queue.erase(g_queue.begin());
    QItem q;
    q.json = json;
    if (json.find("attachment://icon.jpg") != std::string::npos) q.icon = g_curIcon;  // この時点のゲームのアイコン
    g_queue.push_back(q);
}

// 成功(2xx)か、再送しても無駄(4xx。ただし429は除く)なら、その通知は終わりにする
static bool isFinal(long code) { return (code >= 200 && code < 300) || (code >= 400 && code < 500 && code != 429); }

static void flushQueue(const Config &cfg) {
    for (int i = 0; i < 3 && !g_queue.empty(); i++) {
        long code = postJsonWithIcon(cfg.hook, g_queue.front().json, g_queue.front().icon);
        if (!isFinal(code)) break;
        g_queue.erase(g_queue.begin());
    }
}

// ---- スクリーンショット ----
// Albumの構造: <album>/<年>/<月>/<日>/<日時>-<ハッシュ>.jpg (動画は .mp4)
static std::string g_lastSent;
static bool g_albumInit = false;
static std::string g_pendingName;
static std::string g_failName;  // 送信に失敗したファイル
static int g_failCount = 0;
static long g_pendingSize = -1;

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

static bool newestSub(const std::string &path, std::string &out) {
    std::vector<std::string> dirs, files;
    listDir(path, dirs, files);
    bool found = false;
    for (auto &n : dirs) {
        bool digits = !n.empty();
        for (char c : n) if (c < '0' || c > '9') { digits = false; break; }
        if (!digits) continue;
        if (!found || n > out) { out = n; found = true; }
    }
    return found;
}

static bool scanNewestDay(const std::string &album, std::string &dayPath, std::vector<std::string> &media) {
    std::string y, m, d;
    if (!newestSub(album, y)) return false;
    std::string p = album + "/" + y;
    if (!newestSub(p, m)) return false;
    p += "/" + m;
    if (!newestSub(p, d)) return false;
    dayPath = p + "/" + d;
    std::vector<std::string> dirs, files;
    listDir(dayPath, dirs, files);
    for (auto &f : files) if (mediaKind(f) != MK_NONE) media.push_back(f);
    return true;
}


static void checkScreenshots(const Config &cfg) {
    std::string dayPath;
    std::vector<std::string> names;
    if (!scanNewestDay(cfg.album, dayPath, names)) return;

    if (!cfg.autoSend) {
        // 自動送信オフ: 送らない。あとでオンにしたとき、オフの間に撮ったものが一気に送られないよう、印だけ進める
        for (auto &n : names) if (n > g_lastSent) g_lastSent = n;
        g_albumInit = true;
        g_pendingName.clear();
        return;
    }

    if (!g_albumInit) {  // 起動時にすでにある写真は送らない
        for (auto &n : names) if (n > g_lastSent) g_lastSent = n;
        g_albumInit = true;
        return;
    }

    std::string next;
    for (auto &n : names) if (n > g_lastSent && (next.empty() || n < next)) next = n;
    if (next.empty()) { g_pendingName.clear(); return; }

    std::string full = dayPath + "/" + next;
    struct stat st;
    if (stat(full.c_str(), &st) != 0 || st.st_size <= 0) return;
    bool isVideo = mediaKind(next) == MK_VIDEO;

    // 書き込み中に送らないよう、前回とサイズが同じになってから送る
    if (g_pendingName != next || g_pendingSize != (long)st.st_size) {
        g_pendingName = next;
        g_pendingSize = (long)st.st_size;
        return;
    }

    std::string sendPath = full;
    std::string note;        // 説明欄に足す文
    bool trimmed = false;
    long long limitBytes = (long long)cfg.limitMB * 1048576LL - 65536;
    if ((long long)st.st_size > limitBytes) {
        // 上限を超えている。動画なら、最初のほうを削って収められないか試す
        mp4trim::Info ti;
        mp4trim::Result tr = mp4trim::ERR_FORMAT;
        if (isVideo && cfg.trim) {
            nxlog("trim: 開始 %s (%lld bytes, 上限 %lld)", next.c_str(), (long long)st.st_size, limitBytes);
            tr = mp4trim::trim(full.c_str(), TRIM_TMP_PATH, limitBytes, &ti);
            nxlog("trim: result=%d new=%lld cut=%.2fs", (int)tr, (long long)ti.newSize, ti.cutSeconds);
        }
        if (tr == mp4trim::OK) {
            sendPath = TRIM_TMP_PATH;
            trimmed = true;
            char nb[200];
            snprintf(nb, sizeof(nb), "容量の都合で、最初の%.1f秒を削って送りました(%.1fMB→%.1fMB)",
                     ti.cutSeconds, ti.origSize / 1048576.0, ti.newSize / 1048576.0);
            note = nb;
        } else {
            remove(TRIM_TMP_PATH);
            char msg[300];
            if (isVideo && cfg.trim)
                snprintf(msg, sizeof(msg), "ファイルが大きいため送れませんでした(%.1fMB / 上限%ldMB)。削って収めることもできませんでした。",
                         st.st_size / 1048576.0, cfg.limitMB);
            else
                snprintf(msg, sizeof(msg), "ファイルが大きいため送れませんでした(%.1fMB / 上限%ldMB)。config.txtのlimit=で上限を変えられます。",
                         st.st_size / 1048576.0, cfg.limitMB);
            nxlog("media: 上限超え %s (%lld bytes, limit=%ldMB)", next.c_str(), (long long)st.st_size, cfg.limitMB);
            std::string d = withTitle(msg);
            enqueue(embedJson(isVideo ? "🎬 動画(未送信)" : "📸 スクリーンショット(未送信)", d, 10070709));
            g_lastSent = next;
            g_pendingName.clear();
            return;
        }
    }

    std::string payload = std::string("{\"username\":\"") + HOOK_NAME + "\",\"embeds\":[{\"title\":\"" +
                          (isVideo ? "🎬 動画" : "📸 スクリーンショット") + "\"";
    std::string desc = withTitle("");
    if (!note.empty()) desc += (desc.empty() ? "" : "\n") + note;
    if (!desc.empty()) payload += ",\"description\":\"" + jsonEscape(desc) + "\"";
    payload += ",\"color\":15277667";
    if (!isVideo) payload += ",\"image\":{\"url\":\"attachment://screenshot.jpg\"}";
    payload += footerAndIconJson();
    payload += "}]}";

    // 大きいファイルほど時間がかかるので、送信の制限時間を長めにする
    struct stat sst;
    long long sendSize = (stat(sendPath.c_str(), &sst) == 0) ? (long long)sst.st_size : (long long)st.st_size;
    long timeoutSec = 60 + (long)(sendSize / 1048576) * 8;
    long code = postShot(cfg.hook, payload, sendPath, isVideo ? "video.mp4" : "screenshot.jpg",
                         isVideo ? "video/mp4" : "image/jpeg", timeoutSec,
                         iconActive() ? g_curIcon : std::shared_ptr<const std::string>());
    if (trimmed) remove(TRIM_TMP_PATH);  // 一時ファイルは必ず消す
    if (code >= 200 && code < 300) {
        g_lastSent = next;
        g_pendingName.clear();
        g_failName.clear();
        g_failCount = 0;
        return;
    }

    // 失敗。サーバーに断られた(4xx)なら再送しても無駄。通信エラー等は3回までやり直す
    bool refused = code >= 400 && code < 500 && code != 429;
    if (g_failName != next) { g_failName = next; g_failCount = 0; }
    g_failCount++;
    if (refused || g_failCount >= 3) {
        char msg[320];
        if (code == 413)
            snprintf(msg, sizeof(msg), "Discordのサーバーの上限を超えていて送れませんでした(%.1fMB)。config.txtのlimit=を小さくすると、この通知だけで済みます。",
                     st.st_size / 1048576.0);
        else if (code > 0)
            snprintf(msg, sizeof(msg), "送信に失敗しました(http %ld, %.1fMB)。", code, st.st_size / 1048576.0);
        else
            snprintf(msg, sizeof(msg), "通信に失敗したため送れませんでした(%.1fMB)。", st.st_size / 1048576.0);
        nxlog("media: 送信をあきらめました %s (http %ld)", next.c_str(), code);
        std::string d = withTitle(msg);
        enqueue(embedJson(isVideo ? "🎬 動画(送信失敗)" : "📸 スクリーンショット(送信失敗)", d, 10038562));
        g_lastSent = next;
        g_pendingName.clear();
        g_failName.clear();
        g_failCount = 0;
    }
}

static std::string albumReport(const Config &cfg) {
    std::string dayPath;
    std::vector<std::string> names;
    if (!scanNewestDay(cfg.album, dayPath, names))
        return "Albumを読めません。設定→データ管理→スクリーンショットと動画の管理→保存先がSDカードか確認してください。\n場所: " + cfg.album;
    int img = 0, vid = 0;
    for (auto &n : names) { if (mediaKind(n) == MK_VIDEO) vid++; else img++; }
    return "Albumを読めました。最新の日の写真: " + std::to_string(img) + "枚 / 動画: " + std::to_string(vid) + "本\n送信できるファイルの上限: " + std::to_string(cfg.limitMB) + "MB\n写真・動画の自動送信: " + (cfg.autoSend ? "オン" : "オフ") +
           "\nゲームの開始・終了の通知: " + (cfg.notify ? "オン" : "オフ") +
           "\nゲームのアイコン: " + (cfg.icon == 0 ? "なし" : (cfg.icon == 1 ? "左上" : (cfg.icon == 2 ? "右上" : "フッター"))) +
           (cfg.hookName.empty() ? std::string() : "\n送信先: " + cfg.hookName);
}

// ---- オーバーレイからの「この1件を送って」の依頼 ----
// オーバーレイ(Tesla)が OVL_REQ_PATH に、送りたいファイルのパスを1行で書く。
// 常駐版はそれを読んで消し、送って、結果を OVL_RES_PATH に書く(1行目: ok / big / fail / bad、2行目: ファイル名)。
// 自動送信のオン/オフとは関係なく送る。自動送信の「どこまで送ったか」の印は動かさない。
#define OVL_REQ_PATH "sdmc:/config/nxdiscord/ovl_send.txt"
#define OVL_RES_PATH "sdmc:/config/nxdiscord/ovl_result.txt"
#define OVL_READY_PATH "sdmc:/config/nxdiscord/ovl_ready.txt"

static void writeOvlResult(const char *kind, const std::string &name) {
    FILE *f = fopen(OVL_RES_PATH, "w");
    if (!f) return;
    fprintf(f, "%s\n%s\n", kind, name.c_str());
    fclose(f);
}

static void processOverlayRequest(const Config &cfg) {
    FILE *f = fopen(OVL_REQ_PATH, "r");
    if (!f) return;
    char line[640];
    line[0] = 0;
    if (!fgets(line, sizeof(line), f)) line[0] = 0;
    fclose(f);
    remove(OVL_REQ_PATH);  // 先に消す(同じ依頼を二度やらない)
    std::string full = trimLine(line);

    // アルバムの中の jpg / mp4 だけを送る(「..」を含むパスや、ほかの場所のファイルは断る)
    size_t slash = full.rfind('/');
    std::string name = (slash == std::string::npos) ? full : full.substr(slash + 1);
    std::string albumPrefix = cfg.album + "/";
    int kind = mediaKind(name);
    if (full.compare(0, albumPrefix.size(), albumPrefix) != 0 || full.find("..") != std::string::npos || kind == MK_NONE) {
        nxlog("ovl: 依頼を断りました(場所か種類がちがう)");
        writeOvlResult("bad", name);
        return;
    }
    struct stat st;
    if (stat(full.c_str(), &st) != 0 || st.st_size <= 0) {
        nxlog("ovl: ファイルが見つかりません %s", name.c_str());
        writeOvlResult("bad", name);
        return;
    }
    bool isVideo = kind == MK_VIDEO;
    nxlog("ovl: 送信の依頼 %s (%lld bytes)", name.c_str(), (long long)st.st_size);

    std::string sendPath = full;
    std::string note;
    bool trimmed = false;
    long long limitBytes = (long long)cfg.limitMB * 1048576LL - 65536;
    if ((long long)st.st_size > limitBytes) {
        mp4trim::Info ti;
        mp4trim::Result tr = mp4trim::ERR_FORMAT;
        if (isVideo && cfg.trim) tr = mp4trim::trim(full.c_str(), TRIM_TMP_PATH, limitBytes, &ti);
        if (tr != mp4trim::OK) {
            remove(TRIM_TMP_PATH);
            nxlog("ovl: 上限超えで送れません %s", name.c_str());
            writeOvlResult("big", name);
            return;
        }
        sendPath = TRIM_TMP_PATH;
        trimmed = true;
        char nb[200];
        snprintf(nb, sizeof(nb), "容量の都合で、最初の%.1f秒を削って送りました(%.1fMB→%.1fMB)", ti.cutSeconds,
                 ti.origSize / 1048576.0, ti.newSize / 1048576.0);
        note = nb;
    }

    std::string payload = std::string("{\"username\":\"") + HOOK_NAME + "\",\"embeds\":[{\"title\":\"" +
                          (isVideo ? "🎬 動画" : "📸 スクリーンショット") + "\"";
    std::string desc = withTitle("");
    if (!note.empty()) desc += (desc.empty() ? "" : "\n") + note;
    if (!desc.empty()) payload += ",\"description\":\"" + jsonEscape(desc) + "\"";
    payload += ",\"color\":15277667";
    if (!isVideo) payload += ",\"image\":{\"url\":\"attachment://screenshot.jpg\"}";
    payload += footerAndIconJson();
    payload += "}]}";

    struct stat sst;
    long long sendSize = (stat(sendPath.c_str(), &sst) == 0) ? (long long)sst.st_size : (long long)st.st_size;
    long timeoutSec = 60 + (long)(sendSize / 1048576) * 8;
    long code = postShot(cfg.hook, payload, sendPath, isVideo ? "video.mp4" : "screenshot.jpg",
                         isVideo ? "video/mp4" : "image/jpeg", timeoutSec,
                         iconActive() ? g_curIcon : std::shared_ptr<const std::string>());
    if (trimmed) remove(TRIM_TMP_PATH);
    nxlog("ovl: 送信 http %ld %s", code, name.c_str());
    writeOvlResult((code >= 200 && code < 300) ? "ok" : "fail", name);
}

// ---- メイン ----
int main(int argc, char **argv) {
    nxlog("main: start v" VERSION);
    remove(TRIM_TMP_PATH);  // 前回の一時ファイルが残っていたら消す
    remove(OVL_REQ_PATH);   // 前の起動で残った依頼は、やらない
    remove(OVL_RES_PATH);
    {   // オーバーレイが「この常駐版は、送る依頼に対応している」と分かるための印
        FILE *rf = fopen(OVL_READY_PATH, "w");
        if (rf) { fputs("1\n", rf); fclose(rf); }
    }
    bool cfgLogged = false;
    bool netLogged = false, lastNet = false;

    bool bootDone = false;
    u64 curPid = 0, startSec = 0;
    bool startPending = false;
    int titleTries = 0;

    while (true) {
        Config cfg;
        bool cfgOk = loadConfig(cfg);
        if (!cfgOk && !cfgLogged) {
            nxlog("config: 読めません(%s にhttps://で始まるURLを1行目に書く)", CONFIG_PATH);
            cfgLogged = true;
        }
        if (cfgOk) {
            g_footerOn = cfg.footer;
            g_iconMode = cfg.icon;
            u64 pid = 0;
            if (!getRunningProgramId(pid)) pid = 0;

            if (pid != curPid) {
                if (curPid != 0 && !startPending && cfg.notify) {
                    enqueue(embedJson("🏁 プレイ終了", withTitle("プレイ時間: " + fmtDuration(nowSec() - startSec)), 15158332));
                }
                nxlog("game: pid=%llX", (unsigned long long)pid);
                curPid = pid;
                g_curTitle.clear();
                g_curIcon.reset();
                titleTries = 0;
                startPending = (pid != 0);
                startSec = nowSec();
            }
            if (startPending) {
                if (getTitleName(curPid, g_curTitle, &g_curIcon) || ++titleTries >= 3) {
                    if (g_curTitle.empty()) {
                        char b[24];
                        snprintf(b, sizeof(b), "%016llX", (unsigned long long)curPid);
                        g_curTitle = b;
                    }
                    if (cfg.notify) enqueue(embedJson("🎮 ゲームを開始", withTitle(""), 3447003));
                    startPending = false;
                }
            }

            bool net = networkUp();
            if (!netLogged || net != lastNet) {
                nxlog("network: %s", net ? "接続" : "未接続(つながるまで待ちます)");
                netLogged = true;
                lastNet = net;
            }
            if (net) initCurl();
            if (!net || !g_curlReady) {
                // 通信できないときも、依頼は受け取って、できないと返す(オーバーレイが待ち続けないように)
                FILE *qf = fopen(OVL_REQ_PATH, "r");
                if (qf) {
                    fclose(qf);
                    remove(OVL_REQ_PATH);
                    writeOvlResult("net", "");
                }
                svcSleepThread(POLL_NS);
                continue;
            }

            if (!bootDone) {
                // 起動の確認。これがチャンネルに届けば、通信とWebhookは正常
                std::string rep = albumReport(cfg);
                nxlog("album: %s", rep.c_str());
                long code = postJson(cfg.hook, embedJson("🟢 NXShot 起動", rep, 5763719));
                nxlog("boot post: %ld", code);
                if (isFinal(code)) bootDone = true;
            }

            flushQueue(cfg);
            checkScreenshots(cfg);
            processOverlayRequest(cfg);
        }
        svcSleepThread(POLL_NS);
    }
    return 0;
}
