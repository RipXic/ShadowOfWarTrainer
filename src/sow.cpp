// sow.cpp - Middle-earth: Shadow of War (LithTech Firebird, Havok Script 1.x embedded)
// 2026-11-11. Game-thread tick + a doorway into the game's own script VM.
//
// The game runs its scripts through Havok Script (HKS, a Lua 5.1 fork). lua_pcall is called ~1000x/s on one thread
// (the game/script thread): hooking it gives
//   - the engine-thread tick (queued trainer work, panel freezes)
//   - a live lua_State to compile and run HKS source in (HKS.EXEC): the game's own script API, no memory editing.
// HKS API (exe 1.0.8874 / game 1.21, normal code section, not the DRM-packed one):
//   lua_pcall(L, nargs, nres, errf)                      +0x406344   (base lib 'pcall' +0x1D02F94)
//   hksL_loadbuffer(L, &G->compilerSettings, buf, len, name)  +0x55B920   (base lib 'loadstring'; G = [L+0x10], settings at G+0x560)
//   lua_tostring(L, idx)                                 +0x1159344  (base lib 'print')
//   lua_getfield(L, idx, k) +0x4081E4, lua_pushvalue +0x1159378, lua_call(L, nargs, nres) +0x115F8CC
//   lua_State: top @+0x48, base @+0x50, TValue 16 bytes (type tag dword @0, value @8), LUA_GLOBALSINDEX -10002
#include "common.h"
#include "MinHook.h"
#include <fstream>
#include <psapi.h>
#include <atomic>
#include <mutex>
#include <thread>

