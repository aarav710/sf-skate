#include "session_ui.h"
#include "net_protocol.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>

namespace sui {

// ---------------------------------------------------------------- ~/.sfskate

std::string settingsPath() {
    const char* home = getenv("HOME");
    if (!home || !*home) return "";
    std::string p = home;
    if (p.back() != '/') p += '/';
    return p + ".sfskate";
}

std::string cleanRelay(const std::string& in) {
    std::string out;
    for (char c : in)
        if (c > ' ' && c < 127 && out.size() < 128) out += c;
    return out;
}

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

Settings loadSettings(const std::string& path) {
    Settings s;
    if (path.empty()) return s;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return s;
    std::string all;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0 && all.size() < 65536) all.append(buf, n);   // (a huge file: its start)
    fclose(f);
    size_t i = 0;
    while (i < all.size()) {
        size_t e = all.find('\n', i);
        if (e == std::string::npos) e = all.size();
        std::string line = all.substr(i, e - i);
        i = e + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        if (k.empty() || k[0] == '#') continue;
        if (k == "name") {
            s.name = netp::sanitizeName(v);
            if (s.name == "skater" && v.find("skater") == std::string::npos) s.name.clear();   // (its stand-in for nothing usable)
        } else if (k == "relay") s.relay = cleanRelay(v);
        else if (s.other.size() < 64) {
            bool ok = true;   // (only lines that write back as they were)
            for (char c : line) ok &= (unsigned char)c >= 0x20 || c == '\t';
            if (ok) s.other.push_back({k, v});
        }
    }
    return s;
}

bool saveSettings(const std::string& path, const Settings& s) {
    if (path.empty()) return false;
    std::string tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    std::string name = s.name.empty() ? "" : netp::sanitizeName(s.name);
    bool ok = fprintf(f, "# SF Skate settings\n") > 0;
    if (!name.empty()) ok &= fprintf(f, "name=%s\n", name.c_str()) > 0;
    std::string relay = cleanRelay(s.relay);
    if (!relay.empty()) ok &= fprintf(f, "relay=%s\n", relay.c_str()) > 0;
    for (auto& kv : s.other)
        if (kv.first != "name" && kv.first != "relay") ok &= fprintf(f, "%s=%s\n", kv.first.c_str(), kv.second.c_str()) > 0;
    ok &= fclose(f) == 0;
    if (!ok || rename(tmp.c_str(), path.c_str()) != 0) { remove(tmp.c_str()); return false; }
    return true;
}

// ---------------------------------------------------------------- text fields

static std::string utf8(uint32_t cp) {
    std::string o;
    if (cp < 0x80) o += (char)cp;
    else if (cp < 0x800) { o += (char)(0xC0 | cp >> 6); o += (char)(0x80 | (cp & 63)); }
    else if (cp < 0x10000) { o += (char)(0xE0 | cp >> 12); o += (char)(0x80 | (cp >> 6 & 63)); o += (char)(0x80 | (cp & 63)); }
    else { o += (char)(0xF0 | cp >> 18); o += (char)(0x80 | (cp >> 12 & 63)); o += (char)(0x80 | (cp >> 6 & 63)); o += (char)(0x80 | (cp & 63)); }
    return o;
}

size_t TextField::maxBytes() const { return kind == NAME ? netp::NAME_MAX : kind == CODE ? netp::CODE_LEN : 128; }

void TextField::type(uint32_t cp) {
    if (cp < 0x20 || cp == 0x7f || cp > 0x10FFFF || (cp >= 0xD800 && cp < 0xE000)) return;
    std::string add;
    if (kind == NAME) add = utf8(cp);
    else if (kind == CODE) {
        if (cp >= 'a' && cp <= 'z') cp -= 'a' - 'A';
        if (!((cp >= 'A' && cp <= 'Z') || (cp >= '0' && cp <= '9'))) return;   // (spaces, dashes: dropped like the relay does)
        add = (char)cp;
    } else {
        if (cp <= ' ' || cp >= 127) return;
        add = (char)cp;
    }
    if (fresh) text.clear(), fresh = false;
    if (text.size() + add.size() > maxBytes()) return;
    text += add;
    edited = true;
}

void TextField::backspace() {
    if (fresh) { fresh = false; edited |= !text.empty(); text.clear(); return; }
    if (text.empty()) return;
    text.pop_back();
    while (!text.empty() && ((unsigned char)text.back() & 0xC0) == 0x80) text.pop_back();
    if (!text.empty() && (unsigned char)text.back() >= 0xC0) text.pop_back();   // (the lead byte of the one just cut)
    edited = true;
}

