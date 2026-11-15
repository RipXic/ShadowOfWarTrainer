#include <functional>
#include <condition_variable>
#include <deque>
// Shadow of War Trainer - data-driven panel. Everything shown comes from panel.json; the Reload button applies edits live.
//
// Item types:  toggle | int | float | button | text | input
// Action kinds: mem | patch | console | call | multi | local
//   mem     {"addr":"expr","vtype":"i32|i64|f32|f64|u8","on":1,"off":0,"freeze":false}
//   patch   {"addr":"expr","bytes":"90 90"}                        (toggle: restores original on off)
//   console {"cmd":"..."} | {"on_cmd":"...","off_cmd":"..."} | {"cmd":"cvar {v}"}  ({v} = value)
//   call    {"addr":"expr","args":[...],"ret":"u64"}
//   multi   {"actions":[...]}                                      (runs each in order)
//   local   {}                                                     (value only kept in the panel)
#include "common.h"
#include "ui_kit.h"
#include <fstream>
#include <map>
#include <algorithm>
#include <thread>
#include <cmath>
#include "imgui.h"
#include "imgui_internal.h"

namespace wb {

// ---- adapters for panel kinds this game has no use for (they answer cleanly)
static bool tx_active() { return false; }
static bool gated_write(uintptr_t a, const void* p, size_t n, const std::string&) { return safe_write(a, p, n); }
static void item_catalog_load() {}
static json console_exec(const std::string&) { return fail("NO_CONSOLE", "this game has no console"); }
static bool cvar_value(const std::string&, std::string&) { return false; }
static bool engine_state_get(const std::string&, double&) { return false; }
static bool engine_state_set(const std::string&, double) { return false; }
static json engine_call(const std::string& fn, const json& args) { return dispatch(json{ {"cmd", fn}, {"args", args} }); }
// "feature" and "lua" panel kinds are not used by this trainer
static json feature_set(const std::string&, bool) { return fail("NOT_AVAILABLE", "not in this build"); }
static bool feature_on(const std::string&) { return false; }
static json lua_exec(const std::string&, bool) { return fail("NOT_AVAILABLE", "not in this build"); }
static bool lua_live_value(const std::string&, std::string&) { return false; }
static std::string game_build_id() { return "1.0.8874|"; }
static std::string game_version() { return "1.0.8874"; }

extern ImFont* g_font_body;
extern ImFont* g_font_bold;
extern ImFont* g_font_title;
extern float g_ui_scale;
#define S(x) ((x) * g_ui_scale)

struct Item {
    std::string id, label, type, desc;
    std::string icon, style;    // skin: row icon (ri_<icon>.png) and control style ("switch" | "check" for toggles, "primary" buttons)
    json action;
    double minv = 0, maxv = 100, step = 1;
    bool persist = true;
    // live state
    bool on = false;
    bool lock = false;          // freeze for value items
    double val = 0;
    char text[512] = {};
    std::vector<uint8_t> patch_orig;
    std::string last_err;
    bool pending_reapply = false;
    uint64_t req_seq = 0;            // bridge requests sent for this item (only the newest one may revert the switch)
    ULONGLONG live_after = 0;   // ignore getter values reported before this (the action is still on its way to the game)
    std::vector<std::string> options;   // 'select' items
    std::string live_str;       // cvar items: value cached by the main-thread tick
    bool live_ok = false;
};
struct Section { std::string name; std::vector<Item> items; };
struct Tab { std::string name; std::vector<Section> sections; };

static std::recursive_mutex g_mtx;
static std::vector<Tab> g_tabs;
static std::string g_title = WB_PRODUCT_UP;
static std::string g_load_msg;
static int g_tab = 0;
static const char* kSettingsTabName = "Settings";
static char g_search[128] = {};
static char g_console_line[512] = {};
static std::vector<std::string> g_console_hist;
static ULONGLONG g_reapply_deadline = 0;

static std::string panel_path() { return g_cfg.root + "\\panel.json"; }
static std::string state_path() { return g_cfg.root + "\\panel_state.json"; }

// ------------------------------------------------------------------ value io
static size_t vsize(const std::string& t) { return t == "i64" || t == "f64" ? 8 : t == "u8" ? 1 : 4; }

// XOR-encoded values: value = enc ^ key, enc == 0 means 0.
static bool xor_read(const json& a, double& out, std::string& err, int bytes) {
    uintptr_t va, ka;
    if (!eval_addr(a.value("addr", ""), va, &err) || !eval_addr(a.value("key", ""), ka, &err)) return false;
    uint32_t e = 0, k = 0;
    if (!safe_read(va, &e, bytes) || !safe_read(ka, &k, bytes)) { err = "unreadable " + hexstr(va); return false; }
    out = e ? (double)(int64_t)(bytes == 2 ? (uint16_t)(e ^ k) : (int32_t)(e ^ k)) : 0;
    return true;
}
static bool xor_write(const json& a, double v, std::string& err, int bytes) {
    uintptr_t va, ka;
    if (!eval_addr(a.value("addr", ""), va, &err) || !eval_addr(a.value("key", ""), ka, &err)) return false;
    uint32_t k = 0; if (!safe_read(ka, &k, bytes)) { err = "unreadable key"; return false; }
    uint32_t want = bytes == 2 ? (uint16_t)(int64_t)v : (uint32_t)(int32_t)(int64_t)v;
    uint32_t e = 0;
    if (want) {
        e = want ^ k;
        if (bytes == 2) e &= 0xFFFF;
        if (e == 0) {                                  // value equals key: pick a new key so enc stays non-zero
            k = (k + 0x1357) & (bytes == 2 ? 0xFFFF : 0xFFFFFFFF);
            if (!safe_write(ka, &k, bytes)) { err = "key write failed"; return false; }
            e = want ^ k;
        }
    }
    if (!safe_write(va, &e, bytes)) { err = "write failed " + hexstr(va); return false; }
    return true;
}

static bool mem_read_value(const json& a, double& out, std::string& err) {
    if (a.value("vtype", "") == "x32") return xor_read(a, out, err, 4);
    if (a.value("vtype", "") == "x16") return xor_read(a, out, err, 2);
    uintptr_t addr; if (!eval_addr(a.value("addr", ""), addr, &err)) return false;
    std::string t = a.value("vtype", "i32");
    uint8_t b[8]{};
    if (!safe_read(addr, b, vsize(t))) { err = "unreadable " + hexstr(addr); return false; }
    if (t == "i32") { int32_t v; memcpy(&v, b, 4); out = v; }
    else if (t == "i64") { int64_t v; memcpy(&v, b, 8); out = (double)v; }
    else if (t == "f32") { float v; memcpy(&v, b, 4); out = v; }
    else if (t == "f64") { double v; memcpy(&v, b, 8); out = v; }
    else { out = b[0]; }
    return true;
}

static bool mem_write_value(const json& a, double v, std::string& err) {
    // review 2026-11-15: no raw writes on a game build the trainer was not made for (also covers freeze / lock ticks)
    if (!sow_game_supported()) { err = "this game version is not supported - feature switched off"; return false; }
    if (a.value("vtype", "") == "x32") return xor_write(a, v, err, 4);
    if (a.value("vtype", "") == "x16") return xor_write(a, v, err, 2);
    uintptr_t addr; if (!eval_addr(a.value("addr", ""), addr, &err)) return false;
    std::string t = a.value("vtype", "i32");
    uint8_t b[8]{};
    if (t == "i32") { int32_t x = (int32_t)v; memcpy(b, &x, 4); }
    else if (t == "i64") { int64_t x = (int64_t)v; memcpy(b, &x, 8); }
    else if (t == "f32") { float x = (float)v; memcpy(b, &x, 4); }
    else if (t == "f64") { memcpy(b, &v, 8); }
    else { b[0] = (uint8_t)v; }
    if (!gated_write(addr, b, vsize(t), "panel:" + a.value("addr", std::string()))) { err = "write failed " + hexstr(addr); return false; }
    return true;
}

static std::string fill_value(std::string cmd, double v, bool is_int) {
    char num[64];
    if (is_int) sprintf_s(num, "%lld", (long long)v); else sprintf_s(num, "%g", v);
    size_t p;
    while ((p = cmd.find("{v}")) != std::string::npos) cmd.replace(p, 3, num);
    return cmd;
}

static Item* find_item(const std::string& id);
static std::string item_value_str(const Item& it) {
    if (it.type == "select" || it.type == "input") return it.text;
    if (it.type == "toggle") return it.on ? "1" : "0";
    char b[64]; if (it.type == "int") sprintf_s(b, "%lld", (long long)it.val); else sprintf_s(b, "%g", it.val); return b;
}
static std::string resolve_refs(std::string code) {
    size_t p = 0;
    while ((p = code.find("{@", p)) != std::string::npos) {
        size_t e = code.find('}', p); if (e == std::string::npos) break;
        Item* ref = find_item(code.substr(p + 2, e - p - 2));
        std::string v = ref ? item_value_str(*ref) : "";
        code.replace(p, e - p + 1, v); p += v.size();
    }
    return code;
}

static json item_ref(const std::string& id);

// Bridge commands from panel clicks run on ONE worker thread in click order (a fast on-then-off used to be able to run
// as off-then-on on two detached threads, leaving a hook installed while the panel showed OFF).
static void bridge_post(std::function<void()> fn) {
    static std::mutex m; static std::condition_variable cv; static std::deque<std::function<void()>> q; static bool started = false;
    { std::lock_guard<std::mutex> lk(m); q.push_back(std::move(fn));
      if (!started) { started = true; std::thread([]() { for (;;) { std::function<void()> f; { std::unique_lock<std::mutex> lk2(m); cv.wait(lk2, [] { return !q.empty(); }); f = std::move(q.front()); q.pop_front(); } try { f(); } catch (...) {} } }).detach(); } }
    cv.notify_one();
}
// copy into a fixed char buffer, cut instead of the CRT's invalid-parameter abort when too long
template <size_t N> static void set_text(char (&dst)[N], const std::string& v) { strncpy_s(dst, v.c_str(), _TRUNCATE); }
// Runs one action. "phase": "on" | "off" | "value" | "press". Returns false + err on failure.
static bool run_action(Item& it, const json& a, const std::string& phase, std::string& err) {
    std::string kind = a.value("kind", "local");
    if (kind == "local") return true;
    // review 2026-11-15: raw memory writes follow the version guard too (an unknown game build gets nothing written);
    // "off" still restores what an earlier "on" changed
    if ((kind == "mem" || kind == "patch") && phase != "off" && !sow_game_supported()) {
        err = "this game version is not supported - feature switched off"; return false;
    }
    if (kind == "multi") {
        for (auto& sub : a.value("actions", json::array())) if (!run_action(it, sub, phase, err)) return false;
        return true;
    }
    if (kind == "mem") {
        if (phase == "on" && a.contains("on")) return mem_write_value(a, a["on"].get<double>(), err);
        if (phase == "off" && a.contains("off")) return mem_write_value(a, a["off"].get<double>(), err);
        if (phase == "value") return mem_write_value(a, it.val, err);
        return true;
    }
    if (kind == "patch") {
        uintptr_t addr; if (!eval_addr(a.value("addr", ""), addr, &err)) return false;
        std::vector<uint8_t> bytes;
        std::string hx = a.value("bytes", "");
        for (size_t i = 0; i < hx.size();) {
            if (hx[i] == ' ') { ++i; continue; }
            bytes.push_back((uint8_t)strtoul(hx.substr(i, 2).c_str(), nullptr, 16)); i += 2;
        }
        if (bytes.empty()) { err = "patch has no bytes"; return false; }
        if (phase == "on") {
            it.patch_orig.resize(bytes.size());
            if (!safe_read(addr, it.patch_orig.data(), bytes.size())) { err = "unreadable " + hexstr(addr); return false; }
            // already patched (left in place earlier): adopt it; "off" restores "expect"
            if (it.patch_orig == bytes) {
                if (a.contains("expect")) {
                    std::vector<uint8_t> ex; std::string hx2 = a["expect"].get<std::string>();
                    for (size_t i = 0; i < hx2.size();) { if (hx2[i] == ' ') { ++i; continue; } ex.push_back((uint8_t)strtoul(hx2.substr(i, 2).c_str(), nullptr, 16)); i += 2; }
                    if (ex.size() == bytes.size()) { it.patch_orig = ex; return true; }
                }
                err = "already patched and no 'expect' bytes to restore later"; it.patch_orig.clear(); return false;
            }
            if (a.contains("expect")) {
                char hb[4]; std::string cur;
                for (size_t i = 0; i < it.patch_orig.size(); ++i) { sprintf_s(hb, "%02X", it.patch_orig[i]); if (i) cur += ' '; cur += hb; }
                if (_stricmp(cur.c_str(), a["expect"].get<std::string>().c_str()) != 0) { err = "bytes differ from expected (" + cur + ") - not patched"; return false; }
            }
            return gated_write(addr, bytes.data(), bytes.size(), "patch:" + it.id) || (err = "write failed", false);
        }
        if (phase == "off" && !it.patch_orig.empty()) {
            bool w = safe_write(addr, it.patch_orig.data(), it.patch_orig.size());
            it.patch_orig.clear();
            return w || (err = "restore failed", false);
        }
        return true;
    }
    if (kind == "cvar") {
        std::string name = a.value("name", ""), cmd;
        auto num = [&](const json& v) { return v.is_string() ? v.get<std::string>() : fill_value("{v}", v.get<double>(), v.is_number_integer()); };
        if (phase == "on" && a.contains("on")) cmd = name + " " + num(a["on"]);
        else if (phase == "off" && a.contains("off")) cmd = name + " " + num(a["off"]);
        else if (phase == "value") cmd = name + " " + fill_value("{v}", it.val, it.type == "int");
        if (cmd.empty()) return true;
        post_main([cmd]() { console_exec(cmd); });
        return true;
    }
    if (kind == "state") {
        std::string f = a.value("field", "");
        double v = phase == "on" ? a.value("on", 1.0) : phase == "off" ? a.value("off", 0.0) : phase == "press" ? a.value("value", 1.0) : it.val;
        if (!engine_state_set(f, v)) { err = "no State field '" + f + "'"; return false; }
        return true;
    }
    if (kind == "ini") {
        // companion settings: {"file":"companions\\X\\x.ini","section":"Helm","set":{"Enabled":"true","DefaultHidden":"{on}"}}
        std::string file = g_cfg.root + "\\" + a.value("file", std::string()), sec = a.value("section", std::string());
        std::string onv = phase == "off" ? a.value("off", std::string("false")) : a.value("on", std::string("true"));
        const json set_ = a.value("set", json::object());   // keep it alive: items() of a temporary dangles
        for (auto& kv : set_.items()) {
            std::string v = kv.value().is_string() ? kv.value().get<std::string>() : kv.value().dump();
            if (v == "{on}") v = onv;
            if (v == "{v}") { char b[64]; sprintf_s(b, it.type == "int" ? "%.0f" : "%g", it.val); v = b; }
            if (!WritePrivateProfileStringA(sec.c_str(), kv.key().c_str(), (" " + v).c_str(), file.c_str())) { err = "cannot write " + file; return false; }
        }
        return true;
    }
    if (kind == "key") {
        // press a key for another mod's hotkey: {"vk":36,"close_panel":true}
        const bool off_ = phase == "off";
        int vk = a.value(off_ ? "off_vk" : "vk", 0); bool close = a.value("close_panel", true);
        if (!vk) return true;
        std::vector<WORD> mods;
        for (auto& m : a.value(off_ ? "off_mods" : "mods", json::array())) { std::string s = m.is_string() ? m.get<std::string>() : std::string(); mods.push_back(s == "ctrl" ? VK_CONTROL : s == "alt" ? VK_MENU : VK_SHIFT); }
        std::thread([vk, close, mods]() {
            if (close) { overlay_set_visible(false); Sleep(120); }
            INPUT in[2] = {}; in[0].type = in[1].type = INPUT_KEYBOARD;
            in[0].ki.wVk = in[1].ki.wVk = (WORD)vk; in[1].ki.dwFlags = KEYEVENTF_KEYUP;
            if (vk == VK_HOME || vk == VK_END || vk == VK_INSERT || vk == VK_DELETE || vk == VK_PRIOR || vk == VK_NEXT) in[0].ki.dwFlags |= KEYEVENTF_EXTENDEDKEY, in[1].ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
            auto mod_ = [&](bool up) { for (WORD m : mods) { INPUT x{}; x.type = INPUT_KEYBOARD; x.ki.wVk = m; if (up) x.ki.dwFlags = KEYEVENTF_KEYUP; SendInput(1, &x, sizeof(INPUT)); } };
            mod_(false); if (!mods.empty()) Sleep(30);
            SendInput(1, &in[0], sizeof(INPUT)); Sleep(60); SendInput(1, &in[1], sizeof(INPUT));
            if (!mods.empty()) Sleep(30); mod_(true);
        }).detach();
        return true;
    }
    if (kind == "native") {
        if (phase == "off" && !a.contains("off_fn")) return true;
        std::string fn = phase == "off" ? a.value("off_fn", std::string()) : a.value("fn", std::string());
        json args = a.value("args", json::object());
        for (auto& kv : args.items()) {
            if (!kv.value().is_string()) continue;
            std::string s = kv.value().get<std::string>();
            if (s == "{v}") { kv.value() = it.type == "int" ? json((int64_t)it.val) : json(it.val); continue; }
            if (s.find("{@") != std::string::npos) {
                s = resolve_refs(s); char* e = nullptr; double d = strtod(s.c_str(), &e);
                kv.value() = (e && *e == 0 && !s.empty()) ? (d == (int64_t)d ? json((int64_t)d) : json(d)) : json(s);
            }
        }
        std::string label = it.label;
        std::thread([fn, args, label]() { json r = engine_call(fn, args); toast(label + (r["ok"].get<bool>() ? " - done" : " - " + r["message"].get<std::string>())); }).detach();
        return true;
    }
    if (kind == "feature") {
        std::string name = a.value("name", "");
        if (phase == "on" || phase == "off") { json r = feature_set(name, phase == "on"); if (!r["ok"].get<bool>()) { err = r["message"].get<std::string>(); return false; } }
        return true;
    }
    if (kind == "lua") {
        std::string code;
        if (phase == "on") code = a.value("on", "");
        else if (phase == "off") code = a.value("off", "");
        else if (phase == "value") code = fill_value(a.value("set", ""), it.val, it.type == "int");
        else code = a.value("code", "");
        if (code.empty()) return true;
        code = resolve_refs(code);
        post_main([code]() { lua_exec(code, false); });
        return true;
    }
    if (kind == "hks") {
        // Shadow of War: Havok Script in the game's own VM (sow.cpp). on/off/code(press)/set(value, {v})
        std::string code;
        if (phase == "on") code = a.value("on", "");
        else if (phase == "off") code = a.value("off", "");
        else if (phase == "value") code = fill_value(a.value("set", ""), it.val, it.type == "int");
        else code = a.value("code", "");
        if (code.empty()) return true;
        code = resolve_refs(code);
        { std::string t = it.text; size_t p; while ((p = code.find("{text}")) != std::string::npos) code.replace(p, 6, t); }
        const std::string id = it.id, label = it.label; const bool quiet = a.value("quiet", phase == "on" || phase == "off" || phase == "value");
        post_main([code, id, label, quiet]() {
            std::string out, e;
            const bool okr = hks_run_panel(code, &out, &e);
            std::lock_guard<std::recursive_mutex> lk(g_mtx);
            for (auto& t : g_tabs) for (auto& s : t.sections) for (auto& x : s.items) if (x.id == id) x.last_err = okr ? "" : e;
            if (!okr) { log("[panel] %s: %s", label.c_str(), e.c_str()); toast(label + " - failed"); }
            else if (!quiet) toast(label + (out.empty() ? " - done" : " - " + out));
        });
        return true;
    }
    if (kind == "console") {
        std::string cmd;
        if (phase == "on") cmd = a.value("on_cmd", a.value("cmd", ""));
        else if (phase == "off") cmd = a.value("off_cmd", "");
        else if (phase == "value") cmd = fill_value(a.value("cmd", ""), it.val, it.type == "int");
        else cmd = a.value("cmd", "");
        if (cmd.empty()) return true;
        post_main([cmd]() { json r = console_exec(cmd); if (!r["ok"].get<bool>()) log("console '%s' failed: %s", cmd.c_str(), r["message"].get<std::string>().c_str()); });
        return true;
    }
    if (kind == "bridge") {
        // run a trainer command. Press: {"cmd","args"}. Toggle: "on"/"off" = {cmd,args}. Value: "set" = {cmd,args}
        // ("{v}" in args = the value, "{text}" = the text field). Readout: "get" = {cmd,args,field} (panel tick).
        // Runs on a worker thread: commands wait for the game thread, the overlay must not.
        // Not while loading (review 2026-11-14): items, levels and data patches on a half-built player. "off" is allowed
        // (it only puts originals back).
        if (phase != "off" && game_base() && !sow_game_supported()) { err = "this game version is not supported - feature switched off"; return false; }
        if (phase != "off" && !sow_world_ready()) { err = "wait until you are in the game world (loading)"; return false; }
        json spec;
        if (phase == "on") { if (!a.contains("on")) return true; spec = a["on"]; }
        else if (phase == "off") { if (!a.contains("off")) return true; spec = a["off"]; }
        else if (phase == "value") { if (!a.contains("set")) return true; spec = a["set"]; }
        else spec = json{ {"cmd", a.value("cmd", std::string())}, {"args", a.value("args", json::object())} };
        json args = spec.value("args", json::object());
        for (auto& kv : args.items()) {
            if (!kv.value().is_string()) continue;
            const std::string s = kv.value().get<std::string>();
            if (s == "{v}") kv.value() = it.type == "int" ? json((int64_t)it.val) : json(it.val);
            else if (s == "{text}") kv.value() = std::string(it.text);
            else if (s.size() > 3 && s.compare(0, 2, "{@") == 0 && s.back() == '}') kv.value() = item_ref(s.substr(2, s.size() - 3));
        }
        const std::string cmd = spec.value("cmd", std::string()), id = it.id, label = it.label;
        const bool quiet = a.value("quiet", phase == "on" || phase == "off");
        it.live_after = GetTickCount64() + 1500;
        const bool turning_on = phase == "on" && it.type == "toggle";
        const uint64_t seq = ++it.req_seq;
        bridge_post([cmd, args, id, label, quiet, turning_on, seq]() {
            json r = dispatch(json{ {"cmd", cmd}, {"args", args} });
            const bool okr = r.value("ok", false);
            std::string msg = okr ? std::string() : r.value("message", std::string("command failed"));
            { std::lock_guard<std::recursive_mutex> lk(g_mtx);
              bool reverted = false;
              for (auto& t : g_tabs) for (auto& s : t.sections) for (auto& x : s.items) if (x.id == id) {
                  x.last_err = msg;
                  // review 2026-11-15: the command failed, so the option is not on - do not show it as ON. The saved
                  // state is left alone on purpose: a reapply that failed once (timeout) is tried again next session,
                  // and the next change the user makes saves the switch as OFF.
                  if (!okr && turning_on && x.req_seq == seq) x.on = false;   // a newer click wins
              } }
            if (!okr) toast(label + " - " + msg);
            else if (!quiet) {
                const json d = r.value("data", json::object());
                std::string extra;
                if (d.contains("after")) extra = " -> " + d["after"].dump(); else if (d.contains("level")) extra = " -> " + d["level"].dump();
                toast(label + extra);
            }
        });
        return true;
    }
    err = "unknown action kind '" + kind + "'";
    return false;
}

static void apply(Item& it, const std::string& phase) {
    std::string err;
    it.last_err = run_action(it, it.action, phase, err) ? "" : err;
    // refused because the game is loading: nothing ran, so the toggle must not show ON
    if (!it.last_err.empty() && (err.rfind("wait until", 0) == 0 || err.rfind("this game version", 0) == 0)) { if (it.type == "toggle" && phase == "on") it.on = false; toast(it.label + " - " + err); }
    if (!it.last_err.empty()) log("[panel] %s (%s): %s", it.label.c_str(), phase.c_str(), it.last_err.c_str());
}

// ------------------------------------------------------------------ load / save
static void save_state() {
    if (game_base() && !sow_game_supported()) return;   // unsupported game: switches are shown off, keep the saved choices
    json s = json::object();
    for (auto& t : g_tabs) for (auto& sec : t.sections) for (auto& it : sec.items) {
        if (!it.persist) continue;
        if (it.type == "toggle") s[it.id] = { {"on", it.on} };
        else if (it.type == "int" || it.type == "float") s[it.id] = { {"val", it.val}, {"lock", it.lock} };
        else if (it.type == "input" || it.type == "select") s[it.id] = { {"text", it.text} };
    }
    std::ofstream f(state_path()); f << s.dump(2);
}

static json starter_panel() {
    return json::parse(R"({
  "title": "SHADOW OF WAR TRAINER",
  "tabs": [
    { "name": "Start", "sections": [
      { "name": "Welcome", "items": [
        { "id": "info_1", "type": "text", "label": "Panel is driven by panel.json", "desc": "panel.json is missing or broken - reinstall the trainer files." }
      ]}
    ]}
  ]
})");
}

