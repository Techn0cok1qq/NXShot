// NXDiscord sysmodule (PC不要版)
// ・Switch単体で、DiscordのWebhookへ直接HTTPS送信する
//   - ゲームの開始 / 終了(プレイ時間つき)の通知
//   - SDカードのAlbumに増えた新しいスクリーンショット(jpg)の自動投稿
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
#include <unistd.h>
#include <cstdarg>

#define INNER_HEAP_SIZE 0x400000  // 4MB(TLS+アップロード用。2MBだと足りない恐れがある)
#define CONFIG_PATH "sdmc:/config/nxdiscord/config.txt"
#define DEFAULT_ALBUM "sdmc:/Nintendo/Album"
#define POLL_NS 5'000'000'000ULL  // 5秒ごとに確認
#define MAX_SHOT_BYTES (8 * 1024 * 1024)
#define VERSION "1.0.0"
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
//   2行目: Albumフォルダの場所(省略可。通常は書かない)
struct Config {
    std::string hook;
    std::string album;
};

static std::string trimLine(const char *line) {
    std::string s(line);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && s[i] == ' ') i++;
    return s.substr(i);
}

static bool loadConfig(Config &c) {
    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) return false;
    char l1[512] = {0}, l2[256] = {0};
    bool ok = fgets(l1, sizeof(l1), f) != nullptr;
    if (ok) fgets(l2, sizeof(l2), f);
    fclose(f);
    if (!ok) return false;
    c.hook = trimLine(l1);
    c.album = trimLine(l2);
    if (c.album.empty()) c.album = DEFAULT_ALBUM;
    while (c.album.size() > 1 && c.album.back() == '/') c.album.pop_back();
    return c.hook.compare(0, 8, "https://") == 0;
}

// ---- Switchの情報 ----
static bool getRunningProgramId(u64 &programId) {
    u64 pid = 0;
    if (R_FAILED(pmdmntGetApplicationProcessId(&pid))) return false;
    return R_SUCCEEDED(pminfoGetProgramId(&programId, pid));
}