namespace wb {
void engine_tick_here();          // core.cpp: drain queued work + Lua ticks + panel ticks on this thread
bool core_stopping();

namespace {
using pcall_t = int(__fastcall*)(uintptr_t L, int nargs, int nres, int errf);
using load_t = int(__fastcall*)(uintptr_t L, uintptr_t cs, const char* buf, size_t len, const char* name);
using tostr_t = const char*(__fastcall*)(uintptr_t L, int idx);
constexpr uintptr_t kPcall = 0x406344, kLoad = 0x55B920, kToStr = 0x1159344;

pcall_t o_pcall = nullptr;                // trampoline (original lua_pcall)
load_t f_load = nullptr;
tostr_t f_tostr = nullptr;
std::atomic<uintptr_t> g_L{0};            // lua_State seen by the last tick
std::atomic<uint64_t> g_calls{0};
thread_local int t_depth = 0;             // >0 while inside a game pcall or our own work: no nested ticks
ULONGLONG g_last_tick = 0;

int __fastcall hk_pcall(uintptr_t L, int nargs, int nres, int errf) {
    ++g_calls;
    if (t_depth == 0 && !core_stopping()) {
        const ULONGLONG now = GetTickCount64();
        if (now != g_last_tick) {         // ~once per timer step (15.6 ms), only from an outermost call
            g_last_tick = now; g_L = L;
            ++t_depth; engine_tick_here(); --t_depth;
        }
    }
    ++t_depth; const int r = o_pcall(L, nargs, nres, errf); --t_depth;
    return r;
}

// SEH-only helper (no C++ objects): load + pcall + tostring, stack restored afterwards
// A native fault inside the VM leaves it inconsistent: the game crashed ~10 s after each caught fault (2026-11-11,
// twice). So a fault is recorded (code + address) and the VM is marked tainted: HKS calls are refused until the game
// restarts instead of carrying on.
struct ExecOut { int load_rc = -1, call_rc = -1; char text[16384]; bool fault = false; DWORD code = 0; uintptr_t at = 0; };
std::atomic<bool> g_tainted{false};
int fault_filter(EXCEPTION_POINTERS* ep, ExecOut* o) {
    o->code = ep->ExceptionRecord->ExceptionCode; o->at = (uintptr_t)ep->ExceptionRecord->ExceptionAddress;
    return EXCEPTION_EXECUTE_HANDLER;
}
void exec_raw(uintptr_t L, const char* code, size_t len, const char* name, ExecOut* o) {
    o->text[0] = 0;
    uintptr_t top0 = *(uintptr_t*)(L + 0x48);
    __try {
        const uintptr_t G = *(uintptr_t*)(L + 0x10);
        o->load_rc = f_load(L, G + 0x560, code, len, name);
        if (o->load_rc == 0) o->call_rc = o_pcall(L, 0, 1, 0);
        const char* s = f_tostr(L, -1);
        if (s) { size_t n = strnlen(s, sizeof o->text - 1); memcpy(o->text, s, n); o->text[n] = 0; }
        *(uintptr_t*)(L + 0x48) = top0;
    } __except (fault_filter(GetExceptionInformation(), o)) {
        o->fault = true; g_tainted = true;
        *(uintptr_t*)(L + 0x48) = top0;
    }
}
std::string fault_text(const ExecOut* o) {
    char b[160]; const uintptr_t gb = game_base();
    if (o->at >= gb && o->at < gb + 0x8000000) sprintf_s(b, "native exception 0x%08lX at ShadowOfWar.exe+0x%llX", o->code, (unsigned long long)(o->at - gb));
    else sprintf_s(b, "native exception 0x%08lX at %s", o->code, hexstr(o->at).c_str());
    return std::string(b) + " - the game's script VM is now unsafe: save nothing, restart the game";
}
} // namespace

// ------------------------------------------------------------------ HKS runner for the panel (game thread only)
// bin\scripts\sow_lib.hks = helper library loaded into the game VM (WB.*); reloaded when WB_LIB is gone (new VM) or
// the file changed on disk.
static FILETIME g_lib_time{};
static ULONGLONG g_lib_check = 0;
static bool run_chunk(uintptr_t L, const std::string& code, const char* name, std::string* out, std::string* err) {
    auto o = std::make_unique<ExecOut>();
    ++t_depth; exec_raw(L, code.data(), code.size(), name, o.get()); --t_depth;
    if (o->fault) { if (err) *err = fault_text(o.get()); log("[sow] %s", fault_text(o.get()).c_str()); return false; }
    if (o->load_rc != 0 || o->call_rc != 0) { if (err) *err = o->text[0] ? o->text : "HKS error"; return false; }
    if (out) *out = o->text;
    return true;
}
static void ensure_lib(uintptr_t L) {
    const ULONGLONG now = GetTickCount64();
    if (now - g_lib_check < 2000) return;
    g_lib_check = now;
    const std::string path = g_cfg.root + "\\scripts\\sow_lib.hks";
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &fa)) return;
    std::string have;
    const bool changed = CompareFileTime(&fa.ftLastWriteTime, &g_lib_time) != 0;
    if (!changed && run_chunk(L, "return WB_LIB", "=check", &have, nullptr) && !have.empty()) return;
    std::ifstream f(path, std::ios::binary); std::string src((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::string err;
    if (run_chunk(L, src, "@sow_lib.hks", nullptr, &err)) { g_lib_time = fa.ftLastWriteTime; log("[sow] sow_lib.hks loaded into the game VM"); }
    else { g_lib_time = fa.ftLastWriteTime; log("[sow] sow_lib.hks failed: %s", err.c_str()); }
}
// World-ready gate for the PANEL's automatic script work (getters, keep, presses). 2026-11-11 23:29: saved "Infinite
// Health = on" + live getters ran while a save was loading; the player object was stale and the VM faulted. Ready =
// player object [[exe+0x26FFA98]+0x888] non-null and unchanged for 4 s (loading / travel / death replace it).
static uintptr_t g_ready_P = 0;
static ULONGLONG g_ready_since = 0;
static std::atomic<uint64_t> g_world_epoch{ 0 };   // +1 whenever the player object changes (load, travel, death)
static std::mutex g_ready_mtx;                      // review 2026-11-15: called from the game, UI and worker threads
bool core_stopping();
bool sow_world_ready() {
    // a small watcher samples the player pointer 4x a second, so a load (P -> 0 -> new P) always bumps the world
    // epoch even while the game's script tick is stalled on a loading screen
    static std::once_flag s_watch;
    std::call_once(s_watch, []() { std::thread([]() { while (!core_stopping()) { sow_world_ready(); Sleep(250); } }).detach(); });
    uintptr_t root = 0, P = 0;
    const uintptr_t gb = game_base();
    if (gb && safe_read(gb + 0x26FFA98, &root, 8) && root) safe_read(root + 0x888, &P, 8);
    const ULONGLONG now = GetTickCount64();
    std::lock_guard<std::mutex> lk(g_ready_mtx);
    static bool s_logged_ready = false;
    if (P != g_ready_P) {
        // world changes go to the log: a crash report then shows whether the game was loading, in the world or in a menu
        if (!g_ready_P) log("[world] player loaded (a save is loading)");
        else if (!P) log("[world] player gone (loading screen or main menu)");
        else log("[world] new player object (reload, death or travel)");
        g_ready_P = P; g_ready_since = now; ++g_world_epoch; s_logged_ready = false;
    }
    const bool ready = P != 0 && now - g_ready_since >= 15000;   // 15 s: load-time game scripts run well after the player exists (crash 2026-11-13 04:04)
    if (ready && !s_logged_ready) { s_logged_ready = true; log("[world] in the game world - trainer options can be used"); }
    return ready;
}
// which world load this is (bumped by every player-pointer change the watcher or a caller sees)
uint64_t sow_world_epoch() { sow_world_ready(); return g_world_epoch.load(); }
bool hks_run_panel(const std::string& code, std::string* out, std::string* err) {
    if (!sow_world_ready()) { if (err) *err = "not in the world yet (loading?)"; return false; }
    return hks_run(code, out, err);
}
bool hks_run(const std::string& code, std::string* out, std::string* err) {
    if (g_tainted) { if (err) *err = "script VM tainted by an earlier native fault - restart the game"; return false; }
    const uintptr_t L = g_L.load();
    if (!L || !o_pcall) { if (err) *err = "game script VM not ready"; return false; }
    if (GetCurrentThreadId() != main_thread_id()) { if (err) *err = "hks_run off the game thread"; return false; }
    ensure_lib(L);
    return run_chunk(L, code, "=panel", out, err);
}

// ------------------------------------------------------------------ crash recorder (observe only)
// First-chance access violations / illegal instructions anywhere in the process are written to trainer.log with the
// faulting place and every stack slot that points into the game exe, the trainer or another non-Windows DLL (a mod),
// each named "Name.dll+0x..". Returns CONTINUE_SEARCH: the game's own handling is unchanged.
// Many of these are caught by whoever caused them and are harmless; a real crash is usually the LAST one before the
// log ends. 2026-11-18: a mod's handled fault repeated 40x during loading filled the old 40-line cap, so the fatal one
// was never written. Now each place is written the first 3 times, then only at its 10th / 100th / 1000th time, and
// the total cap is 200 lines - a new place is always written.
static std::atomic<int> g_veh_lines{0};
static constexpr int kVehSites = 64;
static std::atomic<uintptr_t> g_veh_site[kVehSites];
static std::atomic<uint32_t> g_veh_hits[kVehSites];
static uint32_t veh_hit(uintptr_t at) {
    for (int i = 0; i < kVehSites; ++i) {
        uintptr_t cur = g_veh_site[i].load();
        if (cur == at) return ++g_veh_hits[i];
        if (cur == 0) { uintptr_t z = 0; if (g_veh_site[i].compare_exchange_strong(z, at) || z == at) return ++g_veh_hits[i]; }
    }
    return 1;                                                   // table full: treat as new
}
// a register / stack value that points at readable text (error messages the game passes to its fatal handler)
static bool text_at(uintptr_t p, char* out, size_t n) {
    if (p < 0x10000) return false;
    char b[121] = {};
    if (!safe_read(p, b, 120)) return false;
    size_t len = 0;
    while (len < 120 && b[len] >= 0x20 && b[len] < 0x7F) ++len;
    if (len < 12 || (len < 120 && b[len] != 0)) return false;   // must be a whole, readable C string
    int letters = 0; for (size_t i = 0; i < len; ++i) if (isalpha((unsigned char)b[i])) ++letters;
    if (letters < (int)len / 2) return false;
    b[len] = 0; strncpy_s(out, n, b, _TRUNCATE);
    return true;
}
static LONG CALLBACK veh_recorder(EXCEPTION_POINTERS* ep) {
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_STACK_OVERFLOW
        && code != 0xC0000409 /*fail fast*/ && code != EXCEPTION_INT_DIVIDE_BY_ZERO) return EXCEPTION_CONTINUE_SEARCH;
    const uintptr_t at_a = (uintptr_t)ep->ExceptionRecord->ExceptionAddress;
    const uint32_t hit = veh_hit(at_a);
    if (hit > 3 && hit != 10 && hit != 100 && hit != 1000) return EXCEPTION_CONTINUE_SEARCH;
    if (g_veh_lines.fetch_add(1) >= 200) return EXCEPTION_CONTINUE_SEARCH;
    const uintptr_t gb = game_base();
    // exe+0x.. / ShadowOfWarTrainer.dll+0x.. / FuryPatch.dll+0x..; Windows DLLs only for the faulting place itself
    auto nm = [&](uintptr_t a, char* b, size_t n, bool keep_system) -> bool {
        if (gb && a >= gb && a < gb + 0x8000000) { sprintf_s(b, n, "exe+0x%llX", (unsigned long long)(a - gb)); return true; }
        bool sys = false;
        if (modsnap_find(a, b, n, &sys)) return keep_system || !sys;
        sprintf_s(b, n, "0x%llX", (unsigned long long)a);
        return false;
    };
    char at[96], acc[48] = "";
    nm(at_a, at, sizeof at, true);
    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2)
        sprintf_s(acc, " (%s 0x%llX)", ep->ExceptionRecord->ExceptionInformation[0] == 1 ? "write" : ep->ExceptionRecord->ExceptionInformation[0] == 8 ? "exec" : "read",
                  (unsigned long long)ep->ExceptionRecord->ExceptionInformation[1]);
    std::string st;
    const uintptr_t* sp = (const uintptr_t*)ep->ContextRecord->Rsp;
    int found = 0;
    for (int i = 0; i < 512 && found < 14; ++i) {
        uintptr_t v; if (!safe_read((uintptr_t)(sp + i), &v, 8)) break;
        char b[96]; if (nm(v, b, sizeof b, false)) { st += " "; st += b; ++found; }
    }
    // text the game (or a mod) handed over just before: its fatal-error handler gets the message in a register
    std::string txt;
    { const CONTEXT* c = ep->ContextRecord;
      const uintptr_t regs[] = { c->Rcx, c->Rdx, c->R8, c->R9, c->Rbx, c->Rsi, c->Rdi, c->Rax };
      int got = 0; char t[128];
      auto add = [&](uintptr_t p) {
          if (got >= 2 || !text_at(p, t, sizeof t)) return;
          const std::string piece = std::string(" | text: \"") + t + "\"";
          if (txt.find(piece) == std::string::npos) { txt += piece; ++got; }   // the same message in a register and on the stack: once
      };
      for (uintptr_t r : regs) add(r);
      for (int i = 0; i < 24 && got < 2; ++i) { uintptr_t v; if (!safe_read((uintptr_t)(sp + i), &v, 8)) break; add(v); } }
    char rep[48] = "";
    if (hit > 1) sprintf_s(rep, " [%u%s time here]", hit, hit == 2 ? "nd" : hit == 3 ? "rd" : "th");
    log("[crash-rec] 0x%08lX at %s%s thread %lu%s%s | stack:%s%s", code, at, acc, GetCurrentThreadId(),
        GetCurrentThreadId() == main_thread_id() ? " (game thread)" : "", rep, st.c_str(), txt.c_str());
    return EXCEPTION_CONTINUE_SEARCH;
}