json panel_reload() {
    std::lock_guard<std::recursive_mutex> lk(g_mtx);
    json j;
    std::ifstream f(panel_path());
    if (!f) { j = starter_panel(); std::ofstream o(panel_path()); o << j.dump(2); }
    else {
        try { j = json::parse(f); }
        catch (std::exception& e) { g_load_msg = std::string("panel.json error: ") + e.what(); log("%s", g_load_msg.c_str()); return fail("BAD_PANEL", g_load_msg); }
    }
    json saved = json::object();
    { std::ifstream sf(state_path()); if (sf) try { saved = json::parse(sf); } catch (...) {} }

    // keep live state (patch originals, toggles) for items that survive a reload
    std::map<std::string, Item> old;
    for (auto& t : g_tabs) for (auto& s : t.sections) for (auto& it : s.items) old[it.id] = it;

    std::vector<Tab> tabs; int count = 0;
    g_title = j.value("title", std::string(WB_PRODUCT_UP));
    // "dev": true on a tab or section = hidden (never loaded)
    const bool dev = false;
    for (auto& jt : j.value("tabs", json::array())) {
        if (jt.value("dev", false) && !dev) continue;
        Tab t; t.name = jt.value("name", "Tab");
        for (auto& js : jt.value("sections", json::array())) {
            if (js.value("dev", false) && !dev) continue;
            Section s; s.name = js.value("name", "");
            for (auto& ji : js.value("items", json::array())) {
                Item it;
                it.id = ji.value("id", t.name + "_" + std::to_string(count));
                it.label = ji.value("label", it.id); it.type = ji.value("type", "button");
                it.desc = ji.value("desc", ""); it.action = ji.value("action", json::object());
                it.icon = ji.value("icon", ""); it.style = ji.value("style", "");
                it.minv = ji.value("min", 0.0); it.maxv = ji.value("max", 100.0); it.step = ji.value("step", 1.0);
                it.persist = ji.value("persist", it.type != "button");
                it.val = ji.value("default", it.minv);
                if (it.type == "fxlist") { it.val = -1; it.persist = false; }
                for (auto& o : ji.value("options", json::array())) if (o.is_string()) it.options.push_back(o.get<std::string>());
                if (ji.contains("options_file")) { std::ifstream of(g_cfg.root + "\\" + ji["options_file"].get<std::string>()); std::string ln; while (std::getline(of, ln)) { while (!ln.empty() && (ln.back() == 13 || ln.back() == 32)) ln.pop_back(); if (ln.size() >= 3 && (unsigned char)ln[0] == 0xEF) ln = ln.substr(3); if (!ln.empty()) it.options.push_back(ln); } }
                if (it.type == "select" && !it.options.empty()) set_text(it.text, ji.value("default_text", it.options[0]));
                auto o = old.find(it.id);
                if (o != old.end()) { it.on = o->second.on; it.lock = o->second.lock; it.val = o->second.val; it.patch_orig = o->second.patch_orig; set_text(it.text, std::string(o->second.text)); }
                else if (saved.contains(it.id)) {
                    auto& sv = saved[it.id];
                    it.on = sv.value("on", false); it.lock = sv.value("lock", false); it.val = sv.value("val", it.val);
                    set_text(it.text, sv.value("text", std::string()));
                    it.pending_reapply = it.persist && (it.on || it.lock);
                }
                s.items.push_back(it); ++count;
            }
            t.sections.push_back(std::move(s));
        }
        tabs.push_back(std::move(t));
    }
    g_tabs = std::move(tabs);
    g_reapply_deadline = GetTickCount64() + 20000;
    if (g_tab >= (int)g_tabs.size() + 1) g_tab = 0;
    item_catalog_load();
    g_load_msg = "Loaded " + std::to_string(count) + " items";
    log("[panel] %s", g_load_msg.c_str());
    return ok({ {"items", count}, {"tabs", g_tabs.size()} }, g_load_msg);
}

void panel_init() {
    panel_reload();
    add_main_tick(panel_main_tick);
    add_main_tick(chain_boot_tick);      // Chain Without Aiming before the game copies its target weights
    add_main_tick(gear_preview_tick);    // a transmog preview ends when the trainer closes (was only checked by the loot tick)
}

// ------------------------------------------------------------------ main-thread tick: freezes + reapply
static ULONGLONG g_last_lua_poll = 0;
static void poll_lua_getters() {
    std::string chunk;
    for (auto& t : g_tabs) for (auto& s : t.sections) for (auto& it : s.items) {
        if (it.action.value("kind", "") != "lua" || !it.action.contains("get")) continue;
        chunk += "do local ok, v = pcall(function() return " + it.action["get"].get<std::string>() + " end) WB.report('" + it.id + "', ok and tostring(v) or 'ERR') end\n";
    }
    for (auto& t : g_tabs) for (auto& s : t.sections) for (auto& it : s.items) {
        if (it.action.value("kind", "") != "lua" || !it.action.contains("options")) continue;
        chunk += "do local ok, v = pcall(function() return " + it.action["options"].get<std::string>() + " end) WB.report('" + it.id + "#opts', ok and tostring(v) or '') end\n";
    }
    if (!chunk.empty()) lua_exec(chunk, false);
}

// Shadow of War: "hks" getters/options run in the GAME's script VM, all items in one batched chunk.
// Results: id \x1F value, records separated by \x1E.
static std::map<std::string, std::string> g_hks_live;
static void poll_hks_getters() {
    // readouts only matter on screen: script calls on the player are not free of risk (a call on an empty player
    // object is a native fault the script VM cannot catch - crash 2026-11-13 16:03), so none run while it is closed
    if (!overlay_visible()) return;
    std::string chunk = "local __o = {}\nlocal function R(id, f) local ok, v = pcall(f) __o[#__o + 1] = id .. '\\31' .. (ok and tostring(v) or 'ERR') end\n";
    bool any = false;
    for (auto& t : g_tabs) for (auto& s : t.sections) for (auto& it : s.items) {
        if (it.action.value("kind", "") != "hks") continue;
        if (it.action.contains("get")) { chunk += "R('" + it.id + "', function() return " + it.action["get"].get<std::string>() + " end)\n"; any = true; }
        if (it.action.contains("options")) { chunk += "R('" + it.id + "#opts', function() return " + it.action["options"].get<std::string>() + " end)\n"; any = true; }
    }
    if (!any) return;
    chunk += "return table.concat(__o, '\\30')";
    std::string out, err;
    if (!hks_run_panel(chunk, &out, &err)) { static ULONGLONG last = 0; if (GetTickCount64() - last > 10000) { last = GetTickCount64(); log("[panel] hks getters: %s", err.c_str()); } return; }
    size_t p = 0;
    while (p <= out.size()) {
        size_t q = out.find('\x1E', p); if (q == std::string::npos) q = out.size();
        const std::string rec = out.substr(p, q - p); const size_t k = rec.find('\x1F');
        if (k != std::string::npos) g_hks_live[rec.substr(0, k)] = rec.substr(k + 1);
        p = q + 1;
    }
}
static bool live_value(const json& a, const std::string& key, std::string& out) {
    if (a.value("kind", "") == "hks" || a.value("kind", "") == "bridge") { auto f = g_hks_live.find(key); if (f == g_hks_live.end()) return false; out = f->second; return true; }
    return lua_live_value(key, out);
}
// toggles with "keep" (and locked values with "set") are re-asserted every keep_ms (default 1000) while on: survives
// deaths, loads and cutscenes that reset the game's own flags
static void hks_keep() {
    const ULONGLONG now = GetTickCount64();
    for (auto& t : g_tabs) for (auto& s : t.sections) for (auto& it : s.items) {
        const json& a = it.action;
        if (a.value("kind", "") != "hks") continue;
        std::string code;
        if (it.type == "toggle" && it.on && a.contains("keep")) code = a["keep"].get<std::string>();
        else if ((it.type == "int" || it.type == "float") && it.lock && a.contains("set")) code = fill_value(a["set"].get<std::string>(), it.val, it.type == "int");
        if (code.empty()) continue;
        static std::map<std::string, ULONGLONG> last;
        ULONGLONG& l = last[it.id];
        if (now - l < (ULONGLONG)a.value("keep_ms", 1000)) continue;
        l = now;
        std::string err; if (!hks_run_panel(code, nullptr, &err)) { if (err.find("not in the world") == std::string::npos) it.last_err = err; } else if (!it.last_err.empty() && it.last_err.find("keep") == std::string::npos) it.last_err.clear();
    }
}