std::string codeFromPaste(const std::string& clip) {
    std::string out;
    for (char c : clip) {
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) out += c;
    }
    return out.size() > (size_t)netp::CODE_LEN ? out.substr(out.size() - netp::CODE_LEN) : out;
}

void TextField::paste(const std::string& clip) {
    if (kind == CODE) {
        std::string c = codeFromPaste(clip);
        if (c.empty()) return;
        text = c;   // (a pasted code replaces what was typed)
        edited = true;
        fresh = false;
        return;
    }
    // the rest: character by character, stopping at the first line break
    if (fresh && !clip.empty()) text.clear(), fresh = false, edited = true;
    size_t i = 0;
    while (i < clip.size() && clip[i] != '\n' && clip[i] != '\r') {
        unsigned char c = (unsigned char)clip[i];
        int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (!len || i + len > clip.size()) { i++; continue; }
        uint32_t cp = len == 1 ? c : c & (0x7f >> len);
        bool ok = true;
        for (int k = 1; k < len; k++) {
            unsigned char cc = (unsigned char)clip[i + k];
            ok &= (cc >> 6) == 2;
            cp = cp << 6 | (cc & 63);
        }
        if (ok && !((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000))) type(cp);
        i += ok ? len : 1;
    }
}

// ---------------------------------------------------------------- input

namespace {
struct Script {
    std::vector<std::string> steps;
    size_t at = 0;
    int waitFrames = 0;
    double waitUntil = -1, untilStart = -1;
    std::deque<uint32_t> typing;   // `type` characters still to go, one a frame
    bool quit = false, running = false;
    std::function<bool(const std::string&, const std::string&)> game;
};
Script gScript;
TextField* gFocus = nullptr;
bool gTypingFrame = false;
std::vector<int> gKeys;            // injected this frame
bool gCtrl = false;
std::deque<uint32_t> gChars;       // injected characters for this frame
bool gClick = false;
Vector2 gClickAt{};
std::map<std::string, Rectangle> gPlaces;

int keyByName(const std::string& n) {
    static const std::pair<const char*, int> K[] = {{"ENTER", KEY_ENTER}, {"TAB", KEY_TAB}, {"ESCAPE", KEY_ESCAPE}, {"BACKSPACE", KEY_BACKSPACE},
                                                    {"SPACE", KEY_SPACE}, {"UP", KEY_UP}, {"DOWN", KEY_DOWN}, {"LEFT", KEY_LEFT}, {"RIGHT", KEY_RIGHT}};
    for (auto& k : K) if (n == k.first) return k.second;
    if (n.size() == 1 && n[0] >= 'A' && n[0] <= 'Z') return KEY_A + (n[0] - 'A');
    if (n.size() == 1 && n[0] >= 'a' && n[0] <= 'z') return KEY_A + (n[0] - 'a');
    if (n.size() == 1 && n[0] >= '0' && n[0] <= '9') return KEY_ZERO + (n[0] - '0');
    return 0;
}

void stepScript() {
    Script& s = gScript;
    if (!s.running) return;
    if (!s.typing.empty()) {   // a key goes down with each character, as on a keyboard
        uint32_t c = s.typing.front();
        s.typing.pop_front();
        gChars.push_back(c);
        int k = c < 128 ? keyByName(std::string(1, (char)c)) : 0;
        if (k) gKeys.push_back(k);
        if (c == ' ') gKeys.push_back(KEY_SPACE);
        return;
    }
    if (s.waitFrames > 0) { s.waitFrames--; return; }
    if (s.waitUntil > 0) { if (GetTime() < s.waitUntil) return; s.waitUntil = -1; }
    while (s.at < s.steps.size()) {
        const std::string& st = s.steps[s.at];
        size_t sp = st.find(' ');
        std::string cmd = st.substr(0, sp), arg = sp == std::string::npos ? "" : st.substr(sp + 1);
        if (cmd == "until") {
            std::string cond = arg;
            double limit = 60;
            size_t sp2 = arg.rfind(' ');
            if (sp2 != std::string::npos && atof(arg.c_str() + sp2 + 1) > 0) { cond = arg.substr(0, sp2); limit = atof(arg.c_str() + sp2 + 1); }
            if (s.untilStart < 0) s.untilStart = GetTime();
            if (!(s.game && s.game("until", cond))) {
                if (GetTime() - s.untilStart > limit) {
                    TraceLog(LOG_WARNING, "SFSKATE: ui script: gave up waiting for \"%s\" after %.0f s", cond.c_str(), limit);
                    s.quit = true;
                    s.running = false;
                }
                return;
            }
            TraceLog(LOG_INFO, "SFSKATE: ui script: %s after %.1f s", cond.c_str(), GetTime() - s.untilStart);
            s.untilStart = -1;
            s.at++;
            continue;
        }
        s.at++;
        TraceLog(LOG_INFO, "SFSKATE: ui script: %s", st.c_str());
        if (cmd == "wait") { s.waitFrames = atoi(arg.c_str()); return; }
        if (cmd == "sleep") { s.waitUntil = GetTime() + atof(arg.c_str()); return; }
        if (cmd == "key") { gKeys.push_back(keyByName(arg)); return; }
        if (cmd == "type") { for (unsigned char c : arg) s.typing.push_back(c); stepScript(); return; }
        if (cmd == "paste") { gCtrl = true; gKeys.push_back(KEY_V); return; }
        if (cmd == "clipboard") { setClipboard(arg); continue; }
        if (cmd == "click") {
            auto it = gPlaces.find(arg);
            if (it == gPlaces.end()) { TraceLog(LOG_WARNING, "SFSKATE: ui script: nothing called \"%s\" on screen", arg.c_str()); continue; }
            gClick = true;
            gClickAt = {it->second.x + it->second.width / 2, it->second.y + it->second.height / 2};
            return;
        }
        if (cmd == "shot") { TakeScreenshot(arg.c_str()); return; }
        if (cmd == "quit") { s.quit = true; s.running = false; return; }
        if (cmd == "do") { if (!(s.game && s.game("do", arg))) TraceLog(LOG_WARNING, "SFSKATE: ui script: can't do \"%s\"", arg.c_str()); continue; }
        TraceLog(LOG_WARNING, "SFSKATE: ui script: unknown step \"%s\"", st.c_str());
    }
    s.running = false;
}
}  // namespace

