// Shadow of War Trainer - core: config, log, memory tools, address expressions, main-thread runner, guarded calls, DLL entry.
#define _CRT_RAND_S
#include "common.h"
#include <tlhelp32.h>
#include <psapi.h>
#include <cstdarg>
#include <deque>
#include <fstream>
#include <algorithm>
#include <thread>
#include <share.h>
#include "MinHook.h"
#pragma comment(lib, "psapi.lib")

namespace wb {

Config g_cfg;
static HMODULE g_self = nullptr;
static HANDLE g_active_marker = nullptr;

// ------------------------------------------------------------------ util
std::string hexstr(uint64_t v) { char b[32]; sprintf_s(b, "0x%llX", (unsigned long long)v); return b; }

std::string to_hex(const uint8_t* p, size_t n) {
    std::string s; char b[4];
    for (size_t i = 0; i < n; ++i) { sprintf_s(b, "%02X", p[i]); if (i) s += ' '; s += b; }
    return s;
}

bool parse_pattern(const std::string& s, std::vector<int>& pat) {
    size_t i = 0;
    while (i < s.size()) {
        if (isspace((unsigned char)s[i])) { ++i; continue; }
        if (s[i] == '?' || s[i] == '*') { pat.push_back(-1); while (i < s.size() && (s[i] == '?' || s[i] == '*')) ++i; continue; }
        if (i + 1 >= s.size() || !isxdigit((unsigned char)s[i]) || !isxdigit((unsigned char)s[i + 1])) return false;
        pat.push_back((int)strtoul(s.substr(i, 2).c_str(), nullptr, 16)); i += 2;
    }
    return !pat.empty();
}

HMODULE self_module() { return g_self; }

// ------------------------------------------------------------------ log
static std::mutex g_log_mtx;
static std::deque<std::string> g_log_ring;
static FILE* g_log_file = nullptr;
static char g_log_path[MAX_PATH] = {};          // trainer.log, for the end-of-session marker written from DllMain
static thread_local bool t_in_log = false;      // a fault while this thread writes the log must not log again (deadlock)

void log(const char* fmt, ...) {
    if (t_in_log) return;
    char msg[2048];
    va_list ap; va_start(ap, fmt); vsnprintf(msg, sizeof msg, fmt, ap); va_end(ap);
    SYSTEMTIME t; GetLocalTime(&t);
    char line[2200];
    sprintf_s(line, "[%02d:%02d:%02d.%03d] %s", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, msg);
    t_in_log = true;
    struct Reset { ~Reset() { t_in_log = false; } } reset;
    std::lock_guard<std::mutex> lk(g_log_mtx);
    g_log_ring.push_back(line);
    while (g_log_ring.size() > 800) g_log_ring.pop_front();
    if (g_log_file) { fprintf(g_log_file, "%s\n", line); fflush(g_log_file); }
}

std::vector<std::string> log_tail(size_t n) {
    std::lock_guard<std::mutex> lk(g_log_mtx);
    size_t start = g_log_ring.size() > n ? g_log_ring.size() - n : 0;
    return std::vector<std::string>(g_log_ring.begin() + start, g_log_ring.end());
}

// ------------------------------------------------------------------ config
void load_config() {
    std::ifstream f(g_cfg.root + "\\config.json");
    if (!f) { save_config(); return; }
    try {
        json j = json::parse(f);
        g_cfg.toggle_key = j.value("toggle_key", g_cfg.toggle_key);
        g_cfg.lock_input = j.value("lock_input", g_cfg.lock_input);
    } catch (std::exception& e) { log("config.json is not valid JSON (%s) - using defaults.", e.what()); }
}

void save_config() {
    // a recreated config keeps the trainer key + favorites list
    json j = { {"favorites", json::array()}, {"toggle_key", g_cfg.toggle_key} };
    std::ofstream(g_cfg.root + "\\config.json") << j.dump(2);
}

// ------------------------------------------------------------------ modules / memory
static std::mutex g_mod_mtx;
static std::vector<ModuleInfo> g_mods;

std::vector<ModuleInfo> list_modules(bool refresh) {
    std::lock_guard<std::mutex> lk(g_mod_mtx);
    if (refresh || g_mods.empty()) {
        g_mods.clear();
        HMODULE mods[2048]; DWORD needed = 0;
        if (EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &needed)) {
            for (DWORD i = 0; i < needed / sizeof(HMODULE) && i < 2048; ++i) {
                MODULEINFO mi{}; char name[MAX_PATH]{};
                GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof mi);
                GetModuleBaseNameA(GetCurrentProcess(), mods[i], name, MAX_PATH);
                g_mods.push_back({ name, (uintptr_t)mi.lpBaseOfDll, (size_t)mi.SizeOfImage });
            }
        }
    }
    return g_mods;
}