// "{@id}" in bridge args: another panel item's text (select / input) or value (int / float / toggle)
static json item_ref(const std::string& id) {
    for (auto& t : g_tabs) for (auto& s : t.sections) for (auto& x : s.items) if (x.id == id) {
        if (x.type == "select" || x.type == "input") return std::string(x.text);
        if (x.type == "toggle") return x.on;
        if (x.type == "int" || x.type == "fxlist") return (int64_t)x.val;
        return x.val;
    }
    return std::string();
}
// "bridge" readouts: {"get":{"cmd","args","field"}} -> data[field] as text; toggles with "keep" re-run it every keep_ms
static bool is_fav(const std::string& id);
// Only what is on screen is polled (review 2026-11-14: every getter of every tab ran on the game thread every 300 ms /
// 2 s, including whole-table option scans): the open tab (+ favorites on Home/Favorites), option lists at most every
// 1.5 s or right after a tab switch, and while the trainer is hidden only toggle states.
static void poll_bridge_getters() {
    if (!sow_world_ready()) return;
    const bool vis = overlay_visible(); const ULONGLONG now = GetTickCount64();
    static ULONGLONG s_opts = 0; static int s_tab = -1;
    const bool opts_due = vis && (now - s_opts >= 1500 || s_tab != g_tab); if (opts_due) { s_opts = now; s_tab = g_tab; }
    const bool fav_view = vis && g_tab >= 0 && g_tab < (int)g_tabs.size() && (g_tabs[g_tab].name == "Favorites" || g_tabs[g_tab].name == "Home");
    for (size_t ti = 0; ti < g_tabs.size(); ++ti) for (auto& s : g_tabs[ti].sections) for (auto& it : s.items) {
        const json& a = it.action;
        if (a.value("kind", "") != "bridge") continue;
        const bool shown = (vis && (int)ti == g_tab) || (fav_view && is_fav(it.id));
        if (a.contains("options") && shown && opts_due) {                                       // select options: data[field] = newline list
            const json& o = a["options"]; json oa = o.value("args", json::object());
            for (auto& kv : oa.items()) if (kv.value().is_string()) { const std::string s = kv.value().get<std::string>(); if (s.size() > 3 && s.compare(0, 2, "{@") == 0) kv.value() = item_ref(s.substr(2, s.size() - 3)); }
            json r = dispatch(json{ {"cmd", o.value("cmd", std::string())}, {"args", oa} });
            if (r.value("ok", false) && r["data"].contains(o.value("field", std::string("text")))) {
                const std::string ol = r["data"][o.value("field", std::string("text"))].get<std::string>();
                g_hks_live[it.id + "#opts"] = ol;
                // the list changed under the current choice (another picker filters it, e.g. Kind -> Gear to Drop):
                // a choice that is no longer listed would keep being sent, so move to the first entry of the new list
                if (it.type == "select" && !a.contains("get") && !ol.empty()) {
                    const std::string cur = it.text; bool listed = false; std::string first; size_t p = 0;
                    while (p < ol.size()) { size_t q = ol.find('\n', p); if (q == std::string::npos) q = ol.size();
                        if (q > p) { const std::string ln = ol.substr(p, q - p); if (first.empty()) first = ln; if (ln == cur) { listed = true; break; } } p = q + 1; }
                    if (!listed) {
                        std::string pick = first; const size_t c = cur.find(':');
                        if (c != std::string::npos && c > 0 && c < 12) {           // "<id>: name" lists: keep the same id
                            const std::string key = cur.substr(0, c + 1); size_t p2 = 0;
                            while (p2 < ol.size()) { size_t q2 = ol.find('\n', p2); if (q2 == std::string::npos) q2 = ol.size();
                                if (ol.compare(p2, key.size(), key) == 0) { pick = ol.substr(p2, q2 - p2); break; } p2 = q2 + 1; }
                        }
                        if (!pick.empty() && pick.size() < sizeof it.text) { strcpy_s(it.text, pick.c_str()); save_state(); }
                    }
                }
            }
        }
        if (!a.contains("get")) continue;
        if (!shown && it.type != "toggle") continue;
        const json& g = a["get"]; json ga = g.value("args", json::object());
        for (auto& kv : ga.items()) if (kv.value().is_string()) { const std::string s = kv.value().get<std::string>(); if (s.size() > 3 && s.compare(0, 2, "{@") == 0) kv.value() = item_ref(s.substr(2, s.size() - 3)); }
        json r = dispatch(json{ {"cmd", g.value("cmd", std::string())}, {"args", ga} });
        if (!r.value("ok", false)) { g_hks_live[it.id] = "ERR"; continue; }
        const json d = r.value("data", json::object()); const std::string f = g.value("field", std::string("count"));
        if (!d.contains(f)) { g_hks_live[it.id] = "ERR"; continue; }
        if (d[f].is_string() && it.type == "text") { g_hks_live[it.id] = d[f].get<std::string>(); continue; }
        const json& v = d[f];
        g_hks_live[it.id] = v.is_string() ? v.get<std::string>() : v.dump();
    }
}
static void bridge_keep() {
    if (!sow_world_ready()) return;
    const ULONGLONG now = GetTickCount64();
    static std::map<std::string, ULONGLONG> last;
    for (auto& t : g_tabs) for (auto& s : t.sections) for (auto& it : s.items) {
        const json& a = it.action;
        if (a.value("kind", "") != "bridge" || it.type != "toggle" || !it.on || !a.contains("keep")) continue;
        ULONGLONG& l = last[it.id];
        if (now - l < (ULONGLONG)a.value("keep_ms", 1500)) continue;
        l = now;
        const json& k = a["keep"]; json ka = k.value("args", json::object());
        // {@id} -> the current value of that panel item, exactly like the on/off path (else changed choices never arrive)
        for (auto& kv : ka.items()) if (kv.value().is_string()) { const std::string s = kv.value().get<std::string>(); if (s.size() > 3 && s.compare(0, 2, "{@") == 0 && s.back() == '}') kv.value() = item_ref(s.substr(2, s.size() - 3)); }
        json r = dispatch(json{ {"cmd", k.value("cmd", std::string())}, {"args", ka} });
        it.last_err = r.value("ok", false) ? std::string() : r.value("message", std::string("keep failed"));
    }
}

void panel_main_tick() {
    std::lock_guard<std::recursive_mutex> lk(g_mtx);
    // The saved-option reapply window must start when this tick first RUNS, not when panel.json loads: in Shadow of
    // War the tick comes from the game's script VM, which only starts once a save is loaded (minutes after the
    // panel loads at the title screen). Verified 2026-11-13: after a relaunch the 20 s window had expired before
    // the first tick, so saved byte patches (Choose Multiple Skills, Might Never Drains) were never re-applied.
    // Review 2026-11-14: the window also waits for the world (player + DB) - saved options re-applied during a
    // loading screen ran AddItem / level / data patches on a half-built player, or failed and stayed shown ON.
    static bool s_window_started = false;
    const bool world_ready = sow_world_ready();
    if (!s_window_started && world_ready) { s_window_started = true; g_reapply_deadline = GetTickCount64() + 20000; }
    // getters every 300 ms while the trainer is open, every 2 s while it is hidden (toggle state for hotkeys/locks)
    if (GetTickCount64() - g_last_lua_poll > (overlay_visible() ? 300u : 2000u)) { g_last_lua_poll = GetTickCount64(); poll_lua_getters(); poll_hks_getters(); poll_bridge_getters(); }
    hks_keep(); bridge_keep();
    bool reapply_window = world_ready && GetTickCount64() < g_reapply_deadline;
    bool reapplied_this_frame = false;   // one saved option per frame: each one may scan the game's code
    for (auto& t : g_tabs) for (auto& s : t.sections) for (auto& it : s.items) {
        const json& a = it.action;
        std::string kind = a.value("kind", "local");
        if (it.pending_reapply && reapply_window && !reapplied_this_frame) {
            std::string err; double probe;
            bool resolvable = kind != "mem" || mem_read_value(a, probe, err);
            if (resolvable) {
                if (it.type == "toggle" && it.on) apply(it, "on");
                else if (it.lock) apply(it, "value");
                it.pending_reapply = false; reapplied_this_frame = true; it.live_after = GetTickCount64() + 2200;
            }
        }
        if (kind == "ini") {
            std::string file = g_cfg.root + "\\" + a.value("file", std::string()), sec = a.value("section", std::string());
            std::string key = a.value("live_key", std::string());
            if (!key.empty()) {
                char b[128] = {}; GetPrivateProfileStringA(sec.c_str(), key.c_str(), "", b, sizeof b, file.c_str());
                std::string v = b; while (!v.empty() && v.front() == ' ') v.erase(v.begin());
                if (it.type == "toggle") it.on = _stricmp(v.c_str(), a.value("on", std::string("true")).c_str()) == 0;
                else if (it.type == "int" || it.type == "float") it.val = atof(v.c_str());
            }
            continue;
        }
        if (kind == "state") {
            double lv;
            if (engine_state_get(a.value("field", std::string()), lv)) {
                if (it.type == "toggle") it.on = lv != 0;
                else if (it.type == "int" || it.type == "float") it.val = lv;
            }
            continue;
        }
        if (kind == "feature" && it.type == "toggle") { it.on = feature_on(a.value("name", "")); continue; }
        if ((kind == "lua" || kind == "hks" || kind == "bridge") && a.contains("get")) {
            bool have = live_value(a, it.id, it.live_str);
            // a toggle whose getter says nil was never switched on (or failed to): show it as off, not as the saved state
            if (have && it.type == "toggle" && it.live_str == "nil" && !it.pending_reapply && GetTickCount64() >= it.live_after) it.on = false;
            it.live_ok = have && it.live_str != "ERR" && it.live_str != "nil";
            if (it.live_ok && !it.pending_reapply && GetTickCount64() >= it.live_after) {   // (a saved option waiting for its reapply keeps its saved value)
                double lv = atof(it.live_str.c_str());
                if (it.type == "toggle") it.on = it.live_str == "true" || (lv != 0 && it.live_str != "false");
                if ((it.type == "int" || it.type == "float") && it.lock && fabs(lv - it.val) > 1e-6) apply(it, "value");
            }
            continue;
        }
        if (kind == "cvar") {
            it.live_ok = cvar_value(a.value("name", ""), it.live_str);
            if (it.live_ok) {
                double lv = atof(it.live_str.c_str());
                if (it.type == "toggle" && a.contains("on") && a["on"].is_number()) it.on = fabs(lv - a["on"].get<double>()) < 1e-6;
                if ((it.type == "int" || it.type == "float") && it.lock && fabs(lv - it.val) > 1e-6) apply(it, "value");
            }
            continue;
        }
        if (kind != "mem") continue;
        if (tx_active()) continue;
        if (it.type == "toggle" && it.on && a.value("freeze", false)) { std::string e; mem_write_value(a, a.value("on", 1.0), e); }
        if ((it.type == "int" || it.type == "float") && it.lock) { std::string e; mem_write_value(a, it.val, e); }
    }
}

// ------------------------------------------------------------------ widgets
static bool toggle_switch(const char* id, bool* v) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float h = ImGui::GetFrameHeight() * 0.78f, w = h * 1.85f;
    ImVec2 p = ImGui::GetCursorScreenPos();
    p.y += (ImGui::GetFrameHeight() - h) * 0.5f;
    ImGui::InvisibleButton(id, ImVec2(w, ImGui::GetFrameHeight()));
    bool clicked = ImGui::IsItemClicked();
    if (clicked) *v = !*v;
    ImGuiStorage* st = ImGui::GetStateStorage();
    ImGuiID sid = ImGui::GetID(id);
    float t = st->GetFloat(sid, *v ? 1.f : 0.f);
    t += (*v ? 1.f : -1.f) * ImGui::GetIO().DeltaTime * 9.f; t = ImClamp(t, 0.f, 1.f);
    st->SetFloat(sid, t);
    ImU32 off = ui::rgb(0x2A2826), on = ui::rgb(ui::kGold);   /* ui-rework 2026-11-04 */
    ImVec4 a = ImGui::ColorConvertU32ToFloat4(off), b = ImGui::ColorConvertU32ToFloat4(on);
    ImU32 col = ImGui::GetColorU32(ImLerp(a, b, t));
    if (ImGui::IsItemHovered()) col = ImGui::GetColorU32(ImLerp(ImLerp(a, b, t), ImVec4(1, 1, 1, 1), 0.08f));
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), col, h * 0.5f);
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), ui::rgb(ui::kGold, .35f + .4f * t), h * 0.5f);
    float r = h * 0.5f - 2.5f;
    dl->AddCircleFilled(ImVec2(p.x + r + 2.5f + t * (w - 2 * r - 5), p.y + h * 0.5f), r, ui::rgb(t > .5f ? ui::kInk : ui::kBone));
    return clicked;
}

namespace sk { static bool on(); static void section(const char* name); }
static void section_header(const char* name) {
    if (sk::on()) { sk::section(name); return; }
    ImGui::Dummy(ImVec2(0, 4));
    ImGui::PushFont(g_font_bold);
    ImGui::PushStyleColor(ImGuiCol_Text, ui::rgbv(ui::kGoldHi));
    std::string up = name; std::transform(up.begin(), up.end(), up.begin(), ::toupper);
    ImGui::TextUnformatted(up.c_str());
    ImGui::PopStyleColor(); ImGui::PopFont();
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y - 3), ImVec2(p.x + std::min(S(120.f), ImGui::GetContentRegionAvail().x), p.y - 3), ui::rgb(ui::kGold), S(1.5f));
    ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x + std::min(S(120.f), ImGui::GetContentRegionAvail().x), p.y - 3), ImVec2(p.x + ImGui::GetContentRegionAvail().x, p.y - 3), ui::rgb(ui::kGold, .22f));
    ImGui::Dummy(ImVec2(0, 2));
}

static bool matches(const Item& it) {
    if (!g_search[0]) return true;
    std::string a = it.label + " " + it.desc, b = g_search;
    std::transform(a.begin(), a.end(), a.begin(), ::tolower); std::transform(b.begin(), b.end(), b.begin(), ::tolower);
    return a.find(b) != std::string::npos;
}