// Every SOW feature uses fixed offsets of the Steam exe 1.0.8874 (PE TimeDateStamp 0x5B7F5782, SizeOfImage 0x73FD000).
// Review 2026-11-14: on another exe build the hooks / writes would land in arbitrary code. Checked once: the PE header
// stamp + image size, and the pcall / loadbuffer Havok Script prologues. Unsupported -> no hook, no ticks, SOW commands refused.
bool sow_game_supported() {
    static int s_ok = -1;
    if (s_ok >= 0) return s_ok == 1;
    const uintptr_t gb = game_base(); if (!gb) return false;
    uint32_t lfanew = 0, ts = 0, soi = 0;
    safe_read(gb + 0x3C, &lfanew, 4);
    if (lfanew && lfanew < 0x1000) { safe_read(gb + lfanew + 8, &ts, 4); safe_read(gb + lfanew + 0x50, &soi, 4); }
    static const uint8_t kP[4] = { 0x48, 0x89, 0x5C, 0x24 }, kL[4] = { 0x40, 0x53, 0x57, 0x48 };
    uint8_t p[4] = {}, l[4] = {};
    safe_read(gb + kPcall, p, 4); safe_read(gb + kLoad, l, 4);
    const bool ok = ts == 0x5B7F5782 && soi == 0x73FD000 && !memcmp(p, kP, 4) && !memcmp(l, kL, 4);
    s_ok = ok ? 1 : 0;
    log("[sow] game build check: stamp %08X size %08X -> %s", ts, soi, ok ? "supported (1.0.8874)" : "NOT SUPPORTED - trainer features stay off");
    if (!ok) toast("Shadow of War Trainer: this game version is not supported (needs Steam 1.0.8874) - features are off");
    return ok;
}