std::string module_name_of(uintptr_t a) {
    for (auto& m : list_modules()) if (a >= m.base && a < m.base + m.size) return m.name;
    return "";
}

uintptr_t module_base(const std::string& name, size_t* size) {
    for (int pass = 0; pass < 2; ++pass)
        for (auto& m : list_modules(pass == 1))
            if (_stricmp(m.name.c_str(), name.c_str()) == 0) { if (size) *size = m.size; return m.base; }
    return 0;
}

uintptr_t game_base(size_t* size) {
    static uintptr_t b = 0; static size_t s = 0;
    if (!b) b = module_base(kGameExe, &s);
    if (size) *size = s;
    return b;
}

// ------------------------------------------------------------------ module snapshot (crash recorder + mod list)
// The crash recorder runs inside an exception handler, where asking Windows which DLL owns an address can deadlock
// (loader lock). So a background thread keeps a snapshot of every loaded DLL in two buffers and flips between them;
// the handler only reads the active one. It also logs which non-Windows DLLs are loaded (other mods, overlays): that
// is the first thing to know when a crash report comes in.
namespace {
struct ModSnap { char name[64]; uintptr_t base; size_t size; bool system; };
constexpr int kSnapMax = 768;
ModSnap g_snap[2][kSnapMax];
int g_snap_n[2] = { 0, 0 };
std::atomic<int> g_snap_cur{ -1 };
std::mutex g_snap_mtx;
std::vector<std::string> g_seen_mods;            // module paths already written to the log
bool starts_with_i(const std::string& s, const std::string& p) { return s.size() >= p.size() && _strnicmp(s.c_str(), p.c_str(), p.size()) == 0; }
}

void modsnap_refresh() {
    std::lock_guard<std::mutex> lk(g_snap_mtx);
    char win[MAX_PATH] = {}; GetWindowsDirectoryA(win, MAX_PATH);
    char exe[MAX_PATH] = {}; GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string game_dir = exe; game_dir = game_dir.substr(0, game_dir.find_last_of("\\/"));     // ...\ShadowOfWar\x64
    std::string game_root = game_dir.substr(0, game_dir.find_last_of("\\/"));                  // ...\ShadowOfWar
    const int dst = g_snap_cur.load() == 0 ? 1 : 0;
    int n = 0;
    std::vector<std::string> fresh;
    HMODULE mods[1024]; DWORD needed = 0;
    if (EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &needed)) {
        for (DWORD i = 0; i < needed / sizeof(HMODULE) && i < 1024 && n < kSnapMax; ++i) {
            MODULEINFO mi{}; char path[MAX_PATH] = {};
            if (!GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof mi)) continue;
            GetModuleFileNameExA(GetCurrentProcess(), mods[i], path, MAX_PATH);
            std::string p = path, base = p.substr(p.find_last_of("\\/") + 1);
            ModSnap& m = g_snap[dst][n++];
            strncpy_s(m.name, base.c_str(), _TRUNCATE);
            m.base = (uintptr_t)mi.lpBaseOfDll; m.size = mi.SizeOfImage;
            m.system = starts_with_i(p, win);
            if (m.system || mods[i] == GetModuleHandleA(nullptr) || mods[i] == g_self) continue;
            if (std::find(g_seen_mods.begin(), g_seen_mods.end(), p) != g_seen_mods.end()) continue;
            g_seen_mods.push_back(p);
            // never write a full path from outside the game folder: logs get shared, paths can hold the user name
            std::string shown = starts_with_i(p, game_root + "\\") ? p.substr(game_root.size() + 1) : base + " (outside the game folder)";
            WIN32_FILE_ATTRIBUTE_DATA fa{}; char sz[32] = "";
            if (GetFileAttributesExA(path, GetFileExInfoStandard, &fa)) sprintf_s(sz, " %lu KB", (unsigned long)((fa.nFileSizeLow + 1023) / 1024));
            fresh.push_back(shown + sz);
        }
    }
    g_snap_n[dst] = n;
    g_snap_cur = dst;
    if (fresh.empty()) return;
    static bool first = true;
    if (first) { log("[mods] other DLLs in the game (mods, loaders, overlays): %zu", fresh.size()); for (auto& f : fresh) log("[mods]   %s", f.c_str()); }
    else for (auto& f : fresh) log("[mods] DLL loaded later: %s", f.c_str());
    first = false;
}