/* NAV_HOME_2026_11_06 */
// ---- Favorites (bin\config.json "favorites": [item ids]) + Home tab
static std::vector<std::string> g_favs;
static bool g_favs_loaded = false;
static json read_cfg_json() { std::ifstream f(g_cfg.root + "\\config.json"); if (f) { try { return json::parse(f); } catch (...) {} } return json::object(); }
static void favs_load() {
    g_favs_loaded = true; g_favs.clear();
    json j = read_cfg_json();
    if (j.contains("favorites") && j["favorites"].is_array()) for (auto& v : j["favorites"]) if (v.is_string()) g_favs.push_back(v.get<std::string>());
}
static void favs_save() {
    json j = read_cfg_json();                       // read-modify-write: keep every other config key
    j["favorites"] = g_favs;
    std::ofstream f(g_cfg.root + "\\config.json"); f << j.dump(2);
}
static bool is_fav(const std::string& id) { if (!g_favs_loaded) favs_load(); for (auto& f : g_favs) if (f == id) return true; return false; }
static void set_fav(const std::string& id, bool on) {
    if (!g_favs_loaded) favs_load();
    auto itr = std::find(g_favs.begin(), g_favs.end(), id);
    if (on && itr == g_favs.end()) g_favs.push_back(id);
    if (!on && itr != g_favs.end()) g_favs.erase(itr);
    favs_save();
}
// small gold star in front of each row label
static void fav_star(const std::string& id) {
    bool f = is_fav(id);
    const float sz = ImGui::GetFrameHeight() * 0.62f;
    ImVec2 p = ImGui::GetCursorScreenPos(); p.y += (ImGui::GetFrameHeight() - sz) * 0.5f;
    if (ImGui::InvisibleButton("##fav", ImVec2(sz, ImGui::GetFrameHeight()))) set_fav(id, !f);
    bool hov = ImGui::IsItemHovered();
    if (hov) ImGui::SetTooltip(f ? "Remove from Favorites" : "Add to Favorites");
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 c(p.x + sz * 0.5f, p.y + sz * 0.5f); ImVec2 pts[10];
    for (int i = 0; i < 10; ++i) { float r = (i & 1) ? sz * 0.22f : sz * 0.5f; float a = -1.5707963f + i * 0.6283185f; pts[i] = ImVec2(c.x + cosf(a) * r, c.y + sinf(a) * r); }
    ImU32 col = f ? ui::rgb(ui::kGoldHi) : hov ? ui::rgb(ui::kBone) : ui::rgb(ui::kDim);
    if (f) dl->AddConvexPolyFilled(pts, 10, col); else dl->AddPolyline(pts, 10, col, ImDrawFlags_Closed, 1.2f);
    ImGui::SameLine(0, S(6));
}
static void draw_item(Item& it);
static Item* find_item(const std::string& id);
// ---- hotkeys: any toggle/button can get a key (bin\hotkeys.json). Click the key box, press a key; Backspace/Delete
// clears, Esc cancels. Polled every frame on the render thread; fires while the game window is in front.
static std::map<std::string, int> g_hk; static bool g_hk_loaded = false; static std::string g_hk_capture; static bool g_hk_prev[256] = {};
static std::string hk_path() { return g_cfg.root + "\\hotkeys.json"; }
static void hk_load() { g_hk_loaded = true; std::ifstream f(hk_path()); if (!f) return; try { json j = json::parse(f); for (auto& kv : j.items()) g_hk[kv.key()] = kv.value().get<int>(); } catch (...) {} }
static void hk_save() { json j = json::object(); for (auto& kv : g_hk) j[kv.first] = kv.second; std::ofstream f(hk_path()); f << j.dump(2); }
static std::string key_name(int vk) {
    if (vk >= VK_F1 && vk <= VK_F24) return "F" + std::to_string(vk - VK_F1 + 1);
    UINT sc = MapVirtualKeyA(vk, MAPVK_VK_TO_VSC); LONG lp = (LONG)(sc << 16);
    if (vk == VK_INSERT || vk == VK_DELETE || vk == VK_HOME || vk == VK_END || vk == VK_PRIOR || vk == VK_NEXT || (vk >= VK_LEFT && vk <= VK_DOWN) || vk == VK_DIVIDE) lp |= 1 << 24;
    char b[64] = {}; if (GetKeyNameTextA(lp, b, sizeof b) > 0) return b;
    char h[16]; sprintf_s(h, "Key %d", vk); return h;
}
static bool hk_bindable(int vk) { return vk >= 0x08 && vk != VK_SHIFT && vk != VK_CONTROL && vk != VK_MENU && vk != g_cfg.toggle_key && !(vk >= VK_LBUTTON && vk <= VK_XBUTTON2) && vk != VK_ESCAPE; }
static void hotkey_box(Item& it) {
    if (!g_hk_loaded) hk_load();
    const bool cap = g_hk_capture == it.id; auto f = g_hk.find(it.id);
    std::string lbl = cap ? "Press a key..." : (f != g_hk.end() ? key_name(f->second) : "Set key");
    ImGui::PushStyleColor(ImGuiCol_Text, cap ? ui::rgbv(ui::kGoldHi) : f != g_hk.end() ? ui::rgbv(ui::kBone) : ui::rgbv(ui::kDim));
    if (ImGui::Button((lbl + "##hk").c_str(), ImVec2(S(96), 0))) { g_hk_capture = cap ? "" : it.id; for (int k = 0; k < 256; ++k) g_hk_prev[k] = (GetAsyncKeyState(k) & 0x8000) != 0; }
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(cap ? "Press the key to use. Backspace/Delete clears, Esc cancels." : "Hotkey: click, then press a key");
}
// render thread, every frame (overlay.cpp)
void panel_hotkeys_poll() {
    std::lock_guard<std::recursive_mutex> lk(g_mtx);
    if (!g_hk_loaded) hk_load();
    bool down[256]; for (int k = 0; k < 256; ++k) down[k] = (GetAsyncKeyState(k) & 0x8000) != 0;
    if (!g_hk_capture.empty()) {
        for (int k = 0x08; k < 256; ++k) if (down[k] && !g_hk_prev[k]) {
            if (k == VK_ESCAPE) { g_hk_capture.clear(); break; }
            if (k == VK_BACK || k == VK_DELETE) { g_hk.erase(g_hk_capture); hk_save(); g_hk_capture.clear(); break; }
            if (!hk_bindable(k)) continue;
            for (auto it = g_hk.begin(); it != g_hk.end();) { if (it->second == k) it = g_hk.erase(it); else ++it; }   // one action per key
            g_hk[g_hk_capture] = k; hk_save(); g_hk_capture.clear(); break;
        }
        memcpy(g_hk_prev, down, sizeof down); return;
    }
    const bool typing = ImGui::GetCurrentContext() && ImGui::GetIO().WantTextInput;
    for (auto& kv : g_hk) {
        int k = kv.second; if (k <= 0 || k > 255 || !down[k] || g_hk_prev[k] || typing) continue;
        Item* it = find_item(kv.first); if (!it) continue;
        if (it->type == "toggle") { it->on = !it->on; apply(*it, it->on ? "on" : "off"); save_state(); toast(it->label + (it->on ? ": ON" : ": OFF")); }
        else if (it->type == "button") { apply(*it, "press"); toast(it->label); }
    }
    memcpy(g_hk_prev, down, sizeof down);
}
// ------------------------------------------------------------------ skin (skin\skin.json + PNGs)
// The Shadow of War art is drawn whole at one scale K (no 9-slice):
// frame + live pieces (tabs, icons, switches, slider, value boxes) + live text. Layout numbers are reference pixels of
// the 1620x971 picture, times K. No skin folder, or "Classic look" on (config.json classic_look) = the old look.
extern ImFont* g_font_skin; extern ImFont* g_font_skin_big; extern ImFont* g_font_skin_row;
bool skin_draw(ImDrawList* dl, const char* sprite, ImVec2 a, ImVec2 b, ImU32 tint);
namespace sk {
struct Piece { std::string kind; float w = 0, h = 0, l = 0, r = 0; };
static bool g_loaded = false, g_ok = false, g_classic = false;
static float g_user = 1.f;                                 // config.json skin_scale (Bridge tab)
static std::map<std::string, Piece> g_p;
static json g_lay = json::object();
static float K = 1.f;                                      // reference px -> screen px, set each frame
static ImVec2 O(0, 0);                                     // window origin on screen
constexpr uint32_t kCream = 0xF2E6C8, kWhite = 0xF4EFE4, kGoldT = 0xF0C35A, kGoldL = 0xF6DE9A, kGreen = 0x5BE33C, kRed = 0xC81E1E;
static bool load() {
    g_loaded = true; g_ok = false;
    try {
        json c = read_cfg_json();
        g_classic = c.value("classic_look", false); g_user = (float)c.value("skin_scale", 1.0);
        std::ifstream f(g_cfg.root + "\\skin\\skin.json");
        if (!f) return false;
        json j = json::parse(f, nullptr, false);
        if (!j.is_object() || !j.contains("pieces")) return false;
        for (auto& [n, v] : j["pieces"].items()) {
            Piece p; p.kind = v.value("kind", std::string("image")); p.w = v.value("w", 0.f); p.h = v.value("h", 0.f);
            p.l = v.value("l", 0.f); p.r = v.value("r", 0.f); g_p[n] = p;
        }
        if (j.contains("layout") && j["layout"].is_object()) g_lay = j["layout"];
        g_ok = g_p.count("frame") && g_p.count("tab");
    } catch (...) { g_ok = false; }
    return g_ok;
}
static bool on() { if (!g_loaded) load(); return g_ok && !g_classic; }
static void save_cfg() { json j = read_cfg_json(); j["classic_look"] = g_classic; j["skin_scale"] = g_user; std::ofstream f(g_cfg.root + "\\config.json"); f << j.dump(2); }
static float L(const char* k, float d = 0) { return (g_lay.contains(k) && g_lay[k].is_number() ? g_lay[k].get<float>() : d) * K; }
static ImVec4 box(const char* k) {                         // [x0,y0,x1,y1] reference px -> screen rect
    if (!g_lay.contains(k) || !g_lay[k].is_array() || g_lay[k].size() < 4) return ImVec4(0, 0, 0, 0);
    auto& v = g_lay[k]; return ImVec4(O.x + v[0].get<float>() * K, O.y + v[1].get<float>() * K, O.x + v[2].get<float>() * K, O.y + v[3].get<float>() * K);
}
static ImVec2 psize(const char* n) { auto it = g_p.find(n); return it == g_p.end() ? ImVec2(0, 0) : ImVec2(it->second.w * K, it->second.h * K); }
static bool draw(ImDrawList* dl, const char* n, ImVec2 a, ImVec2 b, ImU32 tint = IM_COL32_WHITE) {
    auto it = g_p.find(n); if (it == g_p.end()) return false;
    char spec[160];
    if (it->second.kind == "3slice") sprintf_s(spec, "%s#3:%g,%g", n, it->second.l, it->second.r); else sprintf_s(spec, "%s", n);
    return skin_draw(dl, spec, a, b, tint);
}
static ImU32 c_(uint32_t c, float a = 1.f) { return ui::rgb(c, a); }
static std::string upper(std::string s) { std::transform(s.begin(), s.end(), s.begin(), ::toupper); return s; }
// text at font size px (reference px * K), aligned in [a,b] (ax 0 left .5 centre 1 right), vertically centred
static float text(ImDrawList* dl, ImFont* f, float px, ImVec2 a, ImVec2 b, ImU32 col, const char* s, float ax = 0.f, bool shadow = true) {
    if (!f) f = g_font_body;
    const ImVec2 ts = f->CalcTextSizeA(px, FLT_MAX, 0.f, s);
    // Palatino's line box carries a deep descender area, so box-centred text looks high on the plates (user, 2026-11-10):
    // nudge the serif skin fonts down by a part of their size
    const float drop = (f == g_font_skin || f == g_font_skin_big) ? px * 0.11f : 0.f;
    const ImVec2 p(std::floor(a.x + (b.x - a.x - ts.x) * ax), std::floor(a.y + (b.y - a.y - ts.y) * .5f + drop));
    if (shadow) dl->AddText(f, px, ImVec2(p.x + std::max(1.f, K), p.y + std::max(1.f, K)), IM_COL32(0, 0, 0, 210), s);
    dl->AddText(f, px, p, col, s);
    return ts.x;
}
static float tw(ImFont* f, float px, const char* s) { if (!f) f = g_font_body; return f->CalcTextSizeA(px, FLT_MAX, 0.f, s).x; }
// skin colours (skin.json layout.style, 0xRRGGBB numbers), with built-in defaults
static uint32_t col(const char* k, uint32_t d) {
    if (g_lay.contains("style") && g_lay["style"].is_object() && g_lay["style"].contains(k) && g_lay["style"][k].is_number()) return g_lay["style"][k].get<uint32_t>();
    return d;
}
static bool has(const char* n) { return g_p.count(n) != 0; }
// plate button: Shadow of War art = "button" (bronze) / "button_hi" (green, on hover or primary)
static bool plate_button(const char* id, const char* label, ImVec2 a, ImVec2 sz, bool primary = false) {
    ImGui::SetCursorScreenPos(a);
    const bool click = ImGui::InvisibleButton(id, sz);
    const bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 b(a.x + sz.x, a.y + sz.y);
    if (has("button")) {
        draw(dl, (hov || primary) && has("button_hi") ? "button_hi" : "button", a, b, act ? IM_COL32(170, 170, 170, 255) : IM_COL32_WHITE);
        text(dl, g_font_skin, 15.f * K, a, b, c_(hov ? 0xFFFFFF : col("label", kCream)), label, .5f);
    } else {
        draw(dl, "tab_active", a, b, act ? IM_COL32(190, 190, 190, 255) : hov ? IM_COL32(255, 255, 255, 255) : IM_COL32(225, 225, 225, 255));
        text(dl, g_font_skin, 17.f * K, a, b, c_(hov ? kGoldL : kCream), label, .5f);
    }
    return click;
}
static void section(const char* name) {
    ImGui::Dummy(ImVec2(0, 6 * K));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos(); const float w = ImGui::GetContentRegionAvail().x;
    const ImVec2 is = psize("sec_icon"); const float ih = 30 * K, iw = is.y > 0 ? ih * is.x / is.y : ih;
    draw(dl, "sec_icon", ImVec2(p.x + 4 * K, p.y), ImVec2(p.x + 4 * K + iw, p.y + ih));
    const float tx = text(dl, g_font_skin, 25.f * K, ImVec2(p.x + 12 * K + iw, p.y), ImVec2(p.x + w, p.y + ih), c_(col("section", kGoldT)), upper(name).c_str(), 0.f);
    const float lx = p.x + 12 * K + iw + tx + 14 * K;
    if (has("sec_line") && lx < p.x + w - 40 * K) draw(dl, "sec_line", ImVec2(lx, p.y + ih * .5f - 7 * K), ImVec2(p.x + w - 4 * K, p.y + ih * .5f + 8 * K));
    else if (!has("sec_line")) dl->AddLine(ImVec2(p.x + 4 * K, p.y + ih + 3 * K), ImVec2(p.x + w - 6 * K, p.y + ih + 3 * K), c_(0xC8A04A, .75f), std::max(1.f, 1.5f * K));
    ImGui::Dummy(ImVec2(w, ih + (has("row") ? 4 : 8) * K));
}
// transparent ImGui widgets over the art
static void widget_colors_push() {
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0)); ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(255, 255, 255, 14));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(255, 255, 255, 22));
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0, 0, 0, 0)); ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(255, 255, 255, 18));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(255, 255, 255, 30));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, IM_COL32(0, 0, 0, 0)); ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Text, c_(kWhite)); ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.f);
}
static void widget_colors_pop() { ImGui::PopStyleVar(); ImGui::PopStyleColor(9); }
} // namespace sk

// a 'select' pick (shared by the classic combo and the skinned arrows)
static void select_pick(Item& it, size_t oi) {
    if (oi >= it.options.size()) return;
    const json& a = it.action; const std::string o = it.options[oi];
    set_text(it.text, o); save_state();
    if (a.contains("preview") && a["preview"].is_object() && sow_world_ready()) {   // review 2026-11-15: not while loading
        const std::string cmd = a["preview"].value("cmd", std::string()); json args = a["preview"].value("args", json::object());
        for (auto& kv : args.items()) { if (!kv.value().is_string()) continue; const std::string v = kv.value().get<std::string>();
            if (v == "{text}") kv.value() = o; else if (v.size() > 3 && v.compare(0, 2, "{@") == 0 && v.back() == '}') kv.value() = item_ref(v.substr(2, v.size() - 3)); }
        const std::string label = it.label;
        bridge_post([cmd, args, label]() { json r = dispatch(json{ {"cmd", cmd}, {"args", args} });
            if (!r.value("ok", false)) toast(label + " - " + r.value("message", std::string("preview failed")));
            else if (r.contains("data") && r["data"].value("preview", true) == false) toast(r["data"].value("why", std::string("no preview"))); });
    }
    if (a.contains("pick")) {
        std::string code = a["pick"].get<std::string>(); size_t p;
        while ((p = code.find("{i}")) != std::string::npos) code.replace(p, 3, std::to_string(oi + 1));
        while ((p = code.find("{text}")) != std::string::npos) code.replace(p, 6, "[==[" + o + "]==]");
        code = resolve_refs(code); post_main([code]() { lua_exec(code, false); });
    }
}

