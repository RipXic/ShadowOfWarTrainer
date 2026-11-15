// Shadow of War Trainer - signature registry. signatures.json (next to the DLL) holds named byte patterns.
// Entry: {"name","pattern","resolve":"match|rip|call","offset":0,"rip_disp":3,"instr_len":7,"add":0,"note":""}
//   match - address of the match (+offset)
//   rip   - instruction at match+offset; disp32 at +rip_disp, next instruction at +instr_len -> target (+add)
//   call  - E8 call at match+offset -> its target
// Usable everywhere as "sig:<name>" in address expressions.
#include "common.h"
#include <fstream>
#include <map>
#include <cstring>
#include <algorithm>

namespace wb {

struct Sig {
    std::string name, pattern, resolve = "match", note;
    int offset = 0, rip_disp = 3, instr_len = 7; int64_t add = 0;
    uintptr_t addr = 0; int matches = 0;
    bool from_fns = false; bool hooked = false;   // from functions.json (never saved to signatures.json); found under a hook
};
static json g_fn_defs = json::array();          // functions.json entries + resolved address (FN.DEFS)
static std::mutex g_sig_mtx;
static std::map<std::string, Sig> g_sigs;

// One region, SEH-guarded (the packed exe can change page protection while we read). Plain types only (no unwinding).
static int scan_region(const uint8_t* p, size_t size, const int* pat, size_t n, int anchor, uintptr_t* out, int max_out) {
    int found = 0;
    __try {
        size_t limit = size - n, i = 0;
        while (i <= limit && found < max_out) {
            const void* f = memchr(p + i + anchor, pat[anchor], limit - i + 1);
            if (!f) break;
            size_t pos = (const uint8_t*)f - p - anchor, k = 0;
            for (; k < n; ++k) if (pat[k] >= 0 && p[pos + k] != (uint8_t)pat[k]) break;
            if (k == n) out[found++] = (uintptr_t)(p + pos);
            i = pos + 1;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return found;
}

static void scan_pattern(const std::vector<int>& pat, int max_hits, std::vector<uintptr_t>& hits) {
    size_t gsz = 0; uintptr_t gb = game_base(&gsz);
    int anchor = -1; for (size_t i = 0; i < pat.size(); ++i) if (pat[i] >= 0) { anchor = (int)i; break; }
    if (anchor < 0 || !gb) return;
    uintptr_t buf[64];
    for (auto& r : readable_regions(gb, gsz)) {
        if (!r.exec || r.size < pat.size()) continue;
        int want = std::min(64, max_hits - (int)hits.size());
        int n = scan_region((const uint8_t*)r.start, r.size, pat.data(), pat.size(), anchor, buf, want);
        for (int i = 0; i < n; ++i) hits.push_back(buf[i]);
        if ((int)hits.size() >= max_hits) return;
    }
}

static uintptr_t resolve_one(Sig& s, std::string* why = nullptr) {
    std::vector<int> pat;
    if (!parse_pattern(s.pattern, pat)) { if (why) *why = "bad pattern"; return 0; }
    std::vector<uintptr_t> hits; scan_pattern(pat, 8, hits);
    s.matches = (int)hits.size(); s.hooked = false;
    if (hits.empty() && s.resolve == "match" && s.offset == 0 && pat.size() >= 13) {
        // A hook of ours (MinHook: E9 rel32) may sit on the function start: retry with the first 5 bytes as wildcards
        // and accept only hits that start with a jmp.
        std::vector<int> tail = pat; for (int i = 0; i < 5; ++i) tail[i] = -1;
        std::vector<uintptr_t> h2; scan_pattern(tail, 8, h2);
        for (auto x : h2) { uint8_t b = 0; if (read_as(x, b) && b == 0xE9) hits.push_back(x); }
        s.matches = (int)hits.size(); s.hooked = !hits.empty();
    }
    if (hits.empty()) { if (why) *why = "no match"; return 0; }
    uintptr_t at = hits[0] + s.offset;
    if (s.resolve == "rip") { int32_t d; if (!read_as(at + s.rip_disp, d)) return 0; return at + s.instr_len + d + s.add; }
    if (s.resolve == "call") { int32_t d; if (!read_as(at + 1, d)) return 0; return at + 5 + d + s.add; }
    return at + s.add;
}

static Sig from_json(const json& j) {
    Sig s; s.name = j.value("name", ""); s.pattern = j.value("pattern", ""); s.resolve = j.value("resolve", "match");
    s.offset = j.value("offset", 0); s.rip_disp = j.value("rip_disp", 3); s.instr_len = j.value("instr_len", 7);
    s.add = j.value("add", (int64_t)0); s.note = j.value("note", "");
    return s;
}
static json to_json(const Sig& s) {
    return { {"name", s.name}, {"pattern", s.pattern}, {"resolve", s.resolve}, {"offset", s.offset}, {"rip_disp", s.rip_disp},
             {"instr_len", s.instr_len}, {"add", s.add}, {"note", s.note} };
}
static void save_sigs() {
    json arr = json::array();
    for (auto& kv : g_sigs) if (!kv.second.from_fns) arr.push_back(to_json(kv.second));
    std::ofstream(g_cfg.root + "\\signatures.json") << arr.dump(2);
}

// functions.json (framework step 3): the game's named functions exported from the Game Link kb. Each one resolves as
// "sig:<Name>" (signature first, then the rva when it was verified on this exe build). wf.lua reads FN.DEFS and makes
// every entry a checked WF.fn.
static void fns_load_locked() {
    g_fn_defs = json::array();
    std::ifstream f(g_cfg.root + "\\functions.json");
    if (!f) return;
    json doc; try { doc = json::parse(f); } catch (std::exception& e) { log("functions.json invalid: %s", e.what()); return; }
    // rva fallback only when the export was written for this exe (file size + PE timestamp match)
    uintptr_t gb = game_base(); int okc = 0, n = 0;
    bool same_exe = false;
    if (doc.contains("exe") && doc["exe"].is_object() && gb) {
        char path[MAX_PATH] = {}; GetModuleFileNameA((HMODULE)gb, path, MAX_PATH);
        WIN32_FILE_ATTRIBUTE_DATA fa{}; uint32_t pe_time = 0; int32_t lfanew = 0;
        if (GetFileAttributesExA(path, GetFileExInfoStandard, &fa) && read_as(gb + 0x3C, lfanew) && read_as(gb + lfanew + 8, pe_time)) {
            uint64_t size = ((uint64_t)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
            same_exe = doc["exe"].value("size", (uint64_t)0) == size && doc["exe"].value("pe_time", (uint32_t)0) == pe_time;
        }
        log("[fn] functions.json %s this exe", same_exe ? "matches" : "was written for another build of");
    }
    for (auto& j : doc.value("functions", json::array())) {
        std::string name = j.value("name", ""); if (name.empty()) continue;
        ++n; uintptr_t a = 0; std::string how, why;
        if (j.contains("pattern")) {
            Sig s = from_json(j); s.from_fns = true; s.note = j.value("notes", "");
            if (j.contains("offset")) s.offset = j.value("offset", 0);
            s.addr = resolve_one(s, &why); a = s.addr; how = s.hooked ? "signature (under a hook)" : "signature";
            if (s.matches > 1) why = "pattern matched " + std::to_string(s.matches) + " times - using the first";
            if (a || !g_sigs.count(name) || g_sigs[name].from_fns) g_sigs[name] = s;
        }
        if (!a && j.contains("rva") && j.value("rva_ok", false) && same_exe) {
            a = gb + std::stoull(j.value("rva", "0"), nullptr, 16); how = "rva (verified on this build)";
            Sig s; s.name = name; s.from_fns = true; s.addr = a; s.matches = 1; s.note = j.value("notes", ""); g_sigs[name] = s;
        }
        json d = j; d["addr"] = hexstr(a); d["found_by"] = a ? how : ("not found: " + (why.empty() ? "no signature, rva from another build" : why));
        if (a) ++okc; else log("[fn] %s not found (%s)", name.c_str(), why.c_str());
        g_fn_defs.push_back(d);
    }
    log("[fn] %d/%d named functions resolved from functions.json", okc, n);
}

void sigs_init() {
    std::ifstream f(g_cfg.root + "\\signatures.json");
    if (!f) { log("signatures.json missing - no signatures."); return; }
    json arr; try { arr = json::parse(f); } catch (std::exception& e) { log("signatures.json invalid: %s", e.what()); return; }
    int okc = 0, n = 0;
    std::lock_guard<std::mutex> lk(g_sig_mtx);
    for (auto& j : arr) {
        Sig s = from_json(j); if (s.name.empty()) continue;
        std::string why; s.addr = resolve_one(s, &why); ++n;
        if (s.addr) ++okc; else log("[sig] %s unresolved (%s)", s.name.c_str(), why.c_str());
        if (s.matches > 1) log("[sig] %s matched %d times - using the first", s.name.c_str(), s.matches);
        g_sigs[s.name] = s;
    }
    log("[sig] %d/%d signatures resolved", okc, n);
    fns_load_locked();
}


std::string sig_name_at(uintptr_t a) {
    for (auto& kv : g_sigs) if (kv.second.addr == a) return kv.first;
    return std::string();
}
uintptr_t sig_addr(const std::string& name) {
    std::lock_guard<std::mutex> lk(g_sig_mtx);
    auto it = g_sigs.find(name); return it == g_sigs.end() ? 0 : it->second.addr;
}

json sigs_cmd_list(const json& a) {
    std::string f = a.value("filter", ""); uintptr_t gb = game_base();
    json arr = json::array();
    std::lock_guard<std::mutex> lk(g_sig_mtx);
    for (auto& kv : g_sigs) {
        if (!f.empty() && kv.first.find(f) == std::string::npos) continue;
        auto& s = kv.second;
        arr.push_back({ {"name", s.name}, {"addr", hexstr(s.addr)}, {"rva", s.addr ? hexstr(s.addr - gb) : ""}, {"matches", s.matches}, {"note", s.note} });
    }
    return ok(arr);
}

json fns_cmd_defs(const json& a) {
    std::lock_guard<std::mutex> lk(g_sig_mtx);
    if (a.value("reload", false)) fns_load_locked();
    return ok(g_fn_defs);
}

json sigs_cmd_test(const json& a) {
    Sig s = from_json(a); s.name = "test";
    std::vector<int> pat; if (!parse_pattern(s.pattern, pat)) return fail("BAD_ARG", "pattern like '48 8B 05 ?? ?? ?? ??'");
    std::vector<uintptr_t> hits; scan_pattern(pat, 16, hits);
    json h = json::array(); uintptr_t gb = game_base();
    for (auto x : hits) h.push_back(hexstr(x - gb));
    std::string why; uintptr_t r = resolve_one(s, &why);
    return ok({ {"matches", hits.size()}, {"rvas", h}, {"resolved", hexstr(r)} });
}

json sigs_cmd_add(const json& a) {
    Sig s = from_json(a);
    if (s.name.empty() || s.pattern.empty()) return fail("MISSING_ARG", "name and pattern are required");
    std::string why; s.addr = resolve_one(s, &why);
    std::lock_guard<std::mutex> lk(g_sig_mtx);
    g_sigs[s.name] = s; save_sigs();
    return ok({ {"name", s.name}, {"addr", hexstr(s.addr)}, {"matches", s.matches} }, s.addr ? "saved and resolved" : "saved, but " + why);
}

} // namespace wb