// lock-free lookup for the crash recorder; false = not inside any known DLL
bool modsnap_find(uintptr_t a, char* out, size_t n, bool* system) {
    const int cur = g_snap_cur.load();
    if (cur < 0) return false;
    for (int i = 0; i < g_snap_n[cur]; ++i) {
        const ModSnap& m = g_snap[cur][i];
        if (a >= m.base && a < m.base + m.size) {
            sprintf_s(out, n, "%s+0x%llX", m.name, (unsigned long long)(a - m.base));
            if (system) *system = m.system;
            return true;
        }
    }
    return false;
}

bool safe_read(uintptr_t addr, void* out, size_t n) {
    if (addr < 0x10000 || addr > 0x7FFFFFFFFFFFull) return false;
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), (LPCVOID)addr, out, n, &got) && got == n;
}

bool safe_write(uintptr_t addr, const void* in, size_t n) {
    if (addr < 0x10000) return false;
    DWORD old = 0;
    if (!VirtualProtect((LPVOID)addr, n, PAGE_EXECUTE_READWRITE, &old)) return false;
    SIZE_T put = 0;
    BOOL okw = WriteProcessMemory(GetCurrentProcess(), (LPVOID)addr, in, n, &put);
    DWORD tmp; VirtualProtect((LPVOID)addr, n, old, &tmp);
    FlushInstructionCache(GetCurrentProcess(), (LPCVOID)addr, n);
    return okw && put == n;
}

std::vector<Region> readable_regions(uintptr_t base, size_t size) {
    std::vector<Region> out;
    uintptr_t a = base, end = base + size;
    while (a < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery((LPCVOID)a, &mbi, sizeof mbi)) break;
        uintptr_t rs = (uintptr_t)mbi.BaseAddress, re = std::min<uintptr_t>(rs + mbi.RegionSize, end);
        DWORD p = mbi.Protect & 0xFF;
        bool readable = mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_GUARD) && p != PAGE_NOACCESS;
        bool exec = p == PAGE_EXECUTE_READ || p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY || p == PAGE_EXECUTE;
        if (readable) out.push_back({ std::max(rs, a), (size_t)(re - std::max(rs, a)), exec });
        a = re;
    }
    return out;
}

std::vector<Region> heap_regions() {
    std::vector<Region> out;
    SYSTEM_INFO si; GetSystemInfo(&si);
    uintptr_t addr = (uintptr_t)si.lpMinimumApplicationAddress, top = (uintptr_t)si.lpMaximumApplicationAddress;
    while (addr < top) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery((LPCVOID)addr, &mbi, sizeof mbi)) break;
        uintptr_t rs = (uintptr_t)mbi.BaseAddress; size_t rsz = mbi.RegionSize;
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && (mbi.Protect & 0xFF) == PAGE_READWRITE && !(mbi.Protect & PAGE_GUARD))
            out.push_back({ rs, rsz, false });
        addr = rs + rsz;
    }
    return out;
}