/* HOME2_2026_11_06 */
static void home_group(const char* title, const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ui::rgbv(ui::kGoldHi)); ImGui::TextUnformatted(title); ImGui::PopStyleColor();
    ImGui::PushTextWrapPos(0.f); ImGui::PushStyleColor(ImGuiCol_Text, ui::rgbv(ui::kBone)); ImGui::TextWrapped("%s", text); ImGui::PopStyleColor(); ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0, S(6)));
}
static void draw_home_tab() {
    ImGui::PushTextWrapPos(0.f);
    ImGui::PushStyleColor(ImGuiCol_Text, ui::rgbv(ui::kBone));
    ImGui::TextWrapped("Welcome to " WB_PRODUCT ", an in-game trainer for Middle-earth: Shadow of War. Everything runs inside the game and works through the game's own systems: skills are activated the way the skill menu does it, items arrive through the game's own inventory code, and gear changes are recalculated by the game itself.");
    ImGui::PopStyleColor(); ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0, S(6))); ui::gold_rule(.35f); ImGui::Dummy(ImVec2(0, S(6)));
    section_header("What it can do");
    home_group("Player", "Infinite health, Focus, Might, Elf-Shot and Elven Rage, no summon cooldown, Wraith Chain without aiming.");
    home_group("Skills", "Level, skill points, Mirian, every skill and upgrade at once, Raise the Dead, prestige for every gear slot.");
    home_group("Gear & Inventory", "Edit any item's level, effects, look and rarity, copy or remove gear, add any captain gear, auto-complete gear challenges.");
    home_group("Loot & Army", "Choose what captains drop. Edit followers, Instant Dominate, dominate captains at full health, create new orcs.");
    home_group("Stays on", "Options you switch on come back automatically once you are back in the game world.");
    section_header("Quick tips");
    ImGui::PushTextWrapPos(0.f); ImGui::PushStyleColor(ImGuiCol_Text, ui::rgbv(ui::kBone));
    ImGui::BulletText("Insert opens and closes the trainer.");
    ImGui::BulletText("Click the star next to any option to pin it to Favorites.");
    ImGui::BulletText("Hover any option to see what it does.");
    ImGui::BulletText("Make a manual save in a new slot before big changes.");
    ImGui::PopStyleColor(); ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0, S(10))); ui::gold_rule(.25f);
    ImGui::TextDisabled(WB_PRODUCT " - build %s   |   Shadow of War %s", __DATE__, game_version().c_str());
}
static void draw_favorites_tab() {
    if (!g_favs_loaded) favs_load();
    int shown = 0;
    for (auto fid : std::vector<std::string>(g_favs)) if (Item* it = find_item(fid)) { ImGui::PushID("fav"); draw_item(*it); ImGui::PopID(); ++shown; }
    if (!shown) { ImGui::Dummy(ImVec2(0, S(10))); ImGui::TextDisabled("Star any option with the \xE2\x98\x86 to pin it here."); }
}
json panel_favorites(const json& a) {
    std::lock_guard<std::recursive_mutex> lk(g_mtx);
    if (!g_favs_loaded) favs_load();
    std::string act = a.value("action", std::string("list")), id = a.value("id", std::string());
    if (act == "add" || act == "remove") {
        if (id.empty()) return fail("BAD_ARGS", "id required (panel item id, see PANEL.STATE)");
        if (act == "add" && !find_item(id)) return fail("NOT_FOUND", "no panel item with that id");
        set_fav(id, act == "add");
    } else if (act != "list") return fail("BAD_ARGS", "action list|add|remove");
    return ok({ {"favorites", g_favs} });
}


// 'fxlist': the rows of a bridge "get" (JSON array of {index, text, affix}). One check box per row marks the row that
// the next ADD replaces ({@id} = the checked index, -1 = none); the X on a row runs action.remove with {i} = its index.
// action.reset_on = an item id (the gear picker): another choice there clears the check.
static void fxlist_remove(Item& it, int idx) {
    if (!sow_world_ready()) { toast(it.label + " - wait until you are in the game world (loading)"); return; }   // review 2026-11-15
    json spec = it.action.value("remove", json::object()); json args = spec.value("args", json::object());
    for (auto& kv : args.items()) {
        if (!kv.value().is_string()) continue; const std::string s = kv.value().get<std::string>();
        if (s == "{i}") kv.value() = idx;
        else if (s.size() > 3 && s.compare(0, 2, "{@") == 0 && s.back() == '}') kv.value() = item_ref(s.substr(2, s.size() - 3));
    }
    if ((int)it.val == idx) it.val = -1; else if ((int)it.val > idx) it.val -= 1;
    const std::string cmd = spec.value("cmd", std::string()), label = it.label;
    bridge_post([cmd, args, label, idx]() {
        json r = dispatch(json{ {"cmd", cmd}, {"args", args} });
        toast(r.value("ok", false) ? "Removed effect #" + std::to_string(idx) : label + " - " + r.value("message", std::string("failed")));
    });
}
static void draw_fxlist(Item& it) {
    const json& a = it.action;
    if (a.contains("reset_on")) { const json d = item_ref(a["reset_on"].get<std::string>()); const std::string dv = d.is_string() ? d.get<std::string>() : d.dump();
        if (dv != it.text && dv.size() < sizeof it.text) { strcpy_s(it.text, dv.c_str()); it.val = -1; } }
    json rows = json::array();
    { std::string s; if (live_value(a, it.id, s) && !s.empty() && s[0] == '[') { try { rows = json::parse(s); } catch (...) {} } }
    if ((int)it.val >= (int)rows.size()) it.val = -1;
    const bool skin = sk::on();
    const float K = skin ? sk::K : g_ui_scale, w = ImGui::GetContentRegionAvail().x, rh = std::max(ImGui::GetFrameHeight() + 6 * K, 32 * K);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImFont* hf = (skin && g_font_skin_row) ? g_font_skin_row : g_font_body;
    {   const ImVec2 p = ImGui::GetCursorScreenPos();
        const int ck = (int)it.val;
        const std::string hint = ck >= 0 ? "Effect #" + std::to_string(ck) + " is checked: ADD will replace it" : "Check an effect to replace it with the next ADD.  X removes it.";
        const float hpx = (skin ? 20.f : 15.f) * K;
        dl->AddText(hf, hpx, ImVec2(p.x + 10 * K, p.y + 4 * K), ui::rgb(ui::kGold), it.label.c_str());
        const float lw = hf->CalcTextSizeA(hpx, FLT_MAX, 0, it.label.c_str()).x;
        dl->AddText(g_font_body, 14.f * K, ImVec2(p.x + 24 * K + lw, p.y + 7 * K), ui::rgb(ck >= 0 ? 0x7CD17C : 0x9A9384), hint.c_str());
        ImGui::Dummy(ImVec2(w, 30 * K)); }
    if (rows.empty()) { ImGui::TextDisabled("   (no effects)"); ImGui::Dummy(ImVec2(0, 4 * K)); return; }
    int remove = -1;
    for (size_t i = 0; i < rows.size(); ++i) {
        const json& r = rows[i]; const int idx = r.value("index", (int)i);
        std::string t = r.value("text", std::string()); if (t.empty()) t = r.value("affix", std::string("?"));
        t = "#" + std::to_string(idx) + "   " + t;
        ImGui::PushID((int)i);
        const ImVec2 p0 = ImGui::GetCursorScreenPos(), p1(p0.x + w, p0.y + rh);
        const bool checked = (int)it.val == idx, hov = ImGui::IsMouseHoveringRect(p0, p1);
        dl->AddRectFilled(ImVec2(p0.x + 4 * K, p0.y + 1 * K), ImVec2(p1.x - 4 * K, p1.y - 1 * K), checked ? IM_COL32(60, 120, 60, 70) : hov ? IM_COL32(255, 255, 255, 16) : IM_COL32(0, 0, 0, 60), 4 * K);
        dl->AddRect(ImVec2(p0.x + 4 * K, p0.y + 1 * K), ImVec2(p1.x - 4 * K, p1.y - 1 * K), ui::rgb(checked ? 0x7CD17C : ui::kLine, checked ? .8f : .5f), 4 * K);
        // check box
        const float cs = 22 * K; const ImVec2 ca(p0.x + 14 * K, p0.y + (rh - cs) * .5f), cb(ca.x + cs, ca.y + cs);
        ImGui::SetCursorScreenPos(ca);
        if (ImGui::InvisibleButton("##ck", ImVec2(cs, cs))) it.val = checked ? -1 : idx;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(checked ? "Un-check" : "Replace this effect with the next ADD");
        if (!(skin && sk::draw(dl, checked ? "check_on" : "check_off", ca, cb))) {
            dl->AddRect(ca, cb, ui::rgb(ui::kGold), 3 * K, 0, std::max(1.f, 1.5f * K));
            if (checked) { dl->AddLine(ImVec2(ca.x + cs * .2f, ca.y + cs * .55f), ImVec2(ca.x + cs * .42f, ca.y + cs * .78f), ui::rgb(0x7CD17C), 2.5f * K);
                           dl->AddLine(ImVec2(ca.x + cs * .42f, ca.y + cs * .78f), ImVec2(ca.x + cs * .82f, ca.y + cs * .25f), ui::rgb(0x7CD17C), 2.5f * K); }
        }
        // X = remove
        const float xs = 24 * K; const ImVec2 xa(p1.x - 14 * K - xs, p0.y + (rh - xs) * .5f), xb(xa.x + xs, xa.y + xs);
        ImGui::SetCursorScreenPos(xa);
        if (ImGui::InvisibleButton("##x", ImVec2(xs, xs))) remove = idx;
        const bool xh = ImGui::IsItemHovered(); if (xh) ImGui::SetTooltip("Remove this effect");
        dl->AddRectFilled(xa, xb, xh ? IM_COL32(170, 40, 35, 230) : IM_COL32(90, 25, 22, 200), 3 * K);
        const float m = xs * .3f;
        dl->AddLine(ImVec2(xa.x + m, xa.y + m), ImVec2(xb.x - m, xb.y - m), IM_COL32_WHITE, 2.f * K);
        dl->AddLine(ImVec2(xb.x - m, xa.y + m), ImVec2(xa.x + m, xb.y - m), IM_COL32_WHITE, 2.f * K);
        // text (clicking it checks too)
        const ImVec2 ta(cb.x + 12 * K, p0.y), tb(xa.x - 10 * K, p1.y);
        ImGui::SetCursorScreenPos(ta);
        if (tb.x > ta.x && ImGui::InvisibleButton("##t", ImVec2(tb.x - ta.x, rh))) it.val = checked ? -1 : idx;
        const float px = (skin ? 18.f : 15.f) * K; const float th = hf->CalcTextSizeA(px, FLT_MAX, 0, t.c_str()).y;
        dl->PushClipRect(ta, tb, true);
        dl->AddText(hf, px, ImVec2(ta.x, p0.y + (rh - th) * .5f), checked ? IM_COL32(200, 255, 200, 255) : ui::rgb(0xEDE6D3), t.c_str());
        dl->PopClipRect();
        ImGui::SetCursorScreenPos(ImVec2(p0.x, p1.y + 2 * K)); ImGui::Dummy(ImVec2(0, 0));
        ImGui::PopID();
    }
    // one removal per refresh: the rows are the previous poll's, a second X before the next poll would hit the wrong effect
    if (remove >= 0 && GetTickCount64() >= it.live_after) { fxlist_remove(it, remove); it.live_after = GetTickCount64() + 900; }
    ImGui::Dummy(ImVec2(0, 4 * K));
}

static void draw_item_skin(Item& it);
static void draw_item(Item& it) {
    ImGui::PushID(it.id.c_str());
    if (it.type == "fxlist") { draw_fxlist(it); ImGui::PopID(); return; }
    if (sk::on()) { draw_item_skin(it); ImGui::PopID(); return; }
    ImDrawList* row_dl = ImGui::GetWindowDrawList(); row_dl->ChannelsSplit(2); row_dl->ChannelsSetCurrent(1);
    const ImVec2 row_p0 = ImGui::GetCursorScreenPos(); ImGui::Dummy(ImVec2(0, S(1)));
    float avail = ImGui::GetContentRegionAvail().x;
    float ctrl_w = it.type == "toggle" ? ImGui::GetFrameHeight() * 0.78f * 1.85f
                 : it.type == "button" ? S(120.f) : it.type == "text" ? 0.f : std::min(S(260.f), avail * 0.52f);
    ImGui::AlignTextToFramePadding();
    if (it.type != "text") fav_star(it.id);
    if (it.type == "button") {
        ImGui::TextUnformatted(it.label.c_str());
    } else ImGui::TextUnformatted(it.label.c_str());
    if (!it.desc.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", it.desc.c_str());
    if (!it.last_err.empty()) {
        ImGui::SameLine(); ImGui::TextColored(ImVec4(1.f, 0.48f, 0.44f, 1), "!");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", it.last_err.c_str());
    }
    const json& a = it.action;
    bool is_mem = a.value("kind", "local") == "mem";
    bool is_cvar = a.value("kind", "local") == "cvar" || ((a.value("kind", "local") == "lua" || a.value("kind", "local") == "hks" || a.value("kind", "local") == "bridge") && a.contains("get"));
    if (it.type == "toggle") {
        ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - ctrl_w - S(104)); hotkey_box(it); ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - ctrl_w);
        bool v = it.on;
        if (toggle_switch("##sw", &v)) { it.on = v; apply(it, v ? "on" : "off"); save_state(); }
    } else if (it.type == "int" || it.type == "float") {
        bool has_lock = is_mem || is_cvar;
        float lock_w = has_lock ? ImGui::GetFrameHeight() + 6 : 0;
        ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - ctrl_w - lock_w);
        ImGui::SetNextItemWidth(ctrl_w);
        if (is_mem && !it.lock) { double live; std::string e; if (mem_read_value(a, live, e) && ImGui::GetActiveID() != ImGui::GetID("##v")) it.val = live; }
        if (is_cvar && !it.lock && it.live_ok && ImGui::GetActiveID() != ImGui::GetID("##v")) it.val = atof(it.live_str.c_str());
        bool changed = false;
        bool wide_range = (it.maxv - it.minv) > 5000;
        if (it.type == "int") {
            long long v = (long long)it.val, mn = (long long)it.minv, mx = (long long)it.maxv;
            if (wide_range) { long long stp = (long long)std::max(1.0, it.step);
                if (ImGui::InputScalar("##v", ImGuiDataType_S64, &v, &stp, nullptr, "%lld", ImGuiInputTextFlags_EnterReturnsTrue)) changed = true; }
            else changed = ImGui::SliderScalar("##v", ImGuiDataType_S64, &v, &mn, &mx, "%lld");
            if (changed) it.val = (double)std::clamp(v, mn, mx);
        } else {
            double v = it.val, mn = it.minv, mx = it.maxv;
            changed = wide_range ? ImGui::InputDouble("##v", &v, it.step, it.step * 10, "%.3f", ImGuiInputTextFlags_EnterReturnsTrue)
                                 : ImGui::SliderScalar("##v", ImGuiDataType_Double, &v, &mn, &mx, "%.2f");
            if (changed) it.val = std::clamp(v, mn, mx);
        }
        if (changed) { apply(it, "value"); save_state(); }
        if (has_lock) {
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, it.lock ? ImVec4(0.76f, 0.11f, 0.15f, 1) : ImGui::GetStyleColorVec4(ImGuiCol_Button));
            if (ImGui::Button(it.lock ? "L" : "-", ImVec2(ImGui::GetFrameHeight(), 0))) { it.lock = !it.lock; save_state(); }
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(it.lock ? "Locked: value is held every tick" : "Lock this value");
        }
    } else if (it.type == "button") {
        ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - ctrl_w - S(104)); hotkey_box(it); ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - ctrl_w);
        if (ImGui::Button(a.value("button", "Run").c_str(), ImVec2(ctrl_w, 0))) apply(it, "press");
    } else if (it.type == "input") {
        ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - ctrl_w);
        ImGui::SetNextItemWidth(ctrl_w - S(60));
        bool enter = ImGui::InputText("##t", it.text, sizeof it.text, ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if (ImGui::Button("Go", ImVec2(S(52), 0)) || enter) {
            json a2 = a; std::string cmd = a.value("cmd", "{text}"); size_t p = cmd.find("{text}");
            if (p != std::string::npos) cmd.replace(p, 6, it.text);
            a2["cmd"] = cmd; std::string err; run_action(it, a2, "press", err); it.last_err = err; save_state();
        }
    } else if (it.type == "select") {
        ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - ctrl_w);
        ImGui::SetNextItemWidth(ctrl_w);
        if (a.contains("options")) { std::string ol; if (live_value(a, it.id + "#opts", ol)) { it.options.clear(); size_t p = 0; while (p < ol.size()) { size_t q = ol.find('\n', p); if (q == std::string::npos) q = ol.size(); if (q > p) it.options.push_back(ol.substr(p, q - p)); p = q + 1; } } }
        const char* shown = (a.contains("get") && it.live_ok && !it.live_str.empty()) ? it.live_str.c_str() : it.text;
        if (ImGui::BeginCombo("##sel", shown, ImGuiComboFlags_HeightLarge)) {
            static char filter[64] = {};
            if (ImGui::IsWindowAppearing()) { filter[0] = 0; ImGui::SetKeyboardFocusHere(); }
            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextWithHint("##flt", "filter...", filter, sizeof filter);
            std::string f = filter; std::transform(f.begin(), f.end(), f.begin(), ::tolower);
            for (size_t oi = 0; oi < it.options.size(); ++oi) {
                const std::string& o = it.options[oi];
                std::string lo = o; std::transform(lo.begin(), lo.end(), lo.begin(), ::tolower);
                if (!f.empty() && lo.find(f) == std::string::npos) continue;
                ImGui::PushID((int)oi);
                if (ImGui::Selectable(o.c_str(), o == shown)) select_pick(it, oi);
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
    } else if (it.type == "text") {
        if (is_mem) { double live; std::string e; ImGui::SameLine(); if (mem_read_value(a, live, e)) ImGui::TextDisabled("%g", live); else ImGui::TextDisabled("--"); }
        if (is_cvar) {
            if (it.live_ok && it.live_str.find('\n') != std::string::npos) { ImGui::PushStyleColor(ImGuiCol_Text, ui::rgbv(ui::kDim)); ImGui::TextUnformatted(it.live_str.c_str()); ImGui::PopStyleColor(); }
            else { ImGui::SameLine(); ImGui::TextDisabled("%s", it.live_ok ? it.live_str.c_str() : "--"); }
        }
    }
    if (!it.desc.empty()) { ImGui::PushStyleColor(ImGuiCol_Text, ui::rgbv(ui::kDim)); ImGui::PushTextWrapPos(ImGui::GetWindowContentRegionMax().x - S(70)); ImGui::TextUnformatted(it.desc.c_str()); ImGui::PopTextWrapPos(); ImGui::PopStyleColor(); }
    ImGui::Dummy(ImVec2(0, S(1)));
    {   const ImVec2 row_p1(ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x + S(5), ImGui::GetCursorScreenPos().y - S(3));
        row_dl->ChannelsSetCurrent(0);
        const bool hov = ImGui::IsMouseHoveringRect(ImVec2(row_p0.x - S(7), row_p0.y - S(4)), row_p1);
        row_dl->AddRectFilledMultiColor(ImVec2(row_p0.x - S(7), row_p0.y - S(4)), row_p1, ui::rgb(hov ? 0x22231F : 0x16171A, .92f), ui::rgb(hov ? 0x1A1B1E : 0x101114, .80f), ui::rgb(hov ? 0x1A1B1E : 0x101114, .80f), ui::rgb(hov ? 0x22231F : 0x16171A, .92f));
        row_dl->AddRect(ImVec2(row_p0.x - S(7), row_p0.y - S(4)), row_p1, ui::rgb(hov ? ui::kGold : ui::kLine, hov ? .45f : .7f));
        if (it.type == "toggle" && it.on) row_dl->AddRectFilled(ImVec2(row_p0.x - S(7), row_p0.y - S(4)), ImVec2(row_p0.x - S(4), row_p1.y), ui::rgb(ui::kGold));
        row_dl->ChannelsMerge(); }
    ImGui::Dummy(ImVec2(0, S(2)));
    ImGui::PopID();
}