void beginFrame() {
    gKeys.clear();
    gChars.clear();
    gCtrl = false;
    gClick = false;
    stepScript();
    gTypingFrame = gFocus != nullptr;
}

void injectKey(int key, bool ctrl) {
    gKeys.push_back(key);
    gCtrl |= ctrl;
}

bool keyPressed(int key) {
    for (int k : gKeys) if (k == key) return true;
    return IsKeyPressed(key);
}
bool keyDown(int key) {
    if ((key == KEY_LEFT_CONTROL || key == KEY_LEFT_SUPER) && gCtrl) return true;
    for (int k : gKeys) if (k == key) return true;
    return IsKeyDown(key);
}
bool keyRepeat(int key) { return keyPressed(key) || IsKeyPressedRepeat(key); }

uint32_t charPressed() {
    if (!gChars.empty()) { uint32_t c = gChars.front(); gChars.pop_front(); return c; }
    int c = GetCharPressed();
    return c > 0 ? (uint32_t)c : 0;
}

std::string clipboard() {
    const char* c = IsWindowReady() ? GetClipboardText() : nullptr;
    return c ? c : "";
}
void setClipboard(const std::string& s) { if (IsWindowReady()) SetClipboardText(s.c_str()); }

Vector2 mouse() { return gClick ? gClickAt : GetMousePosition(); }
bool mousePressed() { return gClick || IsMouseButtonPressed(MOUSE_BUTTON_LEFT); }

void focus(TextField* f) {
    if (f && f != gFocus) f->fresh = true;
    if (gFocus && gFocus != f) gFocus->fresh = false;
    gFocus = f;
    if (f) gTypingFrame = true;
}
TextField* focused() { return gFocus; }
bool typing() { return gTypingFrame || gFocus; }

void place(const std::string& id, Rectangle r) { gPlaces[id] = r; }

void startScript(const std::string& script, std::function<bool(const std::string&, const std::string&)> game) {
    gScript = Script();
    std::string cur;
    auto flush = [&] {
        std::string t = trim(cur);
        if (!t.empty()) gScript.steps.push_back(t);
        cur.clear();
    };
    for (char c : script) {
        if (c == ';' || c == '\n') flush();
        else cur += c;
    }
    flush();
    gScript.game = std::move(game);
    gScript.running = true;
}
bool scriptRunning() { return gScript.running; }
bool scriptQuit() { return gScript.quit; }

}  // namespace sui