bool sow_install_tick() {
    const uintptr_t gb = game_base();
    if (!gb) return false;
    AddVectoredExceptionHandler(0, veh_recorder);          // last in line: only sees what reaches the VEH chain
    log("[sow] crash recorder on");
    f_load = (load_t)(gb + kLoad); f_tostr = (tostr_t)(gb + kToStr);
    // sanity: the three functions must start the way they do in 1.0.8874 (prologues), else stay off
    uint8_t p[4] = {}, l[4] = {}, t[4] = {};
    safe_read(gb + kPcall, p, 4); safe_read(gb + kLoad, l, 4); safe_read(gb + kToStr, t, 4);
    log("[sow] lua_pcall %02X %02X %02X %02X  loadbuffer %02X %02X %02X %02X  tostring %02X %02X %02X %02X",
        p[0], p[1], p[2], p[3], l[0], l[1], l[2], l[3], t[0], t[1], t[2], t[3]);
    if (!sow_game_supported()) return false;
    const bool okh = MH_CreateHook((void*)(gb + kPcall), &hk_pcall, (void**)&o_pcall) == MH_OK && MH_EnableHook((void*)(gb + kPcall)) == MH_OK;
    log("[sow] HKS lua_pcall tick hook %s", okh ? "installed" : "FAILED");
    return okh;
}