// ---- skinned row: star + label left; hotkey badge + control right; thin gold line under it
static void hk_badge(Item& it, ImVec2 a, ImVec2 b) {
    using namespace sk;
    if (!g_hk_loaded) hk_load();
    const bool cap = g_hk_capture == it.id; auto f = g_hk.find(it.id);
    ImGui::SetCursorScreenPos(a);
    if (ImGui::InvisibleButton("##hk", ImVec2(b.x - a.x, b.y - a.y))) { g_hk_capture = cap ? "" : it.id; for (int k = 0; k < 256; ++k) g_hk_prev[k] = (GetAsyncKeyState(k) & 0x8000) != 0; }
    const bool hov = ImGui::IsItemHovered();
    if (hov) ImGui::SetTooltip(cap ? "Press the key to use. Backspace/Delete clears, Esc cancels." : "Hotkey: click, then press a key");
    const bool has = f != g_hk.end();
    if (!cap && !has && !hov) return;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    draw(dl, "field", a, b, hov ? IM_COL32(255, 255, 255, 255) : IM_COL32(220, 220, 220, 255));
    text(dl, g_font_skin_row, 15.f * K, a, b, c_(cap ? kGoldL : has ? kGoldT : 0x9A9384), cap ? "press a key" : has ? key_name(f->second).c_str() : "set key", .5f, false);
}
static void draw_item_skin(Item& it) {
    using namespace sk;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float fh = ImGui::GetFrameHeight();
    const bool multi = it.type == "text" && it.live_ok && it.live_str.find('\n') != std::string::npos;
    const bool plates = has("row");                       // Shadow of War: a plate per row
    const float base_h = plates ? std::max(L("row_h", 42), fh + 8.f * K) : std::max(34.f * K, fh + 6.f * K);
    float rowh = base_h;
    std::vector<std::string> lines;
    if (multi) { size_t p = 0; while (p < it.live_str.size()) { size_t q = it.live_str.find('\n', p); if (q == std::string::npos) q = it.live_str.size(); lines.push_back(it.live_str.substr(p, q - p)); p = q + 1; }
                 rowh += lines.size() * 18.f * K; }
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    const ImVec2 p1(p0.x + w, p0.y + rowh);
    const float top = p0.y, rh1 = base_h;                                                // first line of the row
    const bool hov = ImGui::IsMouseHoveringRect(p0, p1) && ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    if (plates) {
        draw(dl, "row", p0, p1, hov ? IM_COL32(255, 255, 255, 255) : IM_COL32(222, 222, 222, 255));
        if (hov) dl->AddRectFilled(ImVec2(p0.x + 8 * K, p0.y + 3 * K), ImVec2(p1.x - 10 * K, p1.y - 3 * K), IM_COL32(120, 255, 150, 8), 4 * K);
    } else {
        if (hov) dl->AddRectFilled(p0, p1, IM_COL32(255, 255, 255, 10));
        dl->AddLine(ImVec2(p0.x + 6 * K, p1.y - 1), ImVec2(p1.x - 6 * K, p1.y - 1), c_(0xC8A04A, .22f), 1.f);
    }
    const json& a = it.action;
    const bool is_mem = a.value("kind", "local") == "mem";
    const bool is_cvar = a.value("kind", "local") == "cvar" || ((a.value("kind", "local") == "lua" || a.value("kind", "local") == "hks" || a.value("kind", "local") == "bridge") && a.contains("get"));
    const float cr = p1.x - (plates ? 20 : 12) * K;       // right edge of the control
    const float cy = top + (rh1 - fh) * .5f;               // ImGui widgets' y
    float ctrl_l = cr;
    bool lit = false;
    const float label_x = plates ? 82.f : 34.f;            // reference px from the row's left edge
    // row icon (click = favourite, gold star in its corner when it is one)
    std::string ri = it.icon.empty() ? std::string() : "ri_" + it.icon;
    if (plates && !ri.empty() && has(ri.c_str())) {
        const float sz = 30.f * K; const ImVec2 sp(p0.x + 22 * K, top + (rh1 - sz) * .5f);
        ImGui::SetCursorScreenPos(sp);
        const bool f = is_fav(it.id);
        if (it.type != "text" && ImGui::InvisibleButton("##fav", ImVec2(sz, sz))) set_fav(it.id, !f);
        const bool sh = ImGui::IsItemHovered() && it.type != "text";
        if (sh) ImGui::SetTooltip(f ? "Remove from Favorites" : "Add to Favorites");
        draw(dl, ri.c_str(), sp, ImVec2(sp.x + sz, sp.y + sz), sh ? IM_COL32(255, 255, 255, 255) : IM_COL32(225, 225, 225, 255));
        if (f) { const float s = 9.f * K; ImVec2 c(sp.x + sz - s * .3f, sp.y + s * .6f), pts[10];
                 for (int i = 0; i < 10; ++i) { float r = (i & 1) ? s * 0.22f : s * 0.5f; float an = -1.5707963f + i * 0.6283185f; pts[i] = ImVec2(c.x + cosf(an) * r, c.y + sinf(an) * r); }
                 dl->AddConvexPolyFilled(pts, 10, c_(kGoldT)); }
    } else
    // favourite star
    {   const float sz = 16.f * K; const ImVec2 sp(p0.x + (plates ? 30 : 10) * K, top + (rh1 - sz) * .5f);
        ImGui::SetCursorScreenPos(sp);
        const bool f = is_fav(it.id);
        if (it.type != "text" && ImGui::InvisibleButton("##fav", ImVec2(sz, sz))) set_fav(it.id, !f);
        const bool sh = ImGui::IsItemHovered() && it.type != "text";
        if (sh) ImGui::SetTooltip(f ? "Remove from Favorites" : "Add to Favorites");
        if (it.type != "text") {
            ImVec2 c(sp.x + sz * .5f, sp.y + sz * .5f), pts[10];
            for (int i = 0; i < 10; ++i) { float r = (i & 1) ? sz * 0.22f : sz * 0.5f; float an = -1.5707963f + i * 0.6283185f; pts[i] = ImVec2(c.x + cosf(an) * r, c.y + sinf(an) * r); }
            const ImU32 col = f ? c_(kGoldT) : sh ? c_(kCream) : c_(0x6E6858);
            if (f) dl->AddConvexPolyFilled(pts, 10, col); else dl->AddPolyline(pts, 10, col, ImDrawFlags_Closed, std::max(1.f, 1.1f * K));
        } }
    const float hk_w = 92 * K, hk_h = 24 * K;
    auto hotkey_col = [&](float right) { const ImVec2 ha(right - hk_w, top + (rh1 - hk_h) * .5f); hk_badge(it, ha, ImVec2(right, ha.y + hk_h)); return ha.x; };

    if (it.type == "toggle") {
        const bool box = has("check_on") && it.style != "switch";
        const ImVec2 ss = box ? ImVec2(L("check", 30), L("check", 30)) : ImVec2(L("switch_w", 61), L("switch_h", 25));
        const ImVec2 sa(cr - ss.x, top + (rh1 - ss.y) * .5f);
        draw(dl, box ? (it.on ? "check_on" : "check_off") : (it.on ? "switch_on" : "switch_off"), sa, ImVec2(sa.x + ss.x, sa.y + ss.y));
        const float hx = hotkey_col(sa.x - 14 * K);
        ImGui::SetCursorScreenPos(ImVec2(p0.x + label_x * K, top));                     // label + switch both toggle
        if (ImGui::InvisibleButton("##row", ImVec2(std::max(1.f, hx - 8 * K - (p0.x + label_x * K)), rh1))) { it.on = !it.on; apply(it, it.on ? "on" : "off"); save_state(); }
        ImGui::SetCursorScreenPos(sa);
        if (ImGui::InvisibleButton("##sw", ss)) { it.on = !it.on; apply(it, it.on ? "on" : "off"); save_state(); }
        ctrl_l = hx; lit = it.on;
    } else if (it.type == "int" || it.type == "float") {
        const bool has_lock = is_mem || is_cvar;
        if (is_mem && !it.lock) { double live; std::string e; if (mem_read_value(a, live, e) && ImGui::GetActiveID() != ImGui::GetID("##v") && ImGui::GetActiveID() != ImGui::GetID("##f")) it.val = live; }
        if (is_cvar && !it.lock && it.live_ok && ImGui::GetActiveID() != ImGui::GetID("##v") && ImGui::GetActiveID() != ImGui::GetID("##f")) it.val = atof(it.live_str.c_str());
        const bool wide = (it.maxv - it.minv) > 5000;
        const bool spin = has("spin");                    // value box with up/down arrows in its right cap
        const float fw = spin ? L("spin_w", 106) * (wide ? 1.5f : 1.f) : L("field_w", 165) * .85f, fhh = spin ? std::min(L("spin_h", 34), rh1 - 6 * K) : 26 * K;
        const ImVec2 fa(cr - fw, top + (rh1 - fhh) * .5f), fb(cr, fa.y + fhh);
        draw(dl, spin ? "spin" : "field", fa, fb);
        bool changed = false;
        float arw = 0;
        if (spin) {
            const Piece& sp = g_p["spin"]; arw = sp.r * fhh / std::max(1.f, sp.h);
            const double st = it.step > 0 ? it.step : it.type == "int" ? 1.0 : 0.1;
            ImGui::SetCursorScreenPos(ImVec2(fb.x - arw, fa.y));
            if (ImGui::InvisibleButton("##up", ImVec2(arw, fhh * .5f))) { it.val = std::clamp(it.val + st, it.minv, it.maxv); changed = true; }
            if (ImGui::IsItemHovered()) dl->AddRectFilled(ImVec2(fb.x - arw, fa.y + 2 * K), ImVec2(fb.x - 2 * K, fa.y + fhh * .5f), IM_COL32(255, 255, 255, 25));
            ImGui::SetCursorScreenPos(ImVec2(fb.x - arw, fa.y + fhh * .5f));
            if (ImGui::InvisibleButton("##dn", ImVec2(arw, fhh * .5f))) { it.val = std::clamp(it.val - st, it.minv, it.maxv); changed = true; }
            if (ImGui::IsItemHovered()) dl->AddRectFilled(ImVec2(fb.x - arw, fa.y + fhh * .5f), ImVec2(fb.x - 2 * K, fb.y - 2 * K), IM_COL32(255, 255, 255, 25));
        }
        widget_colors_push();
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6 * K, ImGui::GetStyle().FramePadding.y));
        ImGui::SetCursorScreenPos(ImVec2(fa.x + 6 * K, cy)); ImGui::SetNextItemWidth(fw - 12 * K - arw);
        if (it.type == "int") { long long v = (long long)it.val;
            if (ImGui::InputScalar("##f", ImGuiDataType_S64, &v, nullptr, nullptr, "%lld", ImGuiInputTextFlags_EnterReturnsTrue)) { it.val = (double)std::clamp(v, (long long)it.minv, (long long)it.maxv); changed = true; } }
        else { double v = it.val;
            if (ImGui::InputDouble("##f", &v, 0, 0, "%.2f", ImGuiInputTextFlags_EnterReturnsTrue)) { it.val = std::clamp(v, it.minv, it.maxv); changed = true; } }
        ImGui::PopStyleVar();
        float left = fa.x - 14 * K;
        if (!wide) {                                                                     // slider beside the value
            const float sw = std::min(L("slider_w", 330), std::max(80.f * K, (left - (p0.x + 260 * K)))), th = (spin ? 14 : 16) * K;
            const ImVec2 ta(left - sw, top + (rh1 - th) * .5f), tb(left, ta.y + th);
            const float frac = (float)std::clamp((it.val - it.minv) / std::max(1e-9, it.maxv - it.minv), 0.0, 1.0);
            draw(dl, "track_empty", ta, tb);
            if (frac > 0.002f) draw(dl, "track_fill", ta, ImVec2(ta.x + std::max(8 * K, sw * frac), tb.y));
            const ImVec2 ks = spin ? ImVec2(24 * K, 25 * K) : ImVec2(22 * K, 27 * K); const float kx = ta.x + sw * frac - ks.x * .5f;
            draw(dl, "knob", ImVec2(kx, top + (rh1 - ks.y) * .5f), ImVec2(kx + ks.x, top + (rh1 + ks.y) * .5f));
            ImGui::SetCursorScreenPos(ImVec2(ta.x, cy)); ImGui::SetNextItemWidth(sw);
            if (it.type == "int") { long long v = (long long)it.val, mn = (long long)it.minv, mx = (long long)it.maxv;
                if (ImGui::SliderScalar("##v", ImGuiDataType_S64, &v, &mn, &mx, "")) { it.val = (double)v; changed = true; } }
            else { double v = it.val, mn = it.minv, mx = it.maxv;
                if (ImGui::SliderScalar("##v", ImGuiDataType_Double, &v, &mn, &mx, "")) { it.val = v; changed = true; } }
            left = ta.x - 12 * K;
        }
        widget_colors_pop();
        if (changed) { apply(it, "value"); save_state(); }
        if (has_lock) {                                                                  // lock: value held every tick
            const float ls = 24 * K; const ImVec2 la(left - ls, top + (rh1 - ls) * .5f);
            ImGui::SetCursorScreenPos(la);
            if (ImGui::InvisibleButton("##lock", ImVec2(ls, ls))) { it.lock = !it.lock; save_state(); }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(it.lock ? "Locked: value is held every tick" : "Lock this value");
            draw(dl, "field", la, ImVec2(la.x + ls, la.y + ls), it.lock ? IM_COL32(255, 255, 255, 255) : IM_COL32(180, 180, 180, 255));
            text(dl, g_font_skin, 15.f * K, la, ImVec2(la.x + ls, la.y + ls), c_(it.lock ? kGoldL : 0x8A8474), "L", .5f, false);
            left = la.x - 10 * K;
        }
        ctrl_l = left; lit = it.lock;
    } else if (it.type == "button") {
        const std::string cap = upper(a.value("button", std::string("Run")));
        const ImVec2 bs(std::max(L("button_w", 170) * .8f, tw(g_font_skin, 17.f * K, cap.c_str()) + 40 * K), L("button_h", 30) > 0 ? L("button_h", 30) : 30 * K);
        const ImVec2 ba(cr - bs.x, top + (rh1 - bs.y) * .5f);
        if (plate_button("##b", cap.c_str(), ba, bs, it.style == "primary")) apply(it, "press");
        ctrl_l = hotkey_col(ba.x - 14 * K);
    } else if (it.type == "input") {
        const float gw = 54 * K, fw = std::min(300 * K, w * .42f), fhh = 26 * K;
        const ImVec2 ga(cr - gw, top + (rh1 - 28 * K) * .5f);
        const ImVec2 fa(ga.x - 8 * K - fw, top + (rh1 - fhh) * .5f), fb(fa.x + fw, fa.y + fhh);
        draw(dl, "field", fa, fb);
        widget_colors_push();
        ImGui::SetCursorScreenPos(ImVec2(fa.x + 6 * K, cy)); ImGui::SetNextItemWidth(fw - 12 * K);
        const bool enter = ImGui::InputText("##t", it.text, sizeof it.text, ImGuiInputTextFlags_EnterReturnsTrue);
        widget_colors_pop();
        if (plate_button("##go", "GO", ga, ImVec2(gw, 28 * K)) || enter) {
            json a2 = a; std::string cmd = a.value("cmd", "{text}"); size_t pp = cmd.find("{text}");
            if (pp != std::string::npos) cmd.replace(pp, 6, it.text);
            a2["cmd"] = cmd; std::string err; run_action(it, a2, "press", err); it.last_err = err; save_state();
        }
        ctrl_l = fa.x - 12 * K;
    } else if (it.type == "select") {
        if (a.contains("options")) { std::string ol; if (live_value(a, it.id + "#opts", ol)) { it.options.clear(); size_t pp = 0; while (pp < ol.size()) { size_t q = ol.find('\n', pp); if (q == std::string::npos) q = ol.size(); if (q > pp) it.options.push_back(ol.substr(pp, q - pp)); pp = q + 1; } } }
        const char* shown = (a.contains("get") && it.live_ok && !it.live_str.empty()) ? it.live_str.c_str() : it.text;
        const float vw = std::min(std::max(L("valbox_w", 200) * 1.5f, w * .42f), w * .6f), vh = 26 * K, cap = 30 * K;
        const ImVec2 va(cr - vw, top + (rh1 - vh) * .5f), vb(cr, va.y + vh);
        if (!draw(dl, "valbox", va, vb)) {                 // no left/right value box in this art: field + drawn arrows
            draw(dl, "field", va, vb);
            const float ah = vh * .22f, ax0 = va.x + cap * .5f, ax1 = vb.x - cap * .5f, ay = (va.y + vb.y) * .5f;
            dl->AddTriangleFilled(ImVec2(ax0 + ah * .6f, ay - ah), ImVec2(ax0 + ah * .6f, ay + ah), ImVec2(ax0 - ah * .6f, ay), c_(col("label", kCream)));
            dl->AddTriangleFilled(ImVec2(ax1 - ah * .6f, ay - ah), ImVec2(ax1 - ah * .6f, ay + ah), ImVec2(ax1 + ah * .6f, ay), c_(col("label", kCream)));
        }
        int cur = -1; for (size_t i = 0; i < it.options.size(); ++i) if (it.options[i] == shown) { cur = (int)i; break; }
        const int n = (int)it.options.size();
        ImGui::SetCursorScreenPos(va);
        if (ImGui::InvisibleButton("##prev", ImVec2(cap, vh)) && n) select_pick(it, (size_t)((cur < 0 ? 0 : cur + n - 1) % n));
        ImGui::SetCursorScreenPos(ImVec2(vb.x - cap, va.y));
        if (ImGui::InvisibleButton("##next", ImVec2(cap, vh)) && n) select_pick(it, (size_t)((cur + 1) % n));
        dl->PushClipRect(ImVec2(va.x + cap, va.y), ImVec2(vb.x - cap, vb.y), true);
        text(dl, g_font_skin_row, 18.f * K, ImVec2(va.x + cap, va.y), ImVec2(vb.x - cap, vb.y), c_(kWhite), shown, tw(g_font_skin_row, 18.f * K, shown) > vw - 2 * cap ? 0.f : .5f, false);
        dl->PopClipRect();
        widget_colors_push();
        ImGui::SetCursorScreenPos(ImVec2(va.x + cap, cy)); ImGui::SetNextItemWidth(vw - 2 * cap);
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0, 0, 0, 0));
        const bool open = ImGui::BeginCombo("##sel", shown, ImGuiComboFlags_HeightLarge | ImGuiComboFlags_NoArrowButton);
        ImGui::PopStyleColor();
        widget_colors_pop();
        if (open) {
            static char filter[64] = {};
            if (ImGui::IsWindowAppearing()) { filter[0] = 0; ImGui::SetKeyboardFocusHere(); }
            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextWithHint("##flt", "filter...", filter, sizeof filter);
            std::string f = filter; std::transform(f.begin(), f.end(), f.begin(), ::tolower);
            for (size_t oi = 0; oi < it.options.size(); ++oi) {
                std::string lo = it.options[oi]; std::transform(lo.begin(), lo.end(), lo.begin(), ::tolower);
                if (!f.empty() && lo.find(f) == std::string::npos) continue;
                ImGui::PushID((int)oi);
                if (ImGui::Selectable(it.options[oi].c_str(), it.options[oi] == shown)) select_pick(it, oi);
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        ctrl_l = va.x - 12 * K;
    } else if (it.type == "text" && !multi) {
        std::string v = "--";
        if (is_mem) { double live; std::string e; if (mem_read_value(a, live, e)) { char b[64]; sprintf_s(b, "%g", live); v = b; } }
        if (is_cvar && it.live_ok) v = it.live_str;
        const float vw = std::min(tw(g_font_skin_row, 18.f * K, v.c_str()) + 6 * K, w * .55f);
        dl->PushClipRect(ImVec2(cr - vw, top), ImVec2(cr, top + rh1), true);
        text(dl, g_font_skin_row, 18.f * K, ImVec2(cr - vw, top), ImVec2(cr, top + rh1), c_(kGoldL), v.c_str(), 1.f, false);
        dl->PopClipRect();
        ctrl_l = cr - vw - 12 * K;
    }
    // label
    {   const ImVec2 la(p0.x + label_x * K, top), lb(std::max(p0.x + (label_x + 26) * K, ctrl_l - 8 * K), top + rh1);
        dl->PushClipRect(la, lb, true);
        const float lw = text(dl, g_font_skin_row, (plates ? 22.f : 21.f) * K, la, lb, c_(plates ? (lit ? 0xFFFFFF : col("label", kWhite)) : (lit ? kGoldL : kWhite)), it.label.c_str(), 0.f);
        if (!it.last_err.empty()) text(dl, g_font_skin, 17.f * K, ImVec2(la.x + lw + 8 * K, top), lb, c_(0xFF6A5A), "!", 0.f);
        dl->PopClipRect();
        if (ImGui::IsMouseHoveringRect(la, lb) && ImGui::IsWindowHovered()) {
            std::string tip = it.desc; if (!it.last_err.empty()) tip += (tip.empty() ? "" : "\n\n") + std::string("! ") + it.last_err;
            if (!tip.empty()) ImGui::SetTooltip("%s", tip.c_str());
        } }
    if (multi) { float y = top + rh1 - 4 * K;
        for (auto& l : lines) { text(dl, g_font_skin_row, 15.f * K, ImVec2(p0.x + 40 * K, y), ImVec2(p1.x - 10 * K, y + 18 * K), c_(0xA8A08C), l.c_str(), 0.f, false); y += 18 * K; } }
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p1.y + (plates ? L("row_gap", 2) : 2 * K)));
    ImGui::Dummy(ImVec2(0, 0));
}