// ------------------------------------------------------------------ address expressions
namespace {
struct Parser {
    const std::string& s; size_t i = 0; std::string err;
    explicit Parser(const std::string& src) : s(src) {}
    void ws() { while (i < s.size() && isspace((unsigned char)s[i])) ++i; }
    bool term(uintptr_t& v) {
        ws();
        if (i >= s.size()) { err = "unexpected end of expression"; return false; }
        if (s[i] == '[') {
            ++i; uintptr_t inner;
            if (!expr(inner)) return false;
            ws(); if (i >= s.size() || s[i] != ']') { err = "missing ']'"; return false; }
            ++i;
            if (!read_as(inner, v)) { err = "cannot read pointer at " + hexstr(inner); return false; }
            return true;
        }
        if (isdigit((unsigned char)s[i])) {
            size_t st = i; int base = 10;
            if (s.compare(i, 2, "0x") == 0 || s.compare(i, 2, "0X") == 0) { base = 16; i += 2; st = i; }
            while (i < s.size() && isxdigit((unsigned char)s[i])) ++i;
            v = (uintptr_t)strtoull(s.substr(st, i - st).c_str(), nullptr, base);
            return true;
        }
        size_t st = i;
        while (i < s.size() && (isalnum((unsigned char)s[i]) || s[i] == '_' || s[i] == '.' || s[i] == ':')) ++i;
        std::string sym = s.substr(st, i - st);
        if (sym.empty()) { err = std::string("unexpected character '") + s[i] + "'"; return false; }
        if (_stricmp(sym.c_str(), "game") == 0) { v = game_base(); return v != 0; }
        if (_strnicmp(sym.c_str(), "sig:", 4) == 0) {
            v = sig_addr(sym.substr(4));
            if (!v) { err = "signature '" + sym.substr(4) + "' is not resolved (SIG.LIST)"; return false; }
            return true;
        }
        v = module_base(sym);
        if (!v) { err = "unknown symbol or module '" + sym + "'"; return false; }
        return true;
    }
    bool expr(uintptr_t& v) {
        if (!term(v)) return false;
        for (;;) {
            ws();
            if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
                char op = s[i++]; uintptr_t r;
                if (!term(r)) return false;
                v = op == '+' ? v + r : v - r;
            } else return true;
        }
    }
};
}

bool eval_addr(const std::string& e, uintptr_t& out, std::string* err) {
    Parser p(e);
    bool okp = p.expr(out);
    if (okp) { p.ws(); if (p.i != e.size()) { p.err = "unexpected text at position " + std::to_string(p.i); okp = false; } }
    if (!okp && err) *err = "address '" + e + "': " + p.err;
    return okp;
}

// ------------------------------------------------------------------ main thread runner
static DWORD g_main_tid = 0;
static std::atomic<uint64_t> g_ticks{0};
static std::mutex g_q_mtx;
static std::vector<std::function<void()>> g_queue;
static void run_tick_fns();
static thread_local bool t_in_drain = false;

DWORD main_thread_id() { return g_main_tid; }
uint64_t main_ticks() { return g_ticks.load(); }

static void drain_main() {
    if (t_in_drain || GetCurrentThreadId() != g_main_tid) return;
    t_in_drain = true;
    ++g_ticks;
    std::vector<std::function<void()>> work;
    { std::lock_guard<std::mutex> lk(g_q_mtx); work.swap(g_queue); }
    for (auto& w : work) { try { w(); } catch (std::exception& e) { log("main-thread task threw: %s", e.what()); } catch (...) { log("main-thread task threw"); } }
    t_in_drain = false;
}

// Engine tick: the engine runs named tasks every frame (sig:engineTask, rcx = Task, name char* at +0x1A8).
// The "BeginFrame" task is our engine-thread tick: queued commands (FN.CALL, live object lookups) run there.
// (Shadow of War's window thread never pumps the game loop through PeekMessage - 0 ticks on 2026-11-06.)
using Task_t = uintptr_t(__fastcall*)(uintptr_t);
static Task_t o_task = nullptr;
static std::atomic<uintptr_t> g_begin_task{0};
static std::atomic<bool> g_stopping{false};
static uintptr_t __fastcall hk_task(uintptr_t t) {
    uintptr_t bt = g_begin_task.load();
    if (!bt && !g_stopping) {
        uintptr_t name = 0; char nm[12] = {};
        if (read_as(t + 0x1A8, name) && name && safe_read(name, nm, 11) && strcmp(nm, "BeginFrame") == 0) { g_begin_task = t; bt = t; log("[tick] BeginFrame task found (%s)", hexstr(t).c_str()); }
    }
    if (t == bt && !g_stopping) { g_main_tid = GetCurrentThreadId(); drain_main(); run_tick_fns(); }
    return o_task(t);
}