// HKS.EXEC {code, raw, timeout_ms}: compile + run Havok Script source in the game's VM on the game thread.
// Not raw: the code runs as a function body and every return value comes back through tostring, tab separated.
json sow_cmd_hks_exec(const json& a) {
    std::string code = a.value("code", std::string());
    if (code.empty()) return fail("BAD_ARGS", "code is required");
    if (!o_pcall) return fail("NO_HOOK", "HKS hook not installed");
    if (g_tainted) return fail("TAINTED", "the game's script VM had a native fault earlier - restart the game before running more script");
    if (!a.value("raw", false))
        code = "local __r = { (function() " + code + "\nend)() }\nlocal __o = ''\nfor __i = 1, #__r do __o = __o .. (__i > 1 and '\\t' or '') .. tostring(__r[__i]) end\nreturn __o";
    return run_on_main([code]() -> json {
        const uintptr_t L = g_L.load();
        if (!L) return fail("NO_STATE", "no lua_State seen yet");
        ensure_lib(L);
        auto o = std::make_unique<ExecOut>();
        ++t_depth; exec_raw(L, code.data(), code.size(), "=bridge", o.get()); --t_depth;
        if (o->fault) { log("[sow] %s", fault_text(o.get()).c_str()); return fail("FAULT", fault_text(o.get())); }
        if (o->load_rc != 0) return fail("COMPILE", o->text[0] ? o->text : "hksL_loadbuffer failed (rc " + std::to_string(o->load_rc) + ")");
        if (o->call_rc != 0) return fail("SCRIPT_ERROR", o->text[0] ? o->text : "error (rc " + std::to_string(o->call_rc) + ")");
        return ok({ {"result", std::string(o->text)} });
    }, a.value("timeout_ms", 5000));
}

json sow_cmd_status(const json&) {
    return ok({ {"hook", o_pcall != nullptr}, {"lua_state", hexstr(g_L.load())}, {"pcalls", g_calls.load()},
                {"main_thread", main_thread_id()}, {"ticks", main_ticks()} });
}
} // namespace wb