static void draw_bridge_tab() {
    if (sk::g_ok) {                                       // look settings (the skin needs bin\skin)
        section_header("Look");
        bool classic = sk::g_classic;
        if (ImGui::Checkbox("Classic look", &classic)) { sk::g_classic = classic; sk::save_cfg(); }
        if (!sk::g_classic) { ImGui::SameLine(0, S(30)); ImGui::SetNextItemWidth(S(220));
            float s = sk::g_user; if (ImGui::SliderFloat("Panel size", &s, 0.6f, 1.4f, "%.2f")) { sk::g_user = s; }
            if (ImGui::IsItemDeactivatedAfterEdit()) sk::save_cfg(); }
    }
    section_header("About");
    ImGui::Text("Game version"); ImGui::SameLine(S(160)); ImGui::TextDisabled("%s", sow_game_supported() ? "1.0.8874 (supported)" : "not supported - trainer features are off");
    ImGui::Text("Open / close"); ImGui::SameLine(S(160)); ImGui::TextDisabled("Insert");
    section_header("Panel");
    if (ImGui::Button("Reload panel.json", ImVec2(S(180), 0))) panel_reload();
    ImGui::SameLine(); ImGui::TextDisabled("%s", g_load_msg.c_str());
    section_header("Log");
    ImGui::BeginChild("##log", ImVec2(0, 0), ImGuiChildFlags_Borders);
    for (auto& l : log_tail(120)) ImGui::TextUnformatted(l.c_str());
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.f);
    ImGui::EndChild();
}

