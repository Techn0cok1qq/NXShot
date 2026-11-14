// NXShot オーバーレイ (Tesla .ovl)
//
// Tesla メニューから開いて、ゲームを止めずに NXShot の設定を変える。
// 常駐版(sysmodule)は config.txt を5秒ごとに読み直すので、ここで書き換えた内容は、
// 数秒で反映される(常駐版の変更は不要)。
//
//   ・自動送信(auto=) / ゲームの通知(notify=) / フッター(footer=) / 動画を削る(trim=)
//   ・上限MB(limit=) / ゲームのアイコン位置(icon=) / 送信先(use=)
//   ・常駐版が動いているかの表示
//
// config.txt は、書き換えたい行だけを直す(1行目のWebhook、hook=、album=、コメントなどは、そのまま残す)。
// Webhook URL は、画面にも、メモリにも、取り込まない(送信先は名前だけ読む)。
#define TESLA_INIT_IMPL
#include <tesla.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#define OVL_VERSION "1.0.0"

#define CONFIG_DIR  "sdmc:/config/nxdiscord"
#define CONFIG_PATH CONFIG_DIR "/config.txt"
#define CONFIG_TMP  CONFIG_DIR "/config.txt.ovltmp"
#define CONFIG_BAK  CONFIG_DIR "/config.txt.ovlbak"

#define SYSMODULE_TID 0x4200000000000A7EULL
#define MAX_CFG_LINES 58  // 常駐版は60行目までしか読まない。それを超えて足さない

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
        }
    }
    fclose(f);
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

// ---- 画面 ----
class MainGui : public tsl::Gui {
public:
    MainGui() {}

    tsl::elm::Element *createUI() override {
        auto *frame = new tsl::elm::OverlayFrame("NXShot", "設定  v" OVL_VERSION);
        auto *list = new tsl::elm::List();

        loadCfg(m_cfg);

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
        m_limit = new tsl::elm::ListItem("上限(MB)  Aで切替");
        m_limit->setValue(std::to_string(m_cfg.limitMB) + " MB");
        m_limit->setClickListener([this](u64 keys) {
            if (!(keys & HidNpadButton_A)) return false;
            static const long P[] = {5, 10, 25, 50, 100};
            long next = P[0];
            for (long p : P) {
                if (p > m_cfg.limitMB) { next = p; break; }
            }
            if (setKey("limit", std::to_string(next))) {
                m_cfg.limitMB = next;
                m_limit->setValue(std::to_string(next) + " MB");
                say("上限を変えました");
            } else {
                say("保存できませんでした");
            }
            return true;
        });
        list->addItem(m_limit);

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
    tsl::elm::ListItem *m_status = nullptr, *m_msg = nullptr, *m_hook = nullptr, *m_limit = nullptr, *m_icon = nullptr;
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