struct FindWnd { DWORD pid; HWND best; LONG area; };
static BOOL CALLBACK enum_cb(HWND h, LPARAM lp) {
    auto* f = (FindWnd*)lp; DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != f->pid || !IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
    RECT rc; GetWindowRect(h, &rc);
    LONG area = (rc.right - rc.left) * (rc.bottom - rc.top);
    if (area > f->area) { f->area = area; f->best = h; }
    return TRUE;
}
HWND find_game_window() { FindWnd f{ GetCurrentProcessId(), nullptr, 0 }; EnumWindows(enum_cb, (LPARAM)&f); return f.best; }

bool wait_game_window() {
    for (int i = 0; i < 1800; ++i) { if (find_game_window()) return true; Sleep(100); }
    log("Game window not found after 180 s.");
    return false;
}

// Shadow of War: the tick comes from the Havok Script lua_pcall hook (sow.cpp), on the game's script thread
bool sow_install_tick();
void engine_tick_here() { if (g_stopping) return; g_main_tid = GetCurrentThreadId(); drain_main(); run_tick_fns(); }
bool core_stopping() { return g_stopping.load(); }

bool install_main_thread_hook() {
    uintptr_t at = sig_addr("engineTask");
    if (!at) { log("[tick] no engineTask signature - using the Havok Script lua_pcall tick"); return sow_install_tick(); }
    bool okh = MH_CreateHook((void*)at, &hk_task, (void**)&o_task) == MH_OK && MH_EnableHook((void*)at) == MH_OK;
    log("[tick] engine task hook %s at %s", okh ? "installed" : "FAILED", hexstr(at).c_str());
    return okh;
}
// queue work for the engine thread without waiting (panel actions, UI)
void post_main(std::function<void()> fn) { std::lock_guard<std::mutex> lk(g_q_mtx); g_queue.push_back(std::move(fn)); }
// functions run every engine frame after the queue (panel freezes, live values)
static std::mutex g_tk_mtx; static std::vector<void (*)()> g_tick_fns;
void add_main_tick(void (*fn)()) { std::lock_guard<std::mutex> lk(g_tk_mtx); for (auto f : g_tick_fns) if (f == fn) return; g_tick_fns.push_back(fn); }
static void run_tick_fns() {
    if (!sow_game_supported()) return;   // fixed offsets of another exe build: no trainer ticks at all
    std::vector<void (*)()> fns; { std::lock_guard<std::mutex> lk(g_tk_mtx); fns = g_tick_fns; }
    for (auto f : fns) { try { f(); } catch (std::exception& e) { log("[tick] threw: %s", e.what()); } catch (...) { log("[tick] threw"); } }
}

json run_on_main(std::function<json()> fn, DWORD timeout_ms) {
    if (GetCurrentThreadId() == g_main_tid) return fn();
    // 2026-11-18: the game script tick only starts once a save is loading/loaded, so a click in the main menu used to
    // say "Main-thread hook is not installed" (user report). Nothing is wrong then: the game is just not in a world yet.
    if (!g_main_tid) return fail("NOT_IN_WORLD", "wait until you are in the game world (the game is still starting up)");
    // review 2026-11-14: a job that timed out (loading screen) used to run later anyway - "failed" Give clicked twice
    // then gave two items - and its event handle leaked. Now a timed-out job is dropped, the handle closes with the state.
    struct State { HANDLE ev = nullptr; json result; std::atomic<int> phase{ 0 };   /* 0 queued, 1 running, 2 cancelled */ ~State() { if (ev) CloseHandle(ev); } };
    auto st = std::make_shared<State>();
    st->ev = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    if (!st->ev) return fail("NO_EVENT", "could not create a wait event - nothing was done");   // review 2026-11-15
    { std::lock_guard<std::mutex> lk(g_q_mtx);
      g_queue.push_back([st, fn]() {
          int q = 0; if (!st->phase.compare_exchange_strong(q, 1)) return;   // the caller gave up: do not run it late
          // review 2026-11-15: any throw must still signal, or a caller already past its timeout waits forever
          try { st->result = fn(); } catch (std::exception& e) { st->result = fail("EXCEPTION", e.what()); }
          catch (...) { st->result = fail("EXCEPTION", "unknown exception"); }
          if (st->ev) SetEvent(st->ev); }); }
    if (WaitForSingleObject(st->ev, timeout_ms) != WAIT_OBJECT_0) {
        int q = 0;
        if (st->phase.compare_exchange_strong(q, 2)) return fail("TIMEOUT", "Game main thread did not run the command within " + std::to_string(timeout_ms) + " ms (loading or minimized?) - nothing was done.");
        WaitForSingleObject(st->ev, INFINITE);                  // already running: wait for its real result
    }
    return st->result;
}