static bool getTitleName(u64 programId, std::string &name) {
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

static long postShot(const std::string &url, const std::string &payload, const std::string &path) {
    if (!g_curlReady) return -1;
    nxlog("postShot: easy_init (%s)", path.c_str());
    CURL *c = curl_easy_init();
    if (!c) { nxlog("postShot: easy_init failed"); return -1; }
    setCommon(c, url);
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
    curl_mime_filename(p, "screenshot.jpg");
    curl_mime_type(p, "image/jpeg");
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
static std::vector<std::string> g_queue;

static std::string embedJson(const char *title, const std::string &desc, int color) {
    std::string j = std::string("{\"username\":\"") + HOOK_NAME + "\",\"embeds\":[{\"title\":\"" + title + "\"";
    if (!desc.empty()) j += ",\"description\":\"" + jsonEscape(desc) + "\"";
    j += ",\"color\":" + std::to_string(color) + "}]}";
    return j;
}

static void enqueue(const std::string &json) {
    if (g_queue.size() >= 20) g_queue.erase(g_queue.begin());
    g_queue.push_back(json);
}

// 成功(2xx)か、再送しても無駄(4xx。ただし429は除く)なら、その通知は終わりにする
static bool isFinal(long code) { return (code >= 200 && code < 300) || (code >= 400 && code < 500 && code != 429); }

static void flushQueue(const Config &cfg) {
    for (int i = 0; i < 3 && !g_queue.empty(); i++) {
        long code = postJson(cfg.hook, g_queue.front());
        if (!isFinal(code)) break;
        g_queue.erase(g_queue.begin());
    }
}

// ---- スクリーンショット ----
// Albumの構造: <album>/<年>/<月>/<日>/<日時>-<ハッシュ>.jpg
static std::string g_lastSent;
static bool g_albumInit = false;
static std::string g_pendingName;
static long g_pendingSize = -1;

static bool endsWithJpg(const std::string &n) {
    if (n.size() < 4) return false;
    std::string e = n.substr(n.size() - 4);
    for (auto &c : e) c = (char)tolower((unsigned char)c);
    return e == ".jpg";
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

static bool scanNewestDay(const std::string &album, std::string &dayPath, std::vector<std::string> &jpgs) {
    std::string y, m, d;
    if (!newestSub(album, y)) return false;
    std::string p = album + "/" + y;
    if (!newestSub(p, m)) return false;
    p += "/" + m;
    if (!newestSub(p, d)) return false;
    dayPath = p + "/" + d;
    std::vector<std::string> dirs, files;
    listDir(dayPath, dirs, files);
    for (auto &f : files) if (endsWithJpg(f)) jpgs.push_back(f);
    return true;
}

static std::string g_curTitle;  // いま遊んでいるゲーム名(スクショの説明に使う)

static void checkScreenshots(const Config &cfg) {
    std::string dayPath;
    std::vector<std::string> names;
    if (!scanNewestDay(cfg.album, dayPath, names)) return;

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
    if (st.st_size > MAX_SHOT_BYTES) { g_lastSent = next; return; }

    // 書き込み中に送らないよう、前回とサイズが同じになってから送る
    if (g_pendingName != next || g_pendingSize != (long)st.st_size) {
        g_pendingName = next;
        g_pendingSize = (long)st.st_size;
        return;
    }

    std::string payload = std::string("{\"username\":\"") + HOOK_NAME + "\",\"embeds\":[{\"title\":\"📸 スクリーンショット\"";
    if (!g_curTitle.empty()) payload += ",\"description\":\"" + jsonEscape(g_curTitle) + "\"";
    payload += ",\"color\":15277667,\"image\":{\"url\":\"attachment://screenshot.jpg\"}}]}";

    long code = postShot(cfg.hook, payload, full);
    if (isFinal(code)) {
        g_lastSent = next;
        g_pendingName.clear();
    }
}

static std::string albumReport(const Config &cfg) {
    std::string dayPath;
    std::vector<std::string> names;
    if (!scanNewestDay(cfg.album, dayPath, names))
        return "Albumを読めません。設定→データ管理→スクリーンショットと動画の管理→保存先がSDカードか確認してください。\n場所: " + cfg.album;
    return "Albumを読めました。最新の日の写真: " + std::to_string(names.size()) + "枚";
}

// ---- メイン ----
int main(int argc, char **argv) {
    nxlog("main: start v" VERSION);
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
            u64 pid = 0;
            if (!getRunningProgramId(pid)) pid = 0;

            if (pid != curPid) {
                if (curPid != 0 && !startPending) {
                    enqueue(embedJson("🏁 プレイ終了", g_curTitle + "\nプレイ時間: " + fmtDuration(nowSec() - startSec), 15158332));
                }
                nxlog("game: pid=%llX", (unsigned long long)pid);
                curPid = pid;
                g_curTitle.clear();
                titleTries = 0;
                startPending = (pid != 0);
                startSec = nowSec();
            }
            if (startPending) {
                if (getTitleName(curPid, g_curTitle) || ++titleTries >= 3) {
                    if (g_curTitle.empty()) {
                        char b[24];
                        snprintf(b, sizeof(b), "%016llX", (unsigned long long)curPid);
                        g_curTitle = b;
                    }
                    enqueue(embedJson("🎮 ゲームを開始", g_curTitle, 3447003));
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
            if (!net || !g_curlReady) { svcSleepThread(POLL_NS); continue; }

            if (!bootDone) {
                // 起動の確認。これがチャンネルに届けば、通信とWebhookは正常
                std::string rep = albumReport(cfg);
                nxlog("album: %s", rep.c_str());
                long code = postJson(cfg.hook, embedJson("🟢 NXDiscord 起動", rep, 5763719));
                nxlog("boot post: %ld", code);
                if (isFinal(code)) bootDone = true;
            }

            flushQueue(cfg);
            checkScreenshots(cfg);
        }
        svcSleepThread(POLL_NS);
    }
    return 0;
}
