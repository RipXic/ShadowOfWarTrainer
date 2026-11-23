// Shadow of War Trainer - languages. One JSON file per language in the trainer's lang folder:
//   { "_language": "Deutsch", "English text": "translated text", ... }
// The English text itself is the key, so a missing or empty entry simply shows English and code needs no text ids.
// Keys with {1} {2}.. are patterns for messages with changing parts ("Removed effect #{1}").
// Files whose name starts with "_" (the template) are ignored. Only what is drawn is translated: ids, tab names used
// by code, commands and trainer.log stay English.
// Render thread only (the panel and toasts are drawn there; a language change is applied between two frames).
#include "common.h"
#include <fstream>
#include <unordered_map>
#include <algorithm>

namespace wb {
namespace {
std::unordered_map<std::string, std::string> g_tr;
std::vector<std::pair<std::string, std::string>> g_pat;      // keys with {n}
std::vector<LangInfo> g_langs;
std::string g_code = "en", g_font_file, g_want;
bool g_apply = false;

std::wstring widen(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0); if (n) MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
std::string narrow(const std::wstring& w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0); if (n) WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
std::string lang_dir() { return g_cfg.root + "\\lang"; }
bool read_json(const std::string& path, json& j) {
    std::ifstream f(path, std::ios::binary); if (!f) return false;
    try { j = json::parse(f); return j.is_object(); }
    catch (std::exception& e) { log("[lang] %s is not valid JSON: %s", path.substr(path.find_last_of('\\') + 1).c_str(), e.what()); return false; }
}
// {n} placeholder at s[i]? -> its number (1-9), else 0
int hole(const std::string& s, size_t i) { return i + 2 < s.size() && s[i] == '{' && s[i + 2] == '}' && s[i + 1] >= '1' && s[i + 1] <= '9' ? s[i + 1] - '0' : 0; }

bool match(const std::string& key, const std::string& s, std::string (&part)[10]) {
    size_t k = 0, p = 0; int open = 0; size_t open_at = 0;
    while (k < key.size()) {
        if (const int h = hole(key, k)) { open = h; open_at = p; k += 3; continue; }
        size_t e = k; while (e < key.size() && !hole(key, e)) ++e;
        const std::string lit = key.substr(k, e - k);
        size_t at;
        if (open) { at = e == key.size() && s.size() >= lit.size() ? s.size() - lit.size() : s.find(lit, p);
                    if (at == std::string::npos || at < open_at || s.compare(at, lit.size(), lit) != 0) return false;
                    part[open] = s.substr(open_at, at - open_at); open = 0; }
        else { at = p; if (s.compare(at, lit.size(), lit) != 0) return false; }
        p = at + lit.size(); k = e;
    }
    if (open) { part[open] = s.substr(open_at); return true; }
    return p == s.size();
}
bool whole(const std::string& s, std::string& out) {
    auto it = g_tr.find(s);
    if (it != g_tr.end()) { out = it->second; return true; }
    for (auto& kv : g_pat) {
        std::string part[10];
        if (!match(kv.first, s, part)) continue;
        out.clear();
        for (size_t i = 0; i < kv.second.size(); ) {
            if (const int h = hole(kv.second, i)) { out += tr(part[h]); i += 3; } else out += kv.second[i++];
        }
        return true;
    }
    return false;
}

void load(const std::string& code) {
    g_tr.clear(); g_pat.clear(); g_font_file.clear(); g_code = "en";
    if (code.empty() || code == "en") return;
    const LangInfo* li = nullptr;
    for (auto& l : g_langs) if (l.code == code) li = &l;                     // only files that were listed (no paths)
    if (!li) { log("[lang] no lang\\%s.json - using English", code.c_str()); return; }
    json j; if (!read_json(lang_dir() + "\\" + li->code + ".json", j)) return;
    for (auto& kv : j.items()) {
        if (kv.key().empty() || kv.key()[0] == '_' || !kv.value().is_string()) continue;
        const std::string v = kv.value().get<std::string>();
        if (v.empty()) continue;
        bool pat = false; for (size_t i = 0; i < kv.key().size(); ++i) if (hole(kv.key(), i)) pat = true;
        if (pat) g_pat.push_back({ kv.key(), v }); else g_tr[kv.key()] = v;
    }
    // longest (most specific) pattern first
    std::sort(g_pat.begin(), g_pat.end(), [](auto& a, auto& b) { return a.first.size() > b.first.size(); });
    g_font_file = j.contains("_font") && j["_font"].is_string() ? j["_font"].get<std::string>() : std::string();
    if (g_font_file.find_first_of("\\/:") != std::string::npos) g_font_file.clear();       // a file name in lang\ only
    g_code = code;
    log("[lang] %s: %zu texts, %zu patterns", code.c_str(), g_tr.size(), g_pat.size());
}
void scan() {
    g_langs.clear(); g_langs.push_back({ "en", "English" });
    WIN32_FIND_DATAA fd{}; HANDLE h = FindFirstFileA((lang_dir() + "\\*.json").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::string n = fd.cFileName; if (n.empty() || n[0] == '_' || n.size() < 6) continue;
        const std::string code = n.substr(0, n.size() - 5);
        if (code == "en") continue;
        json j; if (!read_json(lang_dir() + "\\" + n, j)) continue;
        g_langs.push_back({ code, j.contains("_language") && j["_language"].is_string() ? j["_language"].get<std::string>() : code });
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}
} // namespace

const char* tr(const char* s) {
    if (!s || g_tr.empty()) return s;
    auto it = g_tr.find(s);
    return it == g_tr.end() ? s : it->second.c_str();
}
std::string tr(const std::string& s) { return tr(s.c_str()); }

// messages: the whole text, a pattern, or "label - message" with each side on its own
std::string tr_msg(const std::string& s) {
    if (g_code == "en") return s;
    std::string out;
    if (whole(s, out)) return out;
    for (size_t p = s.find(" - "); p != std::string::npos; p = s.find(" - ", p + 1)) {
        std::string l; if (!whole(s.substr(0, p), l)) continue;
        return l + " - " + tr_msg(s.substr(p + 3));
    }
    const size_t p = s.find(" - ");
    if (p != std::string::npos) return s.substr(0, p) + " - " + tr_msg(s.substr(p + 3));
    return s;
}

std::string upper_utf8(const std::string& s) { std::wstring w = widen(s); if (!w.empty()) CharUpperBuffW(&w[0], (DWORD)w.size()); return narrow(w); }
std::string lower_utf8(const std::string& s) { std::wstring w = widen(s); if (!w.empty()) CharLowerBuffW(&w[0], (DWORD)w.size()); return narrow(w); }

void lang_init() {
    scan();
    json c; std::string code = "en";
    if (read_json(g_cfg.root + "\\config.json", c) && c.contains("language") && c["language"].is_string()) code = c["language"].get<std::string>();
    load(code);
}
const std::vector<LangInfo>& lang_list() { return g_langs; }
const std::string& lang_code() { return g_code; }
std::string lang_font_file() { return g_font_file.empty() ? std::string() : lang_dir() + "\\" + g_font_file; }
void lang_request(const std::string& code) { g_want = code; g_apply = true; }
// overlay, between two frames: true = texts changed, rebuild the fonts
bool lang_apply_pending() {
    if (!g_apply) return false;
    g_apply = false; scan(); load(g_want);
    return true;
}
// every text the fonts must be able to draw (so only the letters in use are loaded, not whole alphabets)
std::string lang_glyph_text() {
    std::string t;
    auto add = [&](const std::string& s) { t += s; t += upper_utf8(s); };
    for (auto& kv : g_tr) add(kv.second);
    for (auto& kv : g_pat) add(kv.second);
    for (auto& l : g_langs) t += l.name;
    json p;                                                                  // a panel.json the user edited by hand
    if (read_json(g_cfg.root + "\\panel.json", p)) add(p.dump(-1, ' ', false, json::error_handler_t::replace));
    return t;
}

} // namespace wb