static void draw_console_tab() {
    section_header("Console");
    ImGui::TextDisabled(false ? "Runs CryEngine console commands and cvars."
                                                      : "No console in this game.");
    ImGui::SetNextItemWidth(-S(70));
    bool enter = ImGui::InputText("##con", g_console_line, sizeof g_console_line, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if ((ImGui::Button("Run", ImVec2(S(60), 0)) || enter) && g_console_line[0]) {
        std::string cmd = g_console_line;
        g_console_hist.push_back(cmd);
        post_main([cmd]() { console_exec(cmd); });
        g_console_line[0] = 0;
        ImGui::SetKeyboardFocusHere(-1);
    }
    ImGui::BeginChild("##hist", ImVec2(0, 0), ImGuiChildFlags_Borders);
    for (auto it = g_console_hist.rbegin(); it != g_console_hist.rend(); ++it) {
        if (ImGui::Selectable(it->c_str())) strcpy_s(g_console_line, it->c_str());
    }
    ImGui::EndChild();
}


// ------------------------------------------------------------------ skinned panel (whole art at one scale K)
static void panel_draw_skin(bool* visible) {
    using namespace sk;
    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    const float RW = g_lay.value("ref_w", 1620.f), RH = g_lay.value("ref_h", 971.f);
    K = std::clamp(std::min(ds.y * (RH > RW ? .88f : .72f) / RH, ds.x * .72f / RW) * g_user, 0.45f, 2.2f);   // tall art (SoW 1122x1402) may use more height
    ImGui::SetNextWindowSize(ImVec2(RW * K, RH * K), ImGuiCond_Always);
    ImGui::SetNextWindowPos(ImVec2((ds.x - RW * K) * .5f, (ds.y - RH * K) * .5f), ImGuiCond_FirstUseEver);
    ui::theme_push();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    ImGui::PushFont(g_font_body);
    const bool open = ImGui::Begin("##sow_skin", visible, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBackground |
                                                         ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoResize);
    ImGui::PopStyleVar(2);
    if (!open) { ImGui::End(); ImGui::PopFont(); ui::theme_pop(); return; }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    O = ImGui::GetWindowPos();
    draw(dl, "frame", O, ImVec2(O.x + RW * K, O.y + RH * K));

    // window buttons baked into the art: gear = Bridge page, minimise / close = hide
    const int ntab = (int)g_tabs.size(), kBridge = ntab;
    {   struct Wb { const char* key; const char* tip; int act; } wbs[] = { {"win_gear", "Bridge & look settings", 0}, {"win_min", "Hide (Insert)", 1}, {"win_close", "Hide (Insert)", 1} };
        for (auto& b : wbs) { const ImVec4 r = box(b.key); if (r.z <= r.x) continue;
            ImGui::SetCursorScreenPos(ImVec2(r.x, r.y));
            if (ImGui::InvisibleButton(b.key, ImVec2(r.z - r.x, r.w - r.y))) { if (b.act == 0) g_tab = kBridge; else *visible = false; }
            if (ImGui::IsItemHovered()) { dl->AddRect(ImVec2(r.x, r.y), ImVec2(r.z, r.w), c_(kGoldL, .6f), 6 * K, 0, std::max(1.f, 1.5f * K)); ImGui::SetTooltip("%s", b.tip); } } }

    // tabs (spec pages + Bridge)
    {   const int total = ntab + 1;
        if (!ImGui::IsAnyItemActive()) {
            if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false)) g_tab = (g_tab + total - 1) % total;
            if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false)) g_tab = (g_tab + 1) % total;
        }
        if (g_tab >= total) g_tab = 0;
        const float x = O.x + L("tabs_x", 298), y0 = O.y + L("tabs_y", 247), tw_ = L("tab_w", 270), th = L("tab_h", 55);
        const float step = std::min(L("tab_step", 53.6f), (O.y + L("tabs_bottom", 790) - y0) / std::max(1, total));
        const float h = std::min(th, step - 2 * K);
        const json ico = g_lay.contains("tab_icons") ? g_lay["tab_icons"] : json::object();
        for (int i = 0; i < total; ++i) {
            const bool sel = g_tab == i;
            const std::string name = i < ntab ? g_tabs[i].name : std::string(kSettingsTabName), up = upper(name);
            const ImVec2 a(x, y0 + i * step), b(x + tw_, a.y + h);
            ImGui::SetCursorScreenPos(a); ImGui::PushID(i);
            if (ImGui::InvisibleButton("##tab", ImVec2(tw_, h))) g_tab = i;
            const bool hov = ImGui::IsItemHovered(); ImGui::PopID();
            draw(dl, sel ? "tab_active" : "tab", a, b, hov && !sel ? IM_COL32(255, 255, 255, 255) : IM_COL32(232, 232, 232, 255));
            const std::string icn = ico.contains(up) && ico[up].is_string() ? ico[up].get<std::string>() : std::string("ico_utility");
            const float isz = std::min(h - 8 * K, L("tab_icon_size", 46)), icx = a.x + L("tab_icon_cx", 42);
            draw(dl, icn.c_str(), ImVec2(icx - isz * .5f, a.y + (h - isz) * .5f), ImVec2(icx + isz * .5f, a.y + (h + isz) * .5f));
            text(dl, g_font_skin, std::min(22.f * K, h * .48f), ImVec2(a.x + L("tab_text_x", 82), a.y), ImVec2(b.x - 14 * K, b.y),
                 c_(sel ? col("tab_text_sel", 0xFFFFFF) : hov ? col("accent", kGoldL) : col("tab_text", kCream)), up.c_str(), 0.f);
        } }

    // page title on the red plate + search beside it
    {   const ImVec4 tb = box("title_box");
        const std::string title = upper(g_tab < ntab ? g_tabs[g_tab].name : std::string(kSettingsTabName));
        float px = (has("row") ? 40.f : 28.f) * K; while (px > 12.f * K && tw(g_font_skin_big, px, title.c_str()) > (tb.z - tb.x)) px -= 1.f;
        text(dl, g_font_skin_big, px, ImVec2(tb.x, tb.y), ImVec2(tb.z, tb.w), c_(col("title", kGoldL)), title.c_str(), .5f);
        if (g_tab < ntab) {
            const ImVec4 sbx = box("search_box");
            const bool own = sbx.z > sbx.x;
            const ImVec2 sa = own ? ImVec2(sbx.x, sbx.y) : ImVec2(O.x + 604 * K, O.y + 249 * K);
            const float sw = own ? sbx.z - sbx.x : 240 * K, sh = own ? sbx.w - sbx.y : 24 * K;
            const float mag = own && has("search") ? g_p["search"].r * sh / std::max(1.f, g_p["search"].h) : 0.f;
            draw(dl, has("search") ? "search" : "field", sa, ImVec2(sa.x + sw, sa.y + sh));
            widget_colors_push();
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12 * K, std::max(1.f, (sh - ImGui::GetFontSize()) * .5f)));
            ImGui::SetCursorScreenPos(sa); ImGui::SetNextItemWidth(sw - mag);
            ImGui::InputTextWithHint("##search", "Search...", g_search, sizeof g_search);
            ImGui::PopStyleVar(); widget_colors_pop();
        } }

    // content: a transparent child inside the content box
    {   const ImVec4 cb = box("content");
        const float x0 = cb.x + 8 * K, y0 = O.y + L("rows_top", 276), x1 = cb.z - 6 * K, y1 = cb.w - 6 * K;
        ImGui::SetCursorScreenPos(ImVec2(x0, y0));
        ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, IM_COL32(0, 0, 0, 50)); ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, c_(col("accent", 0xC8A04A), .55f));
        ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, c_(col("accent", 0xC8A04A), .8f)); ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, c_(col("accent", kGoldT)));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6 * K, 4 * K));
        ImGui::BeginChild("##skc", ImVec2(x1 - x0, y1 - y0), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoBackground);
        // widgets and plain text in the rows: Segoe UI scaled to the art (~19 reference px), tight vertical spacing
        ImFont* rf = g_font_skin_row ? g_font_skin_row : g_font_body;
        ImGui::PushFont(rf); ImGui::SetWindowFontScale(19.f * K / rf->FontSize);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8 * K, 3 * K));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6 * K, 3 * K));
        if (g_tab < ntab) {
            if (_stricmp(g_tabs[g_tab].name.c_str(), "Home") == 0 && !g_search[0]) draw_home_tab();
            if (_stricmp(g_tabs[g_tab].name.c_str(), "Favorites") == 0) draw_favorites_tab();
            for (auto& s : g_tabs[g_tab].sections) {
                bool any = false; for (auto& it : s.items) if (matches(it)) { any = true; break; }
                if (!any) continue;
                if (!s.name.empty()) section_header(s.name.c_str());
                for (auto& it : s.items) if (matches(it)) draw_item(it);
            }
        } else draw_bridge_tab();
        ImGui::Dummy(ImVec2(0, 8 * K));
        ImGui::PopStyleVar(2); ImGui::PopFont();
        ImGui::EndChild();
        ImGui::PopStyleVar(); ImGui::PopStyleColor(4); }

    // status display + version, both baked into the bottom band
    {   const bool live = game_base() != 0 && main_ticks() != 0;
        const ImVec4 ic = box("status_icon"), st = box("status_text"), vb = box("version_box");
        const ImVec2 c((ic.x + ic.z) * .5f, (ic.y + ic.w) * .5f); const float r = (ic.z - ic.x) * .45f;
        const ImU32 sc = live ? c_(col("status", kGreen)) : c_(0xF2B544);
        dl->AddCircle(c, r, sc, 32, std::max(1.5f, 3.f * K));
        if (live) { const ImVec2 pts[3] = { ImVec2(c.x - r * .5f, c.y + r * .02f), ImVec2(c.x - r * .12f, c.y + r * .42f), ImVec2(c.x + r * .55f, c.y - r * .38f) };
                    dl->AddPolyline(pts, 3, sc, 0, std::max(1.5f, 3.2f * K)); }
        else dl->AddLine(ImVec2(c.x, c.y - r * .45f), ImVec2(c.x, c.y + r * .45f), sc, std::max(1.5f, 3.f * K));
        text(dl, g_font_skin, 27.f * K, ImVec2(st.x, st.y), ImVec2(st.z, st.w), sc, live ? "TRAINER ACTIVE" : "WAITING FOR GAME", 0.f);
        text(dl, g_font_skin, 28.f * K, ImVec2(vb.x, vb.y), ImVec2(vb.z, vb.w), c_(col("version", kGoldL)), (std::string("v") + kVersion).c_str(), .5f); }

    ImGui::End();
    ImGui::PopFont();
    ui::theme_pop();
}

void panel_draw(bool* visible) {

    std::lock_guard<std::recursive_mutex> lk(g_mtx);
    // review 2026-11-15: on a game version the trainer does not support nothing is applied, so saved switches must not
    // show ON (shown OFF only - panel_state.json keeps them for a supported game)
    { static bool s_unsup_done = false;
      if (!s_unsup_done && game_base() && !sow_game_supported()) {
          s_unsup_done = true;
          for (auto& t : g_tabs) for (auto& s : t.sections) for (auto& it : s.items) { if (it.type == "toggle") it.on = false; it.lock = false; it.pending_reapply = false; }
      } }
    if (sk::on()) { panel_draw_skin(visible); return; }
    { const ImVec2 ds = ImGui::GetIO().DisplaySize;   // bigger: ~62% x 86% of the screen, never below the old size
      ImGui::SetNextWindowSize(ImVec2(std::max(S(1060), std::min(ds.x * 0.62f, S(1900))), std::max(S(760), ds.y * 0.86f)), ImGuiCond_Once); }
    ImGui::SetNextWindowPos(ImVec2(S(60), S(70)), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(S(460), S(360)), ImVec2(S(2600), S(1600)));
    ui::theme_push();
    ImGui::PushFont(g_font_body);
    if (!ImGui::Begin("##sow_trainer", visible, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse)) { ImGui::End(); ImGui::PopFont(); ui::theme_pop(); return; }

    // ---- header
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
    dl->AddRectFilledMultiColor(wp, ImVec2(wp.x + ws.x, wp.y + ws.y), ui::rgb(ui::kInk, .55f), ui::rgb(ui::kInk, .25f), ui::rgb(ui::kInk, .25f), ui::rgb(ui::kInk, .55f));
    dl->AddLine(ImVec2(wp.x, wp.y + 1), ImVec2(wp.x + ws.x, wp.y + 1), ui::rgb(ui::kGold, .8f), S(1.5f));
    ui::gold_frame(dl, ImVec2(wp.x + 1, wp.y + 1), ImVec2(wp.x + ws.x - 1, wp.y + ws.y - 1), 1.f, .5f);
    {   // two-tone title + status pills
        std::string t1 = g_title, t2; size_t sp = g_title.find(' '); if (sp != std::string::npos) { t1 = g_title.substr(0, sp); t2 = g_title.substr(sp + 1); }
        ui::brand(t1.c_str(), t2.c_str(), "SHADOW OF WAR  /  TRAINER");
        const std::string bid = game_build_id(); const std::string ver = bid.substr(0, bid.find('|'));
        char vb[64]; sprintf_s(vb, "Game %s", ver.c_str());
        ui::pill(game_base() ? "Game linked" : "Game not found", game_base() ? ui::kGreen : ui::kAmber); ImGui::SameLine(0, S(6));
        ui::pill("Engine linked", main_ticks() ? ui::kGreen : ui::kAmber); ImGui::SameLine(0, S(6));
        ui::pill(vb, ui::kCyan);
    }
    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - S(28));
    if (ImGui::Button("X", ImVec2(S(28), 0))) *visible = false;
    ImGui::Dummy(ImVec2(0, 2));

    // ---- body: tab rail + content
    float rail = S(176.f), footer = ImGui::GetFrameHeightWithSpacing() + 6;
    {   // controller tab switching
        const int total_ = (int)g_tabs.size() + 1;
        if (!ImGui::IsAnyItemActive()) {
            if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false)) g_tab = (g_tab + total_ - 1) % total_;
            if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false)) g_tab = (g_tab + 1) % total_;
        }
    }
    ImGui::BeginChild("##rail", ImVec2(rail, -footer), ImGuiChildFlags_Borders);
    int total = (int)g_tabs.size() + 1;
    for (int i = 0; i < total; ++i) {
        std::string name = i < (int)g_tabs.size() ? g_tabs[i].name : std::string(kSettingsTabName);
        bool sel = g_tab == i;
        if (i == (int)g_tabs.size()) { ImGui::Dummy(ImVec2(0, S(4))); ui::gold_rule(.3f); }
        ImGui::PushID(i);
        {   const ImVec2 p = ImGui::GetCursorScreenPos(); const float w = ImGui::GetContentRegionAvail().x, h = ImGui::GetFrameHeight() + S(8);
            if (ImGui::InvisibleButton("##nav", ImVec2(w, h))) g_tab = i;
            const bool hov = ImGui::IsItemHovered(); ImDrawList* nd = ImGui::GetWindowDrawList();
            if (sel) { nd->AddRectFilledMultiColor(p, ImVec2(p.x + w, p.y + h), ui::rgb(ui::kGold, .22f), ui::rgb(ui::kGold, .02f), ui::rgb(ui::kGold, .02f), ui::rgb(ui::kGold, .22f));
                       nd->AddRectFilled(p, ImVec2(p.x + S(3), p.y + h), ui::rgb(ui::kGold));
                       ui::gold_frame(nd, p, ImVec2(p.x + w - 1, p.y + h - 1), 1.f, .55f); }
            else if (hov) nd->AddRectFilled(p, ImVec2(p.x + w, p.y + h), ui::rgb(0xFFFFFF, .04f));
            std::string up = name; std::transform(up.begin(), up.end(), up.begin(), ::toupper);
            const ImVec2 ts = ImGui::CalcTextSize(up.c_str());
            nd->AddText(ImVec2(p.x + S(14), p.y + (h - ts.y) * .5f), sel ? ui::rgb(ui::kGoldHi) : hov ? ui::rgb(ui::kBone) : ui::rgb(ui::kDim), up.c_str()); }
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##content", ImVec2(0, -footer), ImGuiChildFlags_Borders);
    if (g_tab < (int)g_tabs.size()) {
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##search", "Search...", g_search, sizeof g_search);
        ui::page_title(g_tabs[g_tab].name.c_str());
        if (_stricmp(g_tabs[g_tab].name.c_str(), "Home") == 0 && !g_search[0]) draw_home_tab();
        if (_stricmp(g_tabs[g_tab].name.c_str(), "Favorites") == 0) draw_favorites_tab();
        for (auto& s : g_tabs[g_tab].sections) {
            bool any = false; for (auto& it : s.items) if (matches(it)) { any = true; break; }
            if (!any) continue;
            if (!s.name.empty()) section_header(s.name.c_str());
            for (auto& it : s.items) if (matches(it)) draw_item(it);
        }

    } else draw_bridge_tab();
    ImGui::EndChild();

    // ---- footer
    auto dot = [](bool good) {
        ImVec2 p = ImGui::GetCursorScreenPos(); float h = ImGui::GetFrameHeight();
        ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + 5, p.y + h * 0.5f), 4.f, good ? IM_COL32(0x5f, 0xd3, 0x8c, 255) : IM_COL32(0xf2, 0xb5, 0x44, 255));
        ImGui::Dummy(ImVec2(12, h)); ImGui::SameLine();
    };
    ImGui::AlignTextToFramePadding();
    dot(game_base() != 0); ImGui::TextDisabled("GAME"); ImGui::SameLine(0, 18);
    dot(main_ticks() != 0); ImGui::TextDisabled("ENGINE");
    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - S(120)); ImGui::TextDisabled("INSERT  hide");
    ImGui::End();
    ImGui::PopFont();
    ui::theme_pop();
}

static Item* find_item(const std::string& id) {
    for (auto& t : g_tabs) for (auto& s : t.sections) for (auto& it : s.items) if (it.id == id) return &it;
    return nullptr;
}

// ------------------------------------------------------------------ panel state helpers
json panel_show(bool visible, const std::string& tab) {
    std::lock_guard<std::recursive_mutex> lk(g_mtx);
    if (!tab.empty()) {
        int total = (int)g_tabs.size() + 1, found = -1;
        for (int i = 0; i < (int)g_tabs.size(); ++i) if (_stricmp(g_tabs[i].name.c_str(), tab.c_str()) == 0) found = i;
        if (_stricmp(tab.c_str(), kSettingsTabName) == 0) found = total - 1;
        if (found < 0) return fail("NO_TAB", "No tab '" + tab + "'");
        g_tab = found;
    }
    overlay_set_visible(visible);
    return ok({ {"visible", visible}, {"tab", g_tab} });
}

json panel_state() {
    std::lock_guard<std::recursive_mutex> lk(g_mtx);
    json arr = json::array();
    for (auto& t : g_tabs) for (auto& s : t.sections) for (auto& it : s.items) {
        json r = { {"id", it.id}, {"tab", t.name}, {"section", s.name}, {"label", it.label}, {"type", it.type} };
        if (it.type == "toggle") r["on"] = it.on;
        if (it.type == "int" || it.type == "float") {
            double live; std::string e;
            if (it.action.value("kind", "") == "mem" && mem_read_value(it.action, live, e)) r["live"] = live;
            r["value"] = it.val; r["lock"] = it.lock;
        }
        if (!it.last_err.empty()) r["error"] = it.last_err;
        arr.push_back(r);
    }
    return ok({ {"title", g_title}, {"items", arr}, {"visible", overlay_visible()} });
}

json panel_set(const std::string& id, const json& value) {
    std::lock_guard<std::recursive_mutex> lk(g_mtx);
    for (auto& t : g_tabs) for (auto& s : t.sections) for (auto& it : s.items) {
        if (it.id != id) continue;
        if (it.type == "toggle") { it.on = value.is_boolean() ? value.get<bool>() : !it.on; apply(it, it.on ? "on" : "off"); }
        else if (it.type == "int" || it.type == "float") {
            if (value.is_object()) { if (value.contains("lock")) it.lock = value["lock"].get<bool>(); if (value.contains("value")) { it.val = value["value"].get<double>(); apply(it, "value"); } }
            else { it.val = value.get<double>(); apply(it, "value"); }
        }
        else if (it.type == "button") apply(it, "press");
        else if (it.type == "select" && value.is_string()) { set_text(it.text, value.get<std::string>()); }
        else if (it.type == "input" && value.is_string()) {           // fill the box AND press GO, like the in-game button
            set_text(it.text, value.get<std::string>());
            json a2 = it.action; std::string cmd = it.action.value("cmd", std::string("{text}")); size_t pp = cmd.find("{text}");
            if (pp != std::string::npos) cmd.replace(pp, 6, it.text);
            a2["cmd"] = cmd; std::string err; run_action(it, a2, "press", err); it.last_err = err;
        }
        save_state();
        if (!it.last_err.empty()) return fail("ACTION_FAILED", it.last_err);
        return ok({ {"id", id} }, "applied");
    }
    return fail("NO_ITEM", "No panel item with id '" + id + "' (PANEL.STATE lists ids).");
}

} // namespace wb