// ------------------------------------------------------------------ guarded calls
using Fn6 = uint64_t(__fastcall*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
using Fn6f = float(__fastcall*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
static int seh_call(uintptr_t fn, const uint64_t* a, bool want_float, uint64_t* out_u, float* out_f, FaultInfo* fi) {
    __try {
        if (want_float) *out_f = ((Fn6f)fn)(a[0], a[1], a[2], a[3], a[4], a[5]);
        else *out_u = ((Fn6)fn)(a[0], a[1], a[2], a[3], a[4], a[5]);
        return 1;
    } __except (fault_filter(GetExceptionInformation(), fi)) { return 0; }
}

json guarded_call(uintptr_t fn, const std::vector<uint64_t>& args, const std::string& ret, bool quiet) {
    if (args.size() > 6) return fail("BAD_ARG", "at most 6 arguments");
    uint64_t a[6] = {}; for (size_t i = 0; i < args.size(); ++i) a[i] = args[i];
    uint64_t u = 0; float f = 0; FaultInfo fi;
    bool fl = ret == "f32";
    if (!seh_call(fn, a, fl, &u, &f, &fi)) { std::string m = fault_text(fi); log("[call] FAULT calling %s: %s", hexstr(fn).c_str(), m.c_str()); return fail("CALL_FAULT", m); }
    if (!quiet) log("[call] %s(%zu args) -> %s", hexstr(fn).c_str(), args.size(), fl ? std::to_string(f).c_str() : hexstr(u).c_str());
    return fl ? ok({ {"ret", f} }) : ok({ {"ret", hexstr(u)}, {"ret_i", (int64_t)u} });
}

// ------------------------------------------------------------------ log rotation
// Keeps the previous session's log instead of overwriting it: trainer.log -> logs\trainer_<last write time>.log
// (every start), and only the newest kLogKeep saved logs are kept - older ones are deleted.
static constexpr size_t kLogKeep = 20;
static void rotate_logs(const std::string& dir) {
    std::string cur = dir + "\\trainer.log", ld = dir + "\\logs";
    CreateDirectoryA(ld.c_str(), nullptr);
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (GetFileAttributesExA(cur.c_str(), GetFileExInfoStandard, &fa) && (fa.nFileSizeLow || fa.nFileSizeHigh)) {
        FILETIME lt; SYSTEMTIME st; FileTimeToLocalFileTime(&fa.ftLastWriteTime, &lt); FileTimeToSystemTime(&lt, &st);
        char base[64]; sprintf_s(base, "\\trainer_%04d%02d%02d_%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        for (int n = 1; n < 100; ++n) {                      // same second twice (fast reloads): _2, _3 ...
            std::string to = ld + base + (n == 1 ? std::string() : "_" + std::to_string(n)) + ".log";
            if (MoveFileA(cur.c_str(), to.c_str())) break;
            if (GetLastError() != ERROR_ALREADY_EXISTS && GetLastError() != ERROR_FILE_EXISTS) break;
        }
    }
    std::vector<std::string> files; WIN32_FIND_DATAA fd{};
    HANDLE h = FindFirstFileA((ld + "\\trainer_*.log").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) { do files.push_back(fd.cFileName); while (FindNextFileA(h, &fd)); FindClose(h); }
    std::sort(files.begin(), files.end());                   // names sort by time
    for (size_t i = 0; i + kLogKeep < files.size(); ++i) DeleteFileA((ld + "\\" + files[i]).c_str());
}

// ------------------------------------------------------------------ crash logs
// Every session starts its log with kSessionStart. When the game exits normally, DllMain writes kSessionEnd. A log
// that has the start line but no end line belonged to a game that crashed (or was force-closed): before it is rotated
// away it is copied into logs\crashes, where the normal 20-log rotation never deletes it. To keep that folder small,
// one file collects up to kCrashPerFile crashes, then a new file starts. Old crash files are never deleted: the whole
// crash history stays available for tracing a problem (20 crashes per file keeps the folder small).
static const char kSessionStart[] = "[session] started - crash logs on";
static const char kSessionEnd[] = "[session] game closed normally";
static constexpr int kCrashPerFile = 20;
static const char kCrashHeader[] = "==================== CRASH ";
static std::atomic<int> g_crash_total{ 0 };
static std::atomic<bool> g_prev_crashed{ false };
static std::string g_crash_dir;

bool crash_last_session() { return g_prev_crashed.load(); }
int crash_log_count() { return g_crash_total.load(); }
std::string crash_log_dir() { return g_crash_dir; }

static std::vector<std::string> crash_files() {
    std::vector<std::string> v; WIN32_FIND_DATAA fd{};
    HANDLE h = FindFirstFileA((g_crash_dir + "\\crashes_*.log").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) { do v.push_back(fd.cFileName); while (FindNextFileA(h, &fd)); FindClose(h); }
    std::sort(v.begin(), v.end());                                              // crashes_001, crashes_002 ...
    return v;
}
static int count_headers(const std::string& file) {
    std::ifstream f(file); std::string ln; int n = 0;
    while (std::getline(f, ln)) if (ln.rfind(kCrashHeader, 0) == 0) ++n;
    return n;
}
static std::string last_line_with(const std::vector<std::string>& lines, const char* tag) {
    for (auto it = lines.rbegin(); it != lines.rend(); ++it) if (it->find(tag) != std::string::npos) return *it;
    return "(none)";
}

static void archive_crash(const std::string& dir) {
    g_crash_dir = dir + "\\logs\\crashes";
    CreateDirectoryA((dir + "\\logs").c_str(), nullptr);
    CreateDirectoryA(g_crash_dir.c_str(), nullptr);
    const std::string cur = dir + "\\trainer.log";
    std::vector<std::string> lines;
    { std::ifstream f(cur); std::string ln; while (std::getline(f, ln)) lines.push_back(ln); }
    // "Ready." = the trainer got past the game window: the exe's first stub process (it restarts itself while it
    // unpacks) can be killed before that, which is not a crash
    bool started = false, ended = false, ready = false;
    for (auto& l : lines) {
        if (l.find(kSessionStart) != std::string::npos) started = true;
        if (l.find(kSessionEnd) != std::string::npos || l.find("[session] trainer unloaded") != std::string::npos) ended = true;
        if (l.find("] Ready.") != std::string::npos) ready = true;
    }
    if (started && ready && !ended) {
        auto files = crash_files();
        int num = 1, in_file = 0;
        if (!files.empty()) {
            num = atoi(files.back().c_str() + 8);                               // "crashes_" + number
            in_file = count_headers(g_crash_dir + "\\" + files.back());
            WIN32_FILE_ATTRIBUTE_DATA fa{};
            const bool big = GetFileAttributesExA((g_crash_dir + "\\" + files.back()).c_str(), GetFileExInfoStandard, &fa) && fa.nFileSizeLow > 8u * 1024 * 1024;
            if (in_file >= kCrashPerFile || big) { ++num; in_file = 0; }
        }
        char name[48]; sprintf_s(name, "\\crashes_%03d.log", num);
        WIN32_FILE_ATTRIBUTE_DATA fa{}; SYSTEMTIME st{};
        if (GetFileAttributesExA(cur.c_str(), GetFileExInfoStandard, &fa)) { FILETIME lt; FileTimeToLocalFileTime(&fa.ftLastWriteTime, &lt); FileTimeToSystemTime(&lt, &st); }
        std::ofstream o(g_crash_dir + name, std::ios::app);
        char when[64]; sprintf_s(when, "%04d-%02d-%02d %02d:%02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        o << kCrashHeader << (in_file + 1) << " of this file ====================\n"
          << "The game closed unexpectedly: it crashed, or it was force-closed (Task Manager).\n"
          << "Last log line written at: " << when << "\n"
          << "Last error recorded:      " << last_line_with(lines, "[crash-rec]") << "\n"
          << "Last trainer option used: " << last_line_with(lines, "[option]") << "\n"
          << "Last world change:        " << last_line_with(lines, "[world]") << "\n"
          << "---------- full trainer log of that game session ----------\n";
        for (auto& l : lines) o << l << "\n";
        o << "\n";
        o.close();
        g_prev_crashed = true;
    }
    int total = 0;
    for (auto& f : crash_files()) total += count_headers(g_crash_dir + "\\" + f);
    g_crash_total = total;
}

// ------------------------------------------------------------------ entry
static DWORD WINAPI init_thread(LPVOID) {
    char path[MAX_PATH]; GetModuleFileNameA(g_self, path, MAX_PATH);
    std::string dir = path; dir = dir.substr(0, dir.find_last_of("\\/"));
    if (dir.size() > 7 && _stricmp(dir.substr(dir.size() - 7).c_str(), "\\loaded") == 0) dir = dir.substr(0, dir.size() - 7);
    g_cfg.root = dir;
    archive_crash(dir);                                                         // last session crashed? keep its log
    rotate_logs(dir);                                                           // keep the last 20 sessions' logs
    strcpy_s(g_log_path, (dir + "\\trainer.log").c_str());
    g_log_file = _fsopen(g_log_path, "w", _SH_DENYNO);                         // readable while the game runs
    { SYSTEMTIME st; GetLocalTime(&st);
      log(WB_PRODUCT " %s attached to pid %lu, game base %s, %04d-%02d-%02d", kVersion, GetCurrentProcessId(), hexstr(game_base()).c_str(), st.wYear, st.wMonth, st.wDay); }
    log("%s (saved crash logs: %d, in logs\\crashes)", kSessionStart, g_crash_total.load());
    if (g_prev_crashed) log("[session] the previous game session ended unexpectedly - its log was saved in logs\\crashes");
    load_config();
    MH_Initialize();
    log("Waiting for the game window (the exe unpacks itself first)...");
    if (!wait_game_window()) return 0;   // launcher stub: it relaunches the real game and exits
    Sleep(3000);
    sigs_init();
    install_main_thread_hook();     // engine BeginFrame tick
    install_overlay();
    log("Ready.");
    if (g_prev_crashed) toast("The game closed unexpectedly last time. The trainer saved a crash log in its logs\\crashes folder (Settings > Crash logs).");
    // DLL list for the log now, then every 5 s (DLLs loaded later, and fresh addresses for the crash recorder)
    std::thread([]() { while (!g_stopping) { modsnap_refresh(); Sleep(5000); } }).detach();
    if (sow_game_supported()) sow_world_ready();   // starts the player watcher now: "[world]" lines from the first load on
    return 0;
}

// End-of-session marker, written from DllMain. Other threads are gone or frozen then and may hold the log mutex or
// the CRT file lock, so this uses a fresh handle and WriteFile only (no locks).
static void write_session_end(const char* what) {
    if (!g_log_path[0]) return;
    HANDLE h = CreateFileA(g_log_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME t; GetLocalTime(&t);
    char line[160]; const int n = sprintf_s(line, "[%02d:%02d:%02d.%03d] %s\r\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, what);
    DWORD w = 0; if (n > 0) WriteFile(h, line, (DWORD)n, &w, nullptr);
    CloseHandle(h);
}
void session_end_marker(bool process_exit) { write_session_end(process_exit ? kSessionEnd : "[session] trainer unloaded"); }

} // namespace wb

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID reserved) {
    // process exit (reserved != null): a normal quit runs this, a crash does not - that is how the next start knows
    if (reason == DLL_PROCESS_DETACH && wb::g_self) wb::session_end_marker(reserved != nullptr);
    if (reason == DLL_PROCESS_ATTACH) {
        char nm[64]; sprintf_s(nm, "Local\\ShadowOfWarTrainerActive_%lu", GetCurrentProcessId());
        HANDLE m = CreateMutexA(nullptr, TRUE, nm);
        if (GetLastError() == ERROR_ALREADY_EXISTS) { if (m) CloseHandle(m); return FALSE; }   // one copy per game session
        wb::g_active_marker = m;
        wb::g_self = h;
        DisableThreadLibraryCalls(h);
        CloseHandle(CreateThread(nullptr, 0, wb::init_thread, nullptr, 0, nullptr));
    }
    return TRUE;
}
