#include <fstream>
#include <cmath>
// sow_prog.cpp - Shadow of War progression through the game's OWN functions (2026-11-11).
//   Player P = [[exe+0x26FFA98] + 0x888]; inventory INV = [P + 0x2348]; XP level = [[P + 0x26A8] + 0x568] (0-based)
//   Everything owned (Mirian, skill points, gems, gear, unlocked skills) is an inventory item.
//   AddItem(INV, rec, n) +0x1C01E0   RemoveItem(INV, rec, n) +0x4F163C   GetItemCount(INV, rec) +0x509074
//   DB table descriptor = [exe + global]: +0x28 count u32, +0x38 entries (0x28 bytes: +4 hash, +0x20 name char*);
//   a record pointer IS the entry address.
// Every native call runs on the game thread through guarded_call (faults are caught and reported).
#include "common.h"
#include <set>
#include <regex>

namespace wb {
namespace {
constexpr uintptr_t kPlayerRoot = 0x26FFA98, kAddItem = 0x1C01E0, kRemoveItem = 0x4F163C, kGetCount = 0x509074;
constexpr uintptr_t kTblItems = 0x2700530, kTblSkills = 0x27004D8;

uintptr_t rq(uintptr_t a) { uintptr_t v = 0; return a && safe_read(a, &v, 8) ? v : 0; }

bool player_inv(uintptr_t& P, uintptr_t& INV, std::string& err) {
    const uintptr_t gb = game_base();
    const uintptr_t root = rq(gb + kPlayerRoot);
    P = root ? rq(root + 0x888) : 0;
    INV = P ? rq(P + 0x2348) : 0;
    if (!P) { err = "no player (not in game yet?)"; return false; }
    if (!INV) { err = "player has no inventory pointer"; return false; }
    return true;
}

// record by exact name in a DB table (read-only scan; tables are a few thousand entries)
uintptr_t find_record_scan(uintptr_t table_global, const std::string& name, std::string& err);
// cached: records live as long as the game database (loaded once at startup); the cache is re-checked by name
uintptr_t find_record(uintptr_t table_global, const std::string& name, std::string& err) {
    static std::mutex mtx; static std::map<std::pair<uintptr_t, std::string>, uintptr_t> cache;
    const auto key = std::make_pair(table_global, name);
    { std::lock_guard<std::mutex> lk(mtx); auto f = cache.find(key);
      if (f != cache.end()) { char b[128]; const uintptr_t np = rq(f->second + 0x20);
          if (np && safe_read(np, b, sizeof b)) { b[sizeof b - 1] = 0; if (name == b) return f->second; } cache.erase(f); } }
    const uintptr_t r = find_record_scan(table_global, name, err);
    if (r) { std::lock_guard<std::mutex> lk(mtx); cache[key] = r; }
    return r;
}
uintptr_t find_record_scan(uintptr_t table_global, const std::string& name, std::string& err) {
    const uintptr_t desc = rq(game_base() + table_global);
    if (!desc) { err = "table not loaded"; return 0; }
    uint32_t n = 0; safe_read(desc + 0x28, &n, 4);
    const uintptr_t ents = rq(desc + 0x38);
    if (!ents || n == 0 || n > 200000) { err = "bad table header"; return 0; }
    char buf[128];
    for (uint32_t i = 0; i < n; ++i) {
        const uintptr_t e = ents + (uintptr_t)i * 0x28, np = rq(e + 0x20);
        if (!np || !safe_read(np, buf, sizeof buf)) continue;
        buf[sizeof buf - 1] = 0;
        if (name == buf) return e;
    }
    err = "no record named '" + name + "'";
    return 0;
}

json item_count_on_main(uintptr_t INV, uintptr_t rec) {
    json r = guarded_call(game_base() + kGetCount, { INV, rec }, "u64", true);
    if (!r.value("ok", false)) return r;
    return ok({ {"count", (int64_t)(int32_t)r["data"]["ret_i"].get<int64_t>()} });
}

// common front part of the item commands: resolve everything, then run fn on the game thread
json with_item(const json& a, std::function<json(uintptr_t P, uintptr_t INV, uintptr_t rec)> fn) {
    const std::string name = a.value("name", std::string());
    if (name.empty()) return fail("BAD_ARGS", "name is required (e.g. PC_Mirian, SkillPoint)");
    const uintptr_t tbl = a.contains("table") ? (uintptr_t)a["table"].get<uint64_t>() : kTblItems;
    return run_on_main([=]() -> json {
        uintptr_t P, INV; std::string err;
        if (!player_inv(P, INV, err)) return fail("NO_PLAYER", err);
        const uintptr_t rec = find_record(tbl, name, err);
        if (!rec) return fail("NO_RECORD", err);
        return fn(P, INV, rec);
    }, a.value("timeout_ms", 5000));
}
} // namespace

json sow_cmd_inv(const json&) {
    return run_on_main([]() -> json {
        uintptr_t P, INV; std::string err;
        if (!player_inv(P, INV, err)) return fail("NO_PLAYER", err);
        const uintptr_t lv = rq(P + 0x26A8); int32_t level0 = -1; if (lv) safe_read(lv + 0x568, &level0, 4);
        return ok({ {"player", hexstr(P)}, {"inventory", hexstr(INV)}, {"level_obj", hexstr(lv)}, {"level", level0 + 1} });
    }, 3000);
}

// SOW.ITEM {name} -> count, record
json sow_cmd_item(const json& a) {
    return with_item(a, [](uintptr_t, uintptr_t INV, uintptr_t rec) -> json {
        json c = item_count_on_main(INV, rec); if (!c.value("ok", false)) return c;
        c["data"]["record"] = hexstr(rec); return c;
    });
}

// SOW.HEALTH {fill} - Talion's health straight from the game's health component, no script calls (the old Havok
// Script keep crashed the game twice on 2026-11-13: a script call on an empty player object is a fault the VM cannot
// catch). The game's own getters: GetHealth exe+0x72DD30 = [[char+0x26A8]+0x42C], GetHealthMax exe+0x17DC610 =
// [[char+0x26A8]+0x428]; +0x430 = current/max. Writes only when the three agree (a moved offset reads as "not found").
json sow_cmd_health(const json& a) {
    const bool fill = a.value("fill", false);
    uintptr_t root = 0, P = 0, H = 0; const uintptr_t gb = game_base();
    if (!sow_world_ready() || !safe_read(gb + kPlayerRoot, &root, 8) || !root || !safe_read(root + 0x888, &P, 8) || !P) return fail("NO_PLAYER", "not in the world");
    if (!safe_read(P + 0x26A8, &H, 8) || !H) return fail("NO_PLAYER", "no health component");
    float h[3] = {}; if (!safe_read(H + 0x428, h, 12)) return fail("READ", "health not readable");
    const float mx = h[0], cur = h[1], pct = h[2];
    const bool sane = mx >= 1.f && mx < 1e7f && cur >= 0.f && cur <= mx * 1.001f && std::fabs(cur / mx - pct) < 0.02f;
    if (!sane) return fail("NOT_FOUND", "health values not where expected (game updated?)");
    char t[64]; snprintf(t, sizeof t, "%.0f / %.0f", cur, mx);
    const bool doit = fill && cur > 0.f && cur < mx;
    if (doit) { const float v[2] = { mx, 1.f }; safe_write(H + 0x42C, v, 8); }
    return ok({ {"text", t}, {"health", cur}, {"max", mx}, {"filled", doit} });
}

// SOW.ITEM_ADD {name, count} -> AddItem through the game's inventory (stacking, events, UI handled by the game)
json sow_cmd_item_add(const json& a) {
    const int64_t n = a.value("count", (int64_t)1);
    if (n <= 0 || n > 10000000) return fail("BAD_ARGS", "count 1..10000000");
    const bool each = a.value("each", false);
    if (each && n > 1000) return fail("BAD_ARGS", "each: count 1..1000");
    return with_item(a, [n, each](uintptr_t, uintptr_t INV, uintptr_t rec) -> json {
        json before = item_count_on_main(INV, rec);
        json r;
        if (each) { for (int64_t i = 0; i < n; ++i) { r = guarded_call(game_base() + kAddItem, { INV, rec, 1 }, "u64", true); if (!r.value("ok", false)) return r; } }
        else r = guarded_call(game_base() + kAddItem, { INV, rec, (uint64_t)n }, "u64");
        if (!r.value("ok", false)) return r;
        json after = item_count_on_main(INV, rec);
        return ok({ {"before", before["data"].value("count", (int64_t)-1)}, {"after", after["data"].value("count", (int64_t)-1)}, {"ret", r["data"]["ret_i"]} });
    });
}

// SOW.ITEM_REMOVE {name, count}
json sow_cmd_item_remove(const json& a) {
    const int64_t n = a.value("count", (int64_t)1);
    if (n <= 0 || n > 10000000) return fail("BAD_ARGS", "count 1..10000000");
    return with_item(a, [n](uintptr_t, uintptr_t INV, uintptr_t rec) -> json {
        json before = item_count_on_main(INV, rec);
        json r = guarded_call(game_base() + kRemoveItem, { INV, rec, (uint64_t)n }, "u64");
        if (!r.value("ok", false)) return r;
        json after = item_count_on_main(INV, rec);
        return ok({ {"before", before["data"].value("count", (int64_t)-1)}, {"after", after["data"].value("count", (int64_t)-1)} });
    });
}

// SOW.ITEM_SET {name, value} -> add or remove the difference (keeps the game's own bookkeeping)
json sow_cmd_item_set(const json& a) {
    const int64_t target = a.value("value", (int64_t)-1);
    if (target < 0 || target > 100000000) return fail("BAD_ARGS", "value 0..100000000");
    const bool each = a.value("each", false);
    return with_item(a, [target, each](uintptr_t, uintptr_t INV, uintptr_t rec) -> json {
        json c = item_count_on_main(INV, rec); if (!c.value("ok", false)) return c;
        const int64_t cur = c["data"]["count"].get<int64_t>();
        if (cur == target) return ok({ {"before", cur}, {"after", cur} });
        json r;
        if (target > cur && each) { for (int64_t i = cur; i < target && i < cur + 1000; ++i) { r = guarded_call(game_base() + kAddItem, { INV, rec, 1 }, "u64", true); if (!r.value("ok", false)) return r; } }
        else if (target > cur) r = guarded_call(game_base() + kAddItem, { INV, rec, (uint64_t)(target - cur) }, "u64", true);
        else if (each) { for (int64_t i = target; i < cur; ++i) { r = guarded_call(game_base() + kRemoveItem, { INV, rec, 1 }, "u64", true); if (!r.value("ok", false)) return r; } }
        else r = guarded_call(game_base() + kRemoveItem, { INV, rec, (uint64_t)(cur - target) }, "u64", true);
        if (!r.value("ok", false)) return r;
        json after = item_count_on_main(INV, rec);
        return ok({ {"before", cur}, {"after", after["data"].value("count", (int64_t)-1)} });
    });
}

// SOW.ITEMS {table?, filter, max} -> record names (read-only)
json sow_cmd_items(const json& a) {
    const uintptr_t tbl = a.contains("table") ? (uintptr_t)a["table"].get<uint64_t>() : kTblItems;
    std::string f = a.value("filter", std::string()); std::transform(f.begin(), f.end(), f.begin(), ::tolower);
    const size_t mx = a.value("max", 200);
    const uintptr_t desc = rq(game_base() + tbl);
    if (!desc) return fail("NO_TABLE", "table not loaded");
    uint32_t n = 0; safe_read(desc + 0x28, &n, 4); const uintptr_t ents = rq(desc + 0x38);
    json out = json::array(); char buf[128];
    for (uint32_t i = 0; i < n && out.size() < mx; ++i) {
        const uintptr_t np = rq(ents + (uintptr_t)i * 0x28 + 0x20);
        if (!np || !safe_read(np, buf, sizeof buf)) continue;
        buf[sizeof buf - 1] = 0; std::string s = buf, lo = s; std::transform(lo.begin(), lo.end(), lo.begin(), ::tolower);
        if (f.empty() || lo.find(f) != std::string::npos) out.push_back(s);
    }
    return ok({ {"total", n}, {"names", out} });
}

// SOW.LEVEL {set: 1..cap} -> player level through the game's own setter exe+0x19D2F98(levelObj, level0, 1)
// (-> SetXPLevelInternal 0x35159C: clamps to the level table, fires the level-up event, pays the level-up reward).
// VERIFIED 2026-11-11: 8 -> 9, HUD "Lvl 9", skill points 9 -> 10. Cap = exe+0x351854([levelObj+0x58]) = 80.
json sow_cmd_level(const json& a) {
    const bool set = a.contains("set");
    const int want = set ? a["set"].get<int>() : 0;
    return run_on_main([=]() -> json {
        uintptr_t P, INV; std::string err;
        if (!player_inv(P, INV, err)) return fail("NO_PLAYER", err);
        const uintptr_t lv = rq(P + 0x26A8);
        if (!lv) return fail("NO_LEVEL", "player level object missing");
        int32_t cur0 = 0; safe_read(lv + 0x568, &cur0, 4);
        json capr = guarded_call(game_base() + 0x351854, { rq(lv + 0x58) }, "u64", true);
        const int cap = capr.value("ok", false) ? (int)capr["data"]["ret_i"].get<int64_t>() : 0;
        if (!set) return ok({ {"level", cur0 + 1}, {"cap", cap} });
        if (want < 1 || (cap && want > cap)) return fail("BAD_ARGS", "level 1.." + std::to_string(cap));
        json r = guarded_call(game_base() + 0x19D2F98, { lv, (uint64_t)(want - 1), 1 }, "u64");
        if (!r.value("ok", false)) return r;
        int32_t now0 = 0; safe_read(lv + 0x568, &now0, 4);
        return ok({ {"before", cur0 + 1}, {"level", now0 + 1}, {"cap", cap} });
    }, 5000);
}

// SOW.OWNED {filter, min (default 1), max} -> every Inventory/Items record whose count >= min, through the game's own
// GetItemCount (one native call per record, all on the game thread in one go). Read-only.
json sow_cmd_owned(const json& a) {
    std::string f = a.value("filter", std::string()); std::transform(f.begin(), f.end(), f.begin(), ::tolower);
    const int64_t mn = a.value("min", (int64_t)1); const size_t mx = a.value("max", 2000);
    return run_on_main([=]() -> json {
        uintptr_t P, INV; std::string err;
        if (!player_inv(P, INV, err)) return fail("NO_PLAYER", err);
        const uintptr_t desc = rq(game_base() + kTblItems); uint32_t n = 0; safe_read(desc + 0x28, &n, 4);
        const uintptr_t ents = rq(desc + 0x38);
        json out = json::object(); size_t checked = 0; char buf[128];
        for (uint32_t i = 0; i < n && out.size() < mx; ++i) {
            const uintptr_t e = ents + (uintptr_t)i * 0x28, np = rq(e + 0x20);
            if (!np || !safe_read(np, buf, sizeof buf)) continue;
            buf[sizeof buf - 1] = 0; std::string s = buf, lo = s; std::transform(lo.begin(), lo.end(), lo.begin(), ::tolower);
            if (!f.empty() && lo.find(f) == std::string::npos) continue;
            ++checked;
            json r = guarded_call(game_base() + kGetCount, { INV, e }, "u64", true);
            if (!r.value("ok", false)) return fail("CALL_FAULT", "GetItemCount faulted at " + s);
            const int64_t c = (int32_t)r["data"]["ret_i"].get<int64_t>();
            if (c >= mn) out[s] = c;
        }
        return ok({ {"checked", checked}, {"owned", out} });
    }, a.value("timeout_ms", 10000));
}

// ------------------------------------------------------------------ skills
// An upgrade is "active" while the player OWNS the items it grants; there is no other state. The game's switcher
// AcquireAbility 0x1BC8C94 revokes the siblings' items unless the multiSkill patch (sig multiSkill 74->EB) is on.
//   ActivateSkill(inv, skillRec) exe+0x1BC8C6C   Skill -> key item getter exe+0x18A2278(skillRec)
namespace {
constexpr uintptr_t kActivateSkill = 0x1BC8C6C, kSkillItem = 0x18A2278;
constexpr uint32_t kFldUnlockItem = 0xF7C685C7, kFldSkillItem = 0x052A842C;   // Inventory/Skill/Skills fields
// field of a DB record by name hash: entry +8 field map (16 B: hash, .., +12 count, +14 slot), +0x10 data, +0x18 owner
static bool skill_field(uintptr_t rec, uint32_t hash, uintptr_t& addr, int& cnt) {
    const uintptr_t own = rq(rec + 0x18), t = own ? rq(own + 0x20) : 0, m = rq(rec + 8), data = rq(rec + 0x10);
    if (!t || !m || !data) return false;
    uint32_t nf = 0; safe_read(t + 8, &nf, 4); if (nf == 0 || nf > 400) return false;
    for (uint32_t i = 0; i < nf; ++i) {
        uint8_t b[16]; if (!safe_read(m + i * 16, b, 16)) return false;
        uint32_t h; memcpy(&h, b, 4); if (h != hash) continue;
        uint16_t c, sl; memcpy(&c, b + 12, 2); memcpy(&sl, b + 14, 2);
        addr = data + (uintptr_t)sl * 8; cnt = c; return true;
    }
    return false;
}
bool multiskill_on() { const uintptr_t a = sig_addr("multiSkill"); uint8_t b = 0; return a && safe_read(a, &b, 1) && b == 0xEB; }
std::string rec_name(uintptr_t rec) { char b[128] = {}; const uintptr_t np = rq(rec + 0x20); if (!np || !safe_read(np, b, sizeof b)) return "?"; b[127] = 0; return b; }
// one skill on the game thread: optional buy (own the key item), then the game's ActivateSkill
json activate_one(uintptr_t INV, uintptr_t srec, bool buy) {
    const uintptr_t gb = game_base();
    json k = guarded_call(gb + kSkillItem, { srec }, "u64", true);
    if (!k.value("ok", false)) return k;
    const uintptr_t key = (uintptr_t)k["data"]["ret_i"].get<int64_t>();
    if (!key) return fail("NO_KEY", "skill has no item");
    json c = item_count_on_main(INV, key);
    const int64_t owned = c.value("ok", false) ? c["data"]["count"].get<int64_t>() : 0;
    bool bought = false;
    if (owned < 1) {
        if (!buy) return ok({ {"key", rec_name(key)}, {"skipped", "not bought"} });
        json r = guarded_call(gb + kAddItem, { INV, key, 1 }, "u64", true);
        if (!r.value("ok", false)) return r;
        bought = true;
    }
    json a = guarded_call(gb + kActivateSkill, { INV, srec }, "u64", true);
    if (!a.value("ok", false)) return a;
    return ok({ {"key", rec_name(key)}, {"bought", bought}, {"activated", a["data"]["ret_i"].get<int64_t>() != 0} });
}
bool talion_skill(const std::string& n, bool story, bool cut, bool prestige) {
    if (n.rfind("PSS_", 0) != 0) return false;                                    // DLC3_/DLC4_ = Eltariel / Baranor trees
    if (n.find("_OLD") != std::string::npos || n.find("zOld") != std::string::npos) return false;
    if (n.find("_DLC3") != std::string::npos || n.find("_DLC4") != std::string::npos) return false;
    const bool is_cut = n.find("Possess") != std::string::npos || n.find("RaisePets") != std::string::npos || n.find("RaiseRevanants") != std::string::npos || n.find("RaiseRevenants") != std::string::npos;
    if (is_cut && !cut) return false;                                              // no skill-menu node: cut content
    if (n.find("Prestige") != std::string::npos && !prestige) return false;
    (void)story;
    return true;
}
} // namespace

// SOW.SKILL_ACTIVATE {name: PSS_..., buy: false} -> the game's ActivateSkill (needs the multiSkill patch unless force)
json sow_cmd_skill_activate(const json& a) {
    const std::string name = a.value("name", std::string());
    if (name.empty()) return fail("BAD_ARGS", "name is required (a PSS_* skill record)");
    if (!multiskill_on() && !a.value("force", false)) return fail("NEED_MULTISKILL", "turn on 'Choose Multiple Skills' first (otherwise activating strips the other upgrades)");
    const bool buy = a.value("buy", false);
    return run_on_main([=]() -> json {
        uintptr_t P, INV; std::string err;
        if (!player_inv(P, INV, err)) return fail("NO_PLAYER", err);
        const uintptr_t srec = find_record(kTblSkills, name, err);
        if (!srec) return fail("NO_RECORD", err);
        return activate_one(INV, srec, buy);
    }, 5000);
}

// SOW.SKILLS_ALL {buy_missing, cut, prestige, filter, dry} -> every Talion upgrade active at once.
// "Active" = owning the ability item, so for each owned PS_<X>_UNLOCK (the bought skill /
// upgrade) the ability item PS_<X>_SKILL (or _NEWSKILL / _Skill, whichever the game has) is given if missing.
// buy_missing also gives every missing Talion PS_<X>_UNLOCK first. 2026-11-12: the game's ActivateSkill bought
// PS_Execution_Child2_UNLOCK but granted no ability item; the direct AddItem of PS_Execution_Child2_SKILL worked
// and kept Child1. Needs the multiSkill patch so a later in-game activation does not strip them again.
json sow_cmd_skills_all(const json& a) {
    if (!multiskill_on() && !a.value("dry", false)) return fail("NEED_MULTISKILL", "turn on 'Choose Multiple Skills' first");
    const bool buy = a.value("buy_missing", false), cut = a.value("cut", false), prestige = a.value("prestige", false), dry = a.value("dry", false);
    std::string f = a.value("filter", std::string());
    return run_on_main([=]() -> json {
        uintptr_t P, INV; std::string err;
        if (!player_inv(P, INV, err)) return fail("NO_PLAYER", err);
        const uintptr_t gb = game_base(), desc = rq(gb + kTblItems); uint32_t n = 0; safe_read(desc + 0x28, &n, 4);
        const uintptr_t ents = rq(desc + 0x38);
        std::map<std::string, uintptr_t> recs, all_items;                         // every PS_ item record by name (+ story grants)
        for (uint32_t i = 0; i < n; ++i) { const uintptr_t e = ents + (uintptr_t)i * 0x28; std::string nm = rec_name(e); if (nm.rfind("PS_", 0) == 0) recs[nm] = e; else if (nm.rfind("ScriptedUnlock_SkillGrant_", 0) == 0) all_items[nm] = e; }
        // current skills = records in Inventory/Skill/Skills (legacy ones carry _OLD; DEFAULT items are base meters)
        std::set<std::string> current;
        { const uintptr_t sd = rq(gb + kTblSkills); uint32_t sn = 0; safe_read(sd + 0x28, &sn, 4); const uintptr_t se = rq(sd + 0x38);
          for (uint32_t i = 0; i < sn; ++i) { std::string s = rec_name(se + (uintptr_t)i * 0x28); if (s.find("_OLD") == std::string::npos) current.insert(s); } }
        auto count = [&](uintptr_t rec) -> int64_t { json c = item_count_on_main(INV, rec); return c.value("ok", false) ? c["data"]["count"].get<int64_t>() : -1; };
        json granted = json::array(), bought = json::array(), already = json::array(), refused = json::array(), no_ability = json::array(), skipped = json::object();
        for (auto& [nm, rec] : recs) {
            if (nm.size() < 8 || nm.compare(nm.size() - 7, 7, "_UNLOCK") != 0) continue;
            const std::string base = nm.substr(0, nm.size() - 7);                // PS_<X>
            if (!talion_skill("PSS_" + base.substr(3), true, cut, prestige)) { if (dry && count(rec) < 1) skipped[nm] = "filtered (cut/prestige/DLC/old)"; continue; }
            if (!f.empty() && nm.find(f) == std::string::npos) continue;
            uintptr_t ability = 0; std::string an;
            for (const char* suf : { "_SKILL", "_NEWSKILL", "_Skill" }) { auto it = recs.find(base + suf); if (it != recs.end()) { ability = it->second; an = it->first; break; } }
            if (!ability) {
                // main skills with no ability item (Brand = Dominate, Detonate, Impact, Pin, RhythmMovement_Jump): the
                // story unlocks them, so a new game has none - buy them through the game's own ActivateSkill
                if (buy && count(rec) < 1 && current.count("PSS_" + base.substr(3)) && base.find("DEFAULT") == std::string::npos) {
                    std::string e2; const uintptr_t srec = find_record(kTblSkills, "PSS_" + base.substr(3), e2);
                    if (srec && !dry) {
                        json r = activate_one(INV, srec, true);
                        if (!r.value("ok", false) || count(rec) < 1) {           // no key item from the getter: own the unlock directly
                            json r2 = guarded_call(gb + kAddItem, { INV, rec, 1 }, "u64", true); if (!r2.value("ok", false)) return r2;
                            guarded_call(gb + kActivateSkill, { INV, srec }, "u64", true);
                        }
                    }
                    if (srec) { (count(rec) >= 1 || dry ? bought : refused).push_back(nm); continue; }
                }
                no_ability.push_back(nm); continue;
            }
            if (count(rec) < 1) {
                if (!buy) continue;
                // only buy what the skill menu offers today: a current PSS_ record with this exact name, no DEFAULT meters
                if (!current.count("PSS_" + base.substr(3)) || base.find("DEFAULT") != std::string::npos) { if (dry) skipped[nm] = "no PSS_" + base.substr(3) + " skill record"; continue; }
                // a renamed version of the same menu slot is owned (Impact_Child2 vs Impact_Child2_AmmoGain): skip the old one
                bool newer_owned = false;
                for (auto it2 = recs.lower_bound(base + "_"); it2 != recs.end() && it2->first.rfind(base + "_", 0) == 0; ++it2)
                    if (it2->first != nm && it2->first.size() > 7 && it2->first.compare(it2->first.size() - 7, 7, "_UNLOCK") == 0 && count(it2->second) >= 1) { newer_owned = true; break; }
                if (newer_owned) { if (dry) skipped[nm] = "a renamed version is owned"; continue; }
                if (!dry) { json r = guarded_call(gb + kAddItem, { INV, rec, 1 }, "u64", true); if (!r.value("ok", false)) return r; }
                bought.push_back(nm);
            }
            if (count(ability) >= 1) { already.push_back(an); continue; }
            // only UPGRADES get their ability item added: a parent cannot be swapped out, and an owned parent without its
            // _SKILL item (RhythmMovement) is granted through other items by the game - leave those alone
            { const bool is_child = base.find("_Child") != std::string::npos || std::regex_search(base, std::regex("_C[0-9]"));
              if (!is_child) continue; }
            if (dry) { granted.push_back(an + " (dry)"); continue; }
            json r = guarded_call(gb + kAddItem, { INV, ability, 1 }, "u64", true);
            if (!r.value("ok", false)) return r;
            if (count(ability) >= 1) granted.push_back(an); else refused.push_back(an);
        }
        // second pass, by the skill menu itself: every current Talion PSS_ skill record -> the game's own key item
        // (exe+0x18A2278). Names do not match the item names (Shadow Strider = PSS_RhythmMovement_Jump..., its upgrades,
        // Glaive, Fire Pits, Shadow Strike Kill ...), so the first pass misses those; the game's mapping does not.
        json by_skill = json::array();
        if (buy) {
            const uintptr_t sd = rq(gb + kTblSkills); uint32_t sn = 0; safe_read(sd + 0x28, &sn, 4); const uintptr_t se = rq(sd + 0x38);
            for (uint32_t i = 0; i < sn && se; ++i) {
                const uintptr_t srec = se + (uintptr_t)i * 0x28; const std::string sname = rec_name(srec);
                if (sname.find("_OLD") != std::string::npos || sname.find("DEFAULT") != std::string::npos) continue;
                if (!talion_skill(sname, true, cut, prestige)) continue;
                if (!f.empty() && sname.find(f) == std::string::npos) continue;
                json k = guarded_call(gb + kSkillItem, { srec }, "u64", true); if (!k.value("ok", false)) continue;
                const uintptr_t key = (uintptr_t)k["data"]["ret_i"].get<int64_t>(); if (!key) continue;
                const std::string kn = rec_name(key);
                if (kn.find("DLC") != std::string::npos || kn.find("Prestige") != std::string::npos || kn.find("DEFAULT") != std::string::npos) continue;
                if (count(key) >= 1) continue;
                if (dry) { by_skill.push_back(sname + " (" + kn + ")"); continue; }
                json r = activate_one(INV, srec, true); if (!r.value("ok", false)) continue;
                // its ability item too, when the game has one (PS_<X>_SKILL next to PS_<X>_UNLOCK)
                if (kn.size() > 7 && kn.compare(kn.size() - 7, 7, "_UNLOCK") == 0) {
                    const std::string kb = kn.substr(0, kn.size() - 7);
                    for (const char* suf : { "_SKILL", "_NEWSKILL", "_Skill" }) { auto it = recs.find(kb + suf); if (it != recs.end()) { if (count(it->second) < 1) guarded_call(gb + kAddItem, { INV, it->second, 1 }, "u64", true); break; } }
                }
                if (count(key) >= 1) by_skill.push_back(sname); else refused.push_back(sname);
            }
        }
        // third pass, from the skill records' own lists (as the recovered SkillFreedom mod did): for every skill whose
        // UnlockItem(s) are owned, own every SkillItem too. A skill can need several (PSS_ShadowStrike_Kill_Parent:
        // _SKILL, _DUMMY, PS_ShadowBrand_SKILL_SS_Kill_toBrand, PS_ShadowStrike_KD_SKILL = the TAP knockdown); only one
        // was given before. Legacy Mordor records (PSS_Taunt, PSS_Agility, PSS_UnlimitedEnergy ...) have unowned
        // unlocks and are skipped by that rule. Prestige ranks and Dark Talion items stay out.
        json skill_items = json::array(); size_t menu_count = 0;
        if (buy) {
            const uintptr_t sd = rq(gb + kTblSkills); uint32_t sn = 0; safe_read(sd + 0x28, &sn, 4); const uintptr_t se = rq(sd + 0x38);
            // only skills that have a node in the skill menu (Interface menu-node table exe+0x2701038 -> SkillData refs):
            // legacy Mordor records (PSS_Taunt, PSS_UnlimitedEnergy ...) share unlock items with current skills, so
            // "unlock owned" alone handed out their debug / old items (2026-11-13, removed again)
            std::set<uintptr_t> in_menu;
            { const uintptr_t md = rq(gb + 0x2701038); uint32_t mn = 0; safe_read(md + 0x28, &mn, 4); const uintptr_t me = rq(md + 0x38);
              for (uint32_t i = 0; i < mn && me; ++i) { const uintptr_t d = rq(me + (uintptr_t)i * 0x28 + 0x10);
                for (int k = 0; k < 64 && d; ++k) { const uintptr_t v = rq(d + 8 * k); if (v >= se && v < se + (uintptr_t)sn * 0x28 && (v - se) % 0x28 == 0) in_menu.insert(v); } } }
            for (uint32_t i = 0; i < sn && se; ++i) {
                const uintptr_t srec = se + (uintptr_t)i * 0x28; const std::string sname = rec_name(srec);
                if (!in_menu.count(srec)) continue;
                ++menu_count;
                if (sname.find("_OLD") != std::string::npos || !talion_skill(sname, true, cut, prestige)) continue;
                uintptr_t ua, sa; int uc, sc;
                if (!skill_field(srec, kFldUnlockItem, ua, uc) || !skill_field(srec, kFldSkillItem, sa, sc) || uc < 1 || uc > 8 || sc < 1 || sc > 16) continue;
                bool owned = true;
                for (int k = 0; k < uc && owned; ++k) { const uintptr_t u = rq(ua + 8 * k); if (u && count(u) < 1) owned = false; }
                if (!owned) continue;
                for (int k = 0; k < sc; ++k) {
                    const uintptr_t it = rq(sa + 8 * k); if (!it) continue;
                    const std::string in = rec_name(it);
                    if (in.find("Prestige") != std::string::npos || in.find("DarkTalion") != std::string::npos) continue;
                    if (count(it) >= 1) continue;
                    if (!dry) guarded_call(gb + kAddItem, { INV, it, 1 }, "u64", true);
                    skill_items.push_back(in);
                }
            }
        }
        // the story's move grants (ScriptedUnlock_SkillGrant_*): the moves check these items, so owning the skill-tree
        // items alone left Executions / Shadow Strike tap dead in a new game. Story menus (Hierarchy) and the pre-Bruz
        // ambush set stay with the story.
        if (buy) for (auto& [gn, grec] : all_items) {
            if (gn.rfind("ScriptedUnlock_SkillGrant_", 0) != 0 || gn.find("HierarchyMenu") != std::string::npos || gn.find("PreBruzAmbush") != std::string::npos) continue;
            if (count(grec) >= 1) continue;
            if (!dry) guarded_call(gb + kAddItem, { INV, grec, 1 }, "u64", true);
            bought.push_back(gn);
        }
        // the base meters the story hands out (a new game has none): Might (Executions, Might attacks) and Elven Rage
        if (buy) for (const char* m : { "PS_PowerMeter_DEFAULT_UNLOCK", "PS_PowerMeter_DEFAULT_SKILL", "PS_CBMeter_DEFAULT_UNLOCK", "PS_CBMeter_DEFAULT_SKILL" }) {
            auto it = recs.find(m); if (it == recs.end() || count(it->second) >= 1) continue;
            if (!dry) guarded_call(gb + kAddItem, { INV, it->second, 1 }, "u64", true);
            bought.push_back(m);
        }
        return ok({ {"granted", granted}, {"bought", bought}, {"by_skill", by_skill}, {"skill_items", skill_items}, {"menu_skills", menu_count}, {"already_active", already.size()}, {"refused", refused}, {"no_ability_item", no_ability}, {"skipped", skipped} });
    }, a.value("timeout_ms", 20000));
}


// SOW.SKILL_MENU_UNLOCK {on} - the skill menu sells every skill: story-locked, below the level, missing prerequisites.
// The user's CT "Unlock All Skills" (Reverser 2024): three branches in CanPurchaseSkill (sig canPurchaseSkill =
// exe+0x1BC9830): +0xA4 je (story / setup lock) -> NOP 6, +0xEB jae (min level) -> jmp, +0x12F je (requires N of X)
// -> jmp. Buying in the menu then grants everything the game's own way. Restored on off / reload.
namespace { struct MenuPatch { uintptr_t a; std::vector<uint8_t> orig; }; std::vector<MenuPatch> g_menu;
void patch_bytes(uintptr_t a, const uint8_t* b, size_t n) { safe_write(a, b, n); FlushInstructionCache(GetCurrentProcess(), (void*)a, n); } }
void skill_menu_restore() { for (auto& p : g_menu) patch_bytes(p.a, p.orig.data(), p.orig.size()); g_menu.clear(); }
json skill_menu_main(const json& a);
json sow_cmd_skill_menu_unlock(const json& a) { return run_on_main([a]() -> json { return skill_menu_main(a); }, 5000); }
json skill_menu_main(const json& a) {
    const bool on = a.value("on", true);
    if (!on) { const size_t n = g_menu.size(); skill_menu_restore(); return ok({ {"on", false}, {"restored", n} }); }
    if (!g_menu.empty()) return ok({ {"on", true}, {"already", true} });
    const uintptr_t f = sig_addr("canPurchaseSkill"); if (!f) return fail("NO_SIG", "CanPurchaseSkill not found in this game version");
    struct P { uintptr_t off; std::vector<uint8_t> expect, put; };
    const P ps[] = { { 0xA4, { 0x0F, 0x84, 0x2A, 0x01, 0x00, 0x00 }, { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 } }, { 0xEB, { 0x73 }, { 0xEB } }, { 0x12F, { 0x74 }, { 0xEB } } };
    for (auto& p : ps) { std::vector<uint8_t> cur(p.expect.size()); if (!safe_read(f + p.off, cur.data(), cur.size()) || cur != p.expect) return fail("BYTES", "unexpected code in CanPurchaseSkill (game updated?)"); }
    for (auto& p : ps) { g_menu.push_back({ f + p.off, p.expect }); patch_bytes(f + p.off, p.put.data(), p.put.size()); }
    return ok({ {"on", true}, {"patched", g_menu.size()} });
}

// ================================ Wraith Chain (StealthChain) tweaks ================================
// The chain = CB_* SneakInstaKill attacks. Their Combat/Target records
// (table exe+0x2700930) CB_Duel_Instakill, Sneak_Instakill_CBHang, Sneak_Instakill_DualAttack, CB_Chain_InstaHoG
// have RequireOnCamera + UseCameraBasedLOSSTests (packed bools). Slow-motion (SlowMo table exe+0x26FFE08): the hold
// while choosing = CB_Chain_StopPlayer_01_SM (world /14, player /100, 50 s), kills CB_Stealthkill_01/02/Resist_01_SM,
// Dualkill_CB/Hold_Slomo, WraithDashChaining_Slomo; time scales are (min,max) range pointers -> pointed at the (1,1)
// range the game already uses. Camera fly-to (Camera/Transition exe+0x27003E8) Chain_*_TR Duration (0.4 s ...).
namespace {
struct ChainPatch { uintptr_t a; uint64_t orig; int bytes; };
std::vector<ChainPatch> g_chain[3];                        // 0 aim, 1 slow-mo, 2 fast
constexpr uintptr_t kTblTarget = 0x2700930, kTblSlowMo = 0x26FFE08, kTblCamTrans = 0x27003E8;
constexpr uint32_t kFRequireOnCamera = 0x35C64911, kFCameraLOS = 0x24B5BF6F, kFSimScale = 0xB8A4691E, kFPlayerScale = 0xB151BDDC,
                   kFCompanionScale = 0x1C9B2B55, kFDuration = 0xBF1F472C, kFWeightData = 0x129819D2,
                   kFWDistance = 0x35293B55, kFWAngle = 0xDFC8957B, kFWInputCone = 0xB95A5F30, kFWOnCamera = 0xEA6CF6E4, kFWCamFacing = 0x19B6507F,
                   kFWFurther = 0x1A0D2D7D, kFWPrevTarget = 0xF4E19A50, kFWScreenW = 0xA4F9A2E2, kFWScreenH = 0xE9713F2B, kFWMaxAngle = 0x23C15627,
                   kFWMaxAngleNI = 0x1F2405EA, kFWRequireLOS = 0xBD971A1B, kFWAllowInput = 0xE2412916, kFWDistRange = 0xC7CFA288, kFWDistRangeNI = 0x09C98471;
alignas(16) float g_chain_range[4] = { 0.f, 2500.f, 0.f, 0.f };   // (min, max) cm, replaces DistanceRange (0, 1000)
// slot of a field: entry +8 field map (16 B: hash, ... +12 count, +14 slot)
bool field_slot(uintptr_t rec, uint32_t hash, uint16_t& slot) {
    const uintptr_t own = rq(rec + 0x18), t = own ? rq(own + 0x20) : 0, m = rq(rec + 8);
    if (!t || !m) return false; uint32_t nf = 0; safe_read(t + 8, &nf, 4); if (!nf || nf > 400) return false;
    for (uint32_t i = 0; i < nf; ++i) { uint8_t b[16]; if (!safe_read(m + i * 16, b, 16)) return false; uint32_t h; memcpy(&h, b, 4);
        if (h == hash) { memcpy(&slot, b + 14, 2); return true; } }
    return false;
}
void chain_put(int g, uintptr_t a, uint64_t v, int bytes) {
    uint64_t o = 0; safe_read(a, &o, bytes);
    for (auto& p : g_chain[g]) if (p.a == a) { safe_write(a, &v, bytes); return; }
    g_chain[g].push_back({ a, o, bytes }); safe_write(a, &v, bytes);
}
void chain_restore(int g) { for (auto it = g_chain[g].rbegin(); it != g_chain[g].rend(); ++it) safe_write(it->a, &it->orig, it->bytes); g_chain[g].clear(); }
} // namespace
// SOW.CHAIN_TWEAK {aim_free, no_slowmo, fast}  (each optional: true = on, false = back to the game's values)
json sow_cmd_chain_tweak(const json& a) {
    return run_on_main([a]() -> json {
        json out = json::object(); std::string err;
        if (a.contains("aim_free")) {
            if (!a["aim_free"].get<bool>()) { chain_restore(0); out["aim_free"] = false; }
            else { int n = 0;
                for (const char* nm : { "CB_Duel_Instakill", "Sneak_Instakill_CBHang", "Sneak_Instakill_DualAttack", "CB_Chain_InstaHoG", "PC_Default", "InstantKO_Token" }) {
                    const uintptr_t r = find_record(kTblTarget, nm, err); if (!r) continue; const uintptr_t d = rq(r + 0x10);
                    for (uint32_t h : { kFRequireOnCamera, kFCameraLOS }) { uint16_t sl; if (!d || !field_slot(r, h, sl)) continue;
                        const uintptr_t w = d + (uintptr_t)(sl >> 5) * 8; uint32_t v = 0; safe_read(w, &v, 4); chain_put(0, w, v & ~(1u << (sl & 31)), 4); ++n; }
                    // the real picker: WeightData (CombatTargetWeight). Camera facing 1000 / on camera 100 / ahead of
                    // the player 1000 vs distance 15 -> camera wins. Now: distance only (closest first), no screen
                    // box, no LOS, no stick bias, 360 degrees, reach 25 m.
                    uint16_t ws; if (!d || !field_slot(r, kFWeightData, ws)) continue;
                    const uintptr_t wd = rq(d + (uintptr_t)ws * 8), wdd = wd ? rq(wd + 0x10) : 0; if (!wdd) continue;
                    auto setf = [&](uint32_t h, float val) { uint16_t q; if (!field_slot(wd, h, q)) return; uint32_t b; memcpy(&b, &val, 4); chain_put(0, wdd + (uintptr_t)q * 8, b, 4); ++n; };
                    auto clrb = [&](uint32_t h) { uint16_t q; if (!field_slot(wd, h, q)) return; const uintptr_t w2 = wdd + (uintptr_t)(q >> 5) * 8; uint32_t v = 0; safe_read(w2, &v, 4); chain_put(0, w2, v & ~(1u << (q & 31)), 4); ++n; };
                    // balanced (user 2026-11-13): distance-only felt shorter (a far orc you look at never won). Closest
                    // first by default, but camera facing and the stick still pull toward a far orc; 360 / 25 m / no
                    // screen box / no LOS kept.
                    setf(kFWDistance, 100.f); setf(kFWAngle, 10.f); setf(kFWInputCone, 50.f); setf(kFWOnCamera, 50.f); setf(kFWCamFacing, 300.f);
                    setf(kFWFurther, 0.f); setf(kFWPrevTarget, 0.f); setf(kFWScreenW, 100.f); setf(kFWScreenH, 100.f); setf(kFWMaxAngle, 360.f); setf(kFWMaxAngleNI, 360.f);
                    clrb(kFWRequireLOS);
                    // both reaches: with stick input off the game uses DistanceRangeNoInput (6 m for melee) -> it felt shorter
                    for (uint32_t h : { kFWDistRange, kFWDistRangeNI }) { uint16_t q; if (field_slot(wd, h, q)) { chain_put(0, wdd + (uintptr_t)q * 8, (uint64_t)(uintptr_t)g_chain_range, 8); ++n; } }
                }
                out["aim_free"] = n; }
        }
        if (a.contains("no_slowmo")) {
            if (!a["no_slowmo"].get<bool>()) { chain_restore(1); out["no_slowmo"] = false; }
            else { int n = 0;
                uintptr_t one = 0; { const uintptr_t r = find_record(kTblSlowMo, "WraithDashChaining_Slomo", err); uint16_t sl;
                    if (r && field_slot(r, kFCompanionScale, sl)) one = rq(rq(r + 0x10) + (uintptr_t)sl * 8); }
                float f2[2] = {}; if (!one || !safe_read(one, f2, 8) || f2[0] != 1.f || f2[1] != 1.f) return fail("NOT_FOUND", "normal-speed value not found (game updated?)");
                for (const char* nm : { "CB_Chain_StopPlayer_01_SM", "CB_Stealthkill_01_SM", "CB_Stealthkill_02_SM", "CB_Stealthkill_Resist_01_SM", "Dualkill_CB_Slomo", "Dualkill_Hold_Slomo", "WraithDashChaining_Slomo",
                                       "Instakill_01_SM", "Instakill_02_SM", "Instakill_03_SM", "Instakill_Silent_B_SM", "Instakill_StealthKill_F_Slomo_SM", "Instakill_StealthKill_B_Slomo_SM", "Stealthkill_01_SM", "Stealthkill_02_SM" }) {
                    const uintptr_t r = find_record(kTblSlowMo, nm, err); if (!r) continue; const uintptr_t d = rq(r + 0x10);
                    // "CB" = Celebrimbor: in his chain records Talion is frozen ON PURPOSE (player 1/100) while the spirit
                    // jumps target to target; unfreezing Talion broke the chain (user 2026-11-13). There only the world
                    // and the spirit (companion) go to normal speed.
                    const bool cb = strncmp(nm, "CB_", 3) == 0 || strncmp(nm, "Dualkill_CB", 11) == 0;
                    for (uint32_t h : { kFSimScale, kFPlayerScale, kFCompanionScale }) { if (cb && h == kFPlayerScale) continue;
                        uint16_t sl; if (!d || !field_slot(r, h, sl)) continue; chain_put(1, d + (uintptr_t)sl * 8, one, 8); ++n; } }
                out["no_slowmo"] = n; }
        }
        if (a.contains("fast")) {
            if (!a["fast"].get<bool>()) { chain_restore(2); out["fast"] = false; }
            else { int n = 0; chain_restore(2);
                for (const char* nm : { "Chain_FlyToInstakill_TR", "Chain_FlyToReturn_TR", "Chain_Hold_Long_TR", "Chain_FlyToReturn_FromHighPIS_TR", "Chain_FlyToHangReturn_TR" }) {
                    const uintptr_t r = find_record(kTblCamTrans, nm, err); if (!r) continue; const uintptr_t d = rq(r + 0x10); uint16_t sl;
                    if (!d || !field_slot(r, kFDuration, sl)) continue; float v = 0; safe_read(d + (uintptr_t)sl * 8, &v, 4);
                    if (!(v > 0.f && v < 30.f)) continue; float nv = v * 0.25f; uint32_t bits; memcpy(&bits, &nv, 4); chain_put(2, d + (uintptr_t)sl * 8, bits, 4); ++n; }
                out["fast"] = n; }
        }
        return ok(out);
    }, 8000);
}

// Target weights are copied by the game early (hardware breakpoints: never read during play), so "Chain Without
// Aiming" is also applied at game start, as soon as the record database answers - before the first world loads.
void chain_boot_tick() {
    static bool done = false, want = false, checked = false; static ULONGLONG last = 0;
    if (done) return;
    if (!checked) { checked = true;
        try { std::ifstream f(g_cfg.root + "\\panel_state.json"); json st = json::parse(f);
              want = st.contains("chain_aimfree") && st["chain_aimfree"].value("on", false); } catch (...) {}
        if (!want) { done = true; return; } }
    const ULONGLONG now = GetTickCount64(); if (now - last < 250) return; last = now;
    std::string err; if (!find_record(kTblTarget, "PC_Default", err)) return;
    done = true;
    json r = sow_cmd_chain_tweak(json{ {"aim_free", true} });
    log("[chain] boot apply of Chain Without Aiming: %s", r.dump().c_str());
}

// ================================ Health lock (damage hook) ================================
// The refill (SOW.HEALTH fill, every 100 ms) loses to one hit bigger than the bar. The game's damage write
// exe+0x2E4D26 (sig healthWrite, from the user's SeiKur0 CT "Unlimited Health"): comiss xmm6,xmm2 ;
// movss [rbx+0x42C],xmm2 (rbx = health component, xmm2 = health after the hit). Hooked: when rbx is Talion's component
// ([P+0x26A8], refreshed every tick) xmm2 = max ([rbx+0x428]) first. Everyone else takes damage normally.
namespace {
uint8_t* g_god_stub = nullptr; uintptr_t g_god_site = 0; uint8_t g_god_orig[11] = {}; bool g_god_tick = false;
uint8_t* god_alloc_near(uintptr_t at) {
    SYSTEM_INFO si; GetSystemInfo(&si); const uintptr_t gran = si.dwAllocationGranularity, base = at & ~(gran - 1);
    for (uintptr_t d = gran; d < 0x70000000; d += gran)
        for (int sgn = 0; sgn < 2; ++sgn) { const uintptr_t q = sgn ? base + d : base - d;
            if (void* m = VirtualAlloc((void*)q, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE)) return (uint8_t*)m; }
    return nullptr;
}
void god_tick() {
    if (!g_god_stub) return;
    uintptr_t root = 0, P = 0, H = 0; const uintptr_t gb = game_base();
    if (safe_read(gb + kPlayerRoot, &root, 8) && root && safe_read(root + 0x888, &P, 8) && P) safe_read(P + 0x26A8, &H, 8);
    memcpy(g_god_stub + 0x80, &H, 8);                      // slot read by the stub (0 = nobody)
}
}
void god_restore() {
    if (g_god_site && g_god_stub) { safe_write(g_god_site, g_god_orig, 11); FlushInstructionCache(GetCurrentProcess(), (void*)g_god_site, 11); }
    g_god_site = 0;   // the stub page stays (a thread may still be inside it)
}
// SOW.HEALTH_LOCK {on} - patched from the game thread (the damage code runs there, so no thread is mid-instruction)
json health_lock_main(const json& a);
json sow_cmd_health_lock(const json& a) { return run_on_main([a]() -> json { return health_lock_main(a); }, 5000); }
json health_lock_main(const json& a) {
    const bool on = a.value("on", true);
    if (!on) { const bool was = g_god_site != 0; god_restore(); return ok({ {"on", false}, {"was_on", was} }); }
    if (g_god_site) return ok({ {"on", true}, {"already", true} });
    const uintptr_t site = sig_addr("healthWrite"); if (!site) return fail("NO_SIG", "damage code not found in this game version");
    const uint8_t expect[11] = { 0x0F, 0x2F, 0xF2, 0xF3, 0x0F, 0x11, 0x93, 0x2C, 0x04, 0x00, 0x00 };
    uint8_t cur[11]; if (!safe_read(site, cur, 11) || memcmp(cur, expect, 11) != 0) return fail("BYTES", "unexpected code at the damage write (game updated?)");
    if (!g_god_stub) g_god_stub = god_alloc_near(site);
    if (!g_god_stub) return fail("ALLOC", "no memory near the game code");
    uint8_t* st = g_god_stub; size_t i = 0; const uintptr_t slot = (uintptr_t)(st + 0x80), back = site + 11;
    auto put = [&](std::initializer_list<uint8_t> b) { for (uint8_t x : b) st[i++] = x; };
    put({ 0x50 });                                                     // push rax
    put({ 0x48, 0xB8 }); memcpy(st + i, &slot, 8); i += 8;             // mov rax, &slot
    put({ 0x48, 0x8B, 0x00 });                                         // mov rax, [rax]
    put({ 0x48, 0x85, 0xC0 });                                         // test rax, rax
    put({ 0x74, 0x10 });                                               // je +16 -> second pop (nobody: plain path)
    put({ 0x48, 0x39, 0xC3 });                                         // cmp rbx, rax
    put({ 0x75, 0x0B });                                               // jne +11 -> second pop
    put({ 0x58 });                                                     // pop rax
    put({ 0xF3, 0x0F, 0x10, 0x93, 0x28, 0x04, 0x00, 0x00 });           // movss xmm2, [rbx+0x428]  (max)
    put({ 0xEB, 0x01 });                                               // jmp +1
    put({ 0x58 });                                                     // pop rax
    put({ 0x0F, 0x2F, 0xF2 });                                         // comiss xmm6, xmm2   (original)
    put({ 0xF3, 0x0F, 0x11, 0x93, 0x2C, 0x04, 0x00, 0x00 });           // movss [rbx+0x42C], xmm2 (original)
    put({ 0xFF, 0x25, 0, 0, 0, 0 }); memcpy(st + i, &back, 8); i += 8; // jmp back (flags from comiss kept)
    memset(st + 0x80, 0, 8);
    FlushInstructionCache(GetCurrentProcess(), st, 0x100);
    god_tick();
    memcpy(g_god_orig, cur, 11);
    const int64_t rel = (int64_t)(uintptr_t)st - (int64_t)(site + 5);
    if (rel > 0x7FFFFFF0LL || rel < -0x7FFFFFF0LL) return fail("FAR", "hook memory too far from the game code");
    uint8_t patch[11] = { 0xE9 }; int32_t r32 = (int32_t)rel; memcpy(patch + 1, &r32, 4); memset(patch + 5, 0x90, 6);
    if (!safe_write(site, patch, 11)) return fail("WRITE", "could not patch the damage write");
    FlushInstructionCache(GetCurrentProcess(), (void*)site, 11);
    g_god_site = site;
    if (!g_god_tick) { g_god_tick = true; add_main_tick(&god_tick); }
    return ok({ {"on", true}, {"site", hexstr(site)} });
}

// ================================ Elven Rage refills after use ================================
// Wrath (Elven Rage meter) = [[P+0x26A8]+0x340] +0x310, 0..100 (recorded 2026-11-13: 95 -> 1 while rage drained, then
// +1 per kill). "Infinite Elven Rage" (dev bool) keeps it full so rage never ends. This lets rage drain normally and
// refills the meter once it has stopped dropping for 1.5 s (rage over).
namespace { bool g_rr_on = false, g_rr_tick = false; float g_rr_last = -1.f; ULONGLONG g_rr_drop = 0, g_rr_next = 0, g_rr_pulse = 0; }
// the Wrath dev switch is only ours while a pulse runs: put it back if the option goes off / the DLL reloads mid-pulse
// review 2026-11-14: put back what the byte held before the pulse (the user's own Infinite Elven Rage toggle is the same byte)
static uint8_t g_rr_prev = 0;
static void rage_pulse_end() { if (!g_rr_pulse) return; const uintptr_t db = sig_addr("devBool"); if (db) safe_write(db + 3, &g_rr_prev, 1); g_rr_pulse = 0; }
static uintptr_t wrath_addr() {
    uintptr_t root = 0, P = 0, H = 0, W = 0; const uintptr_t gb = game_base();
    if (!safe_read(gb + kPlayerRoot, &root, 8) || !root || !safe_read(root + 0x888, &P, 8) || !P) return 0;
    if (!safe_read(P + 0x26A8, &H, 8) || !H || !safe_read(H + 0x340, &W, 8) || !W) return 0;
    return W + 0x310;
}
static void rage_refill_tick() {
    if (!g_rr_on) { rage_pulse_end(); return; }
    const ULONGLONG now = GetTickCount64(); if (now < g_rr_next) return; g_rr_next = now + 200;
    if (g_rr_pulse && now >= g_rr_pulse) { rage_pulse_end(); g_rr_drop = now; }     // before the world check: a death / load mid-pulse
    if (!sow_world_ready()) { g_rr_last = -1.f; return; }
    const uintptr_t a = wrath_addr(); float v = 0; if (!a || !safe_read(a, &v, 4) || !(v >= 0.f && v <= 200.f)) return;
    // Rage starts when the combat value Float_CBWraithMeter >= 100 (P0086_IsWraithMeterCharged), which a raw write of
    // the bar does not set. So the refill pulses the game's own dev switch Wrath (sig devBool +3) for 1 s: the game
    // fills and arms the meter itself, then the switch goes off again so the next rage drains normally.
    const uintptr_t db = sig_addr("devBool");
    if (g_rr_pulse) { if (now >= g_rr_pulse) { rage_pulse_end(); g_rr_drop = now; } g_rr_last = v; return; }
    if (g_rr_last >= 0.f && v < g_rr_last - 0.05f) g_rr_drop = now;                // draining = rage active
    if (v < 99.9f && now - g_rr_drop > 1500 && db) {
        uint8_t cur = 0; safe_read(db + 3, &cur, 1);
        if (!cur) { g_rr_prev = cur; const uint8_t one = 1; safe_write(db + 3, &one, 1); g_rr_pulse = now + 1000; }   // already on = Infinite rage: nothing to do
    }
    g_rr_last = v;
}
// SOW.RAGE_REFILL {on} -> {wrath}
// runs on the game thread (review 2026-11-14: the tick / the game read the same data)
static json sow_cmd_rage_refill_main(const json& a);
json sow_cmd_rage_refill(const json& a) { return run_on_main([a]() -> json { return sow_cmd_rage_refill_main(a); }, 3000); }
static json sow_cmd_rage_refill_main(const json& a) {
    g_rr_on = a.value("on", true); g_rr_last = -1.f; g_rr_drop = 0; if (!g_rr_on) rage_pulse_end();
    if (g_rr_on && !g_rr_tick) { g_rr_tick = true; add_main_tick(&rage_refill_tick); }
    float v = -1.f; const uintptr_t w = wrath_addr(); if (w) safe_read(w, &v, 4);
    return ok({ {"on", g_rr_on}, {"wrath", v} });
}

uintptr_t sow_skill_table() { return kTblSkills; }

// SOW.RAISE_DEAD {on, dark_talion=true, upgrades=true} - Raise Dead without finishing the story.
//  1. Skill menu: the Raise Dead node (SkillRow7_Node_Parent_Raise) sits in an orphan row that no menu group uses. Put it
//     into the Story row (SkillRow6_RingWraith BaseNodes) in place of the Ring Prestige node (that node is Act 3+ only)
//     and point its creation predicate at HasSkill_RaiseDeadUnlock instead of Is_Dark_Talion. Database edits in memory,
//     never saved; the menu rebuilds on every open. Verified in game 2026-11-13: "Raise Dead - press [T]" + 3 upgrades.
//  2. Skill items PS_Raise_* (game's own AddItem) when missing.
//  3. GP_PlayerIsDarkTalion x1 = the game's own post-Act-3 switch (buff PC_IsDarkTalion that every NecroAoE attack
//     needs). The game does not save it, so the panel re-asserts it after every load ("keep").
namespace {
constexpr uintptr_t kTblMenuTreeBase = 0x2701290, kTblMenuSkills = 0x2701038, kTblUiPlayerPred = 0x2700A30;
struct RdPatch { uintptr_t addr = 0, orig = 0; };
RdPatch g_rd_row, g_rd_pred;
std::vector<RdPatch> g_rd_aoe; bool g_rd_aoe_done = false;
bool wq(uintptr_t a, uintptr_t v) { return safe_write(a, &v, 8); }
constexpr uintptr_t kTblAOE = 0x27008F0, kTblCharType = 0x2700880;
constexpr uintptr_t kTblAttack = 0x27008F8, kTblPredBuffAtk = 0x2700F08, kTblPredItemAtk = 0x27001B0;
// Raise Dead WITHOUT the Dark Talion state (2026-11-13): GP_PlayerIsDarkTalion also swaps Talion's voice set
// (AudioHooks vx_DarkTalionMelee/MeleeStrong/Grunt, condition Source_IsPlayerDarkTalion = the growl on Might attacks),
// the g_DarkTalion FX and the DT game changer. Instead: in every Combat/Attack NecroAoE_Attack_* replace the predicate
// PredicateBuffAttackerIncluded:PC_IsDarkTalion (data slot 12..14) with PredicateInventoryItemAttackerIncluded:
// CSP_RaiseWights (owns PS_Raise_Parent_SKILL), which sits in the next slot anyway.
std::vector<RdPatch> g_rd_atk; bool g_rd_atk_done = false;
json rd_attack_gate(bool on) {
    json out = json::object(); std::string err;
    if (!on) { int n = 0; for (auto& p : g_rd_atk) if (wq(p.addr, p.orig)) ++n; g_rd_atk.clear(); g_rd_atk_done = false; out["restored"] = n; return out; }
    if (g_rd_atk_done) { out["patched"] = g_rd_atk.size(); return out; }
    const uintptr_t dt = find_record(kTblPredBuffAtk, "PC_IsDarkTalion", err), rw = find_record(kTblPredItemAtk, "CSP_RaiseWights", err);
    if (!dt || !rw) { out["error"] = err; return out; }
    const uintptr_t desc = rq(game_base() + kTblAttack); uint32_t n = 0; safe_read(desc + 0x28, &n, 4); const uintptr_t ents = rq(desc + 0x38);
    char nb[96]; int done = 0, already = 0;
    for (uint32_t i = 0; i < n && ents; ++i) {
        const uintptr_t e = ents + (uintptr_t)i * 0x28, np = rq(e + 0x20);
        if (!np || !safe_read(np, nb, sizeof nb)) continue; nb[sizeof nb - 1] = 0;
        if (strncmp(nb, "NecroAoE_Attack_", 16) != 0) continue;
        const uintptr_t d = rq(e + 0x10);
        for (int k = 0; k < 20 && d; ++k) {
            const uintptr_t v = rq(d + 8 * k);
            if (v == dt && rq(d + 8 * (k + 1)) == rw) { if (wq(d + 8 * k, rw)) { g_rd_atk.push_back({ d + 8 * k, dt }); ++done; } break; }
            if (v == rw && rq(d + 8 * (k + 1)) == rw) { ++already; break; }
        }
    }
    g_rd_atk_done = true; out["attacks"] = done; out["already"] = already;
    return out;
}
// Celebrimbor's Elven Rage with Dark Talion on (user 2026-11-13: rage played plain while Raise the Dead was on).
// Starting rage = Combat/Attack PC_GC_Initiate (Celebrimbor) with predicate P0053_PCIsDarkTalion (excluded when DT) vs
// PC_GC_2_Initiate (Dark Talion's rage) with PredicateBuffAttackerIncluded:PC_IsDarkTalion. Swap: in PC_GC_Initiate the
// P0053 slot -> P0086_IsWraithMeterCharged (already in the same list); in PC_GC_2_Initiate the PC_IsDarkTalion slot ->
// PLBoG_NazgulTwin_ChainAnchor_Active (a DLC buff Talion never has), so only Celebrimbor's rage starts.
std::vector<RdPatch> g_rd_rage;
json rd_rage_fix(bool on) {
    json out = json::object(); std::string err;
    if (!on) { int n = 0; for (auto& p : g_rd_rage) if (wq(p.addr, p.orig)) ++n; g_rd_rage.clear(); out["restored"] = n; return out; }
    if (!g_rd_rage.empty()) { out["patched"] = g_rd_rage.size(); return out; }
    const uintptr_t gc1 = find_record(kTblAttack, "PC_GC_Initiate", err), gc2 = find_record(kTblAttack, "PC_GC_2_Initiate", err);
    const uintptr_t dt = find_record(kTblPredBuffAtk, "PC_IsDarkTalion", err), never = find_record(kTblPredBuffAtk, "PLBoG_NazgulTwin_ChainAnchor_Active", err);
    if (!gc1 || !gc2 || !dt || !never) { out["error"] = err; return out; }
    { const uintptr_t d = rq(gc1 + 0x10); uintptr_t p53 = 0, p86 = 0;
      for (int k = 0; k < 40 && d; ++k) { const uintptr_t v = rq(d + 8 * k); if (v < 0x10000) continue; const std::string n = rec_name(v);
          if (n == "P0053_PCIsDarkTalion" && !p53) p53 = d + 8 * k; else if (n == "P0086_IsWraithMeterCharged" && !p86) p86 = v; }
      if (p53 && p86) { g_rd_rage.push_back({ p53, rq(p53) }); wq(p53, p86); out["celebrimbor_rage"] = "Dark Talion check removed"; } else out["celebrimbor_rage"] = "slot not found"; }
    { const uintptr_t d = rq(gc2 + 0x10); uintptr_t slot = 0;
      for (int k = 0; k < 40 && d; ++k) if (rq(d + 8 * k) == dt) { slot = d + 8 * k; break; }
      if (slot) { g_rd_rage.push_back({ slot, dt }); wq(slot, never); out["dark_talion_rage"] = "never starts"; } else out["dark_talion_rage"] = "slot not found"; }
    return out;
}
// Undead Gondorian bodies (user-verified 2026-11-13: soldiers with bodies that fight). Dark Talion's rage and the
// "Gondorian Wights" gear effect summon Gondorian_UndeadSoldier, whose body is the NPC bundle Undead_Gondorians -
// only in story lists (GP_Blade4/5, GP_Finale/30), so outside them only the swords appear and the AI never starts.
// The character bundle manager [exe+0x26FFAB0]+0x868 has the area list and an override slot +0xA0 (+0xB0 = rebuild),
// exactly what System:LoadOverrideNPCBundleList sets (exe+0xE9F330). Missions use that slot for their own characters,
// so it is only taken when empty, and only cleared when it still holds ours.
constexpr uintptr_t kTblNPCBundleLists = 0x26FFE00, kCharBundleMgr = 0x26FFAB0;
json rd_undead_bodies(bool on) {
    json out = json::object(); std::string err;
    const uintptr_t mgr = rq(game_base() + kCharBundleMgr); if (!mgr) { out["state"] = "no bundle manager yet"; return out; }
    const uintptr_t m = mgr + 0x868, cur = rq(m + 0xA0);
    const uintptr_t ours = find_record(kTblNPCBundleLists, "GP_Blade5", err); if (!ours) { out["error"] = err; return out; }
    const uint8_t one = 1;
    if (on) {
        if (cur == ours) { out["state"] = "loaded"; return out; }
        if (cur) { out["state"] = "story list active (" + rec_name(cur) + "), left alone"; return out; }
        wq(m + 0xA0, ours); safe_write(m + 0xB0, &one, 1); out["state"] = "requested";
    } else if (cur == ours) { wq(m + 0xA0, 0); safe_write(m + 0xB0, &one, 1); out["state"] = "released"; }
    return out;
}
// "All upgrades together": the attack's AOEPicker plays only the FIRST owned upgrade (Undying Beast > Death Troll >
// Undying Loyalty). Each PC_NecroAoE_RaiseMonsters_* AOE has ticks [beasts, goblins, grunts]. In every one of them:
// goblin tick -> Undying Loyalty's captain tick (named orcs + Ologs, up to 7, MakePlayerFollower), and the grunt tick's
// IncludedCharacterType All_Orcs -> All_Orcs_OlogHai (Death Troll). Tick slot 36 = IncludedCharacterType (live data).
json rd_combine(bool on) {
    json out = json::object(); std::string err;
    if (!on) { int n = 0; for (auto& p : g_rd_aoe) if (wq(p.addr, p.orig)) ++n; g_rd_aoe.clear(); g_rd_aoe_done = false; out["restored"] = n; return out; }
    if (g_rd_aoe_done) { out["patched"] = g_rd_aoe.size(); return out; }
    const uintptr_t orcs = find_record(kTblCharType, "All_Orcs", err), orcsOlog = find_record(kTblCharType, "All_Orcs_OlogHai", err);
    const uintptr_t gob = find_record(kTblCharType, "Goblins_ExceptRare", err), mons = find_record(kTblCharType, "All_Monsters", err);
    const uintptr_t named = find_record(kTblAOE, "PC_NecroAoE_Named", err);
    if (!orcs || !orcsOlog || !gob || !mons || !named) { out["error"] = err; return out; }
    auto inc = [](uintptr_t tick) -> uintptr_t { const uintptr_t td = rq(tick + 0x10); return td ? rq(td + 36 * 8) : 0; };
    uintptr_t capTick = 0; { const uintptr_t nd = rq(named + 0x10); for (int k = 0; k < 16 && nd && !capTick; ++k) { const uintptr_t v = rq(nd + 8 * k); if (v > 0x10000 && inc(v) == orcsOlog) capTick = v; } }
    if (!capTick) { out["error"] = "captain tick not found"; return out; }
    const uintptr_t desc = rq(game_base() + kTblAOE); uint32_t n = 0; safe_read(desc + 0x28, &n, 4); const uintptr_t ents = rq(desc + 0x38);
    int aoes = 0; std::set<uintptr_t> gruntTicks; char nb[96];
    for (uint32_t i = 0; i < n && ents; ++i) {
        const uintptr_t e = ents + (uintptr_t)i * 0x28, np = rq(e + 0x20);
        if (!np || !safe_read(np, nb, sizeof nb)) continue; nb[sizeof nb - 1] = 0;
        if (strncmp(nb, "PC_NecroAoE_RaiseMonsters", 25) != 0) continue;
        const uintptr_t d = rq(e + 0x10); bool hit = false;
        for (int k = 0; k < 16 && d; ++k) {
            const uintptr_t v = rq(d + 8 * k); if (v < 0x10000) continue;
            const uintptr_t ic = inc(v);
            if (ic == gob) { if (wq(d + 8 * k, capTick)) { g_rd_aoe.push_back({ d + 8 * k, v }); hit = true; } }
            else if (ic == orcs && rq(rq(v + 0x10) + 39 * 8) == mons) gruntTicks.insert(v);
        }
        if (hit) ++aoes;
    }
    for (uintptr_t t : gruntTicks) { const uintptr_t a = rq(t + 0x10) + 36 * 8; if (wq(a, orcsOlog)) g_rd_aoe.push_back({ a, orcs }); }
    g_rd_aoe_done = true;
    out["aoes"] = aoes; out["grunt_ticks"] = gruntTicks.size(); out["patched"] = g_rd_aoe.size();
    return out;
}

// "Raise everything" (2026-11-13): every Combat.AOETick of every PC_NecroAoE_* AOE whose Attack is
// Necro_AoE_RaiseGrunts_Tick: MaxAIToHit -> 30, MustBeAlly -> null (Undying Loyalty only raised YOUR fallen captains),
// ExcludedItemList -> null (sappers), and the beast tick (Excluded = All_Orcs) -> Included AI_All x n, Excluded
// All_Orcs_OlogHai (every creature: caragors, graugs, spiders, fell beasts, humans...). Slots differ per tick (arrays
// shift them), so fields are found through the tick's own field map (16-byte entries: +0 hash, +12 cnt, +14 slot).
std::vector<std::pair<uintptr_t, std::pair<uintptr_t, int>>> g_rd_all;   // addr -> (orig, bytes)
bool g_rd_all_done = false;
bool tick_field(uintptr_t tick, uint32_t hash, uintptr_t& addr, int& cnt) {
    const uintptr_t own = rq(tick + 0x18), t = own ? rq(own + 0x20) : 0, m = rq(tick + 8), data = rq(tick + 0x10);
    if (!t || !m || !data) return false;
    uint32_t nf = 0; safe_read(t + 8, &nf, 4); if (nf == 0 || nf > 400) return false;
    for (uint32_t i = 0; i < nf; ++i) {
        uint8_t b[16]; if (!safe_read(m + i * 16, b, 16)) return false;
        uint32_t h; memcpy(&h, b, 4); if (h != hash) continue;
        uint16_t c, sl; memcpy(&c, b + 12, 2); memcpy(&sl, b + 14, 2);
        addr = data + (uintptr_t)sl * 8; cnt = c; return true;
    }
    return false;
}
void rd_put(uintptr_t a, uintptr_t v, int bytes) {
    uintptr_t o = 0; if (!safe_read(a, &o, bytes)) return;
    if (bytes == 4) { o &= 0xFFFFFFFFull; uint32_t v4 = (uint32_t)v; if (!safe_write(a, &v4, 4)) return; }
    else if (!safe_write(a, &v, 8)) return;
    g_rd_all.push_back({ a, { o, bytes } });
}
json rd_everything(bool on) {
    json out = json::object(); std::string err;
    if (!on) { int n = 0; for (auto it = g_rd_all.rbegin(); it != g_rd_all.rend(); ++it) if (safe_write(it->first, &it->second.first, it->second.second)) ++n;
               g_rd_all.clear(); g_rd_all_done = false; out["restored"] = n; return out; }
    if (g_rd_all_done) { out["patched"] = g_rd_all.size(); return out; }
    const uintptr_t atk = find_record(kTblAttack, "Necro_AoE_RaiseGrunts_Tick", err);
    const uintptr_t aiAll = find_record(kTblCharType, "AI_All", err), orcs = find_record(kTblCharType, "All_Orcs", err), orcsOlog = find_record(kTblCharType, "All_Orcs_OlogHai", err);
    if (!atk || !aiAll || !orcs || !orcsOlog) { out["error"] = err; return out; }
    const uint32_t H_ATTACK = 0x4B301C78, H_MAX = 0x6129249C, H_ALLY = 0x59FCD14C, H_EXITEMS = 0xE44C8883, H_INC = 0xDD4EA5F7, H_EXC = 0xF15EA6B9;
    const uintptr_t desc = rq(game_base() + kTblAOE); uint32_t n = 0; safe_read(desc + 0x28, &n, 4); const uintptr_t ents = rq(desc + 0x38);
    std::set<uintptr_t> ticks; char nb[96];
    for (uint32_t i = 0; i < n && ents; ++i) {
        const uintptr_t e = ents + (uintptr_t)i * 0x28, np = rq(e + 0x20);
        if (!np || !safe_read(np, nb, sizeof nb)) continue; nb[sizeof nb - 1] = 0;
        if (strncmp(nb, "PC_NecroAoE_", 12) != 0) continue;
        const uintptr_t d = rq(e + 0x10);
        for (int k = 0; k < 16 && d; ++k) {
            const uintptr_t v = rq(d + 8 * k); if (v < 0x10000) continue;
            uintptr_t a; int c; if (tick_field(v, H_ATTACK, a, c) && c == 1 && rq(a) == atk) ticks.insert(v);
        }
    }
    int beasts = 0;
    for (uintptr_t t : ticks) {
        uintptr_t a; int c;
        if (tick_field(t, H_MAX, a, c) && c == 1) rd_put(a, 30, 4);
        if (tick_field(t, H_ALLY, a, c) && c == 1 && rq(a)) rd_put(a, 0, 8);
        if (tick_field(t, H_EXITEMS, a, c) && c == 1 && rq(a)) rd_put(a, 0, 8);
        uintptr_t ea; int ec;
        if (tick_field(t, H_EXC, ea, ec) && ec == 1 && rq(ea) == orcs) {          // the beast tick
            rd_put(ea, orcsOlog, 8);
            uintptr_t ia; int ic; if (tick_field(t, H_INC, ia, ic)) for (int j = 0; j < ic && j < 8; ++j) rd_put(ia + 8 * j, aiAll, 8);
            ++beasts;
        }
    }
    g_rd_all_done = true; out["ticks"] = ticks.size(); out["beast_ticks"] = beasts; out["patched"] = g_rd_all.size();
    return out;
}

// Decapitated corpses (2026-11-13): Combat/Sever/FX head records (Orc_Head, *_HeadExplode*, *_HeadRemove, Caragor_Head...)
// have IsHeadSever = True and FactionMemberCanResurrect = False, which marks the body as never resurrectable. Flip both
// (bit-packed bools: dword at data + (slot>>5)*8, bit slot&31; found by field hash). Heads still fly off; only bodies cut
// AFTER the change are affected. *_Dissolve records are skipped (the body melts away). Side effect: beheaded captains
// can come back in the Nemesis system like any other "dead" captain.
std::vector<std::pair<uintptr_t, uint32_t>> g_rd_sev; bool g_rd_sev_done = false;
json rd_decap(bool on) {
    json out = json::object();
    if (!on) { int n = 0; for (auto it = g_rd_sev.rbegin(); it != g_rd_sev.rend(); ++it) if (safe_write(it->first, &it->second, 4)) ++n; g_rd_sev.clear(); g_rd_sev_done = false; out["restored"] = n; return out; }
    if (g_rd_sev_done) { out["patched"] = g_rd_sev.size(); return out; }
    const uint32_t H_HEAD = 0xFE1565F7, H_CANRES = 0xF47F2B9F; constexpr uintptr_t kTblSeverFX = 0x2700B68;
    const uintptr_t desc = rq(game_base() + kTblSeverFX); uint32_t n = 0; safe_read(desc + 0x28, &n, 4); const uintptr_t ents = rq(desc + 0x38);
    char nb[96]; int recs = 0;
    auto setbit = [](uintptr_t a, int slot, bool v) -> bool {
        const uintptr_t w = a; uint32_t d = 0; if (!safe_read(w, &d, 4)) return false;
        const uint32_t o = d; const uint32_t m = 1u << (slot & 31);
        d = v ? (d | m) : (d & ~m); if (d == o) return false;
        if (!safe_write(w, &d, 4)) return false; g_rd_sev.push_back({ w, o }); return true; };
    for (uint32_t i = 0; i < n && ents; ++i) {
        const uintptr_t e = ents + (uintptr_t)i * 0x28, np = rq(e + 0x20);
        if (!np || !safe_read(np, nb, sizeof nb)) continue; nb[sizeof nb - 1] = 0;
        if (strstr(nb, "Dissolve")) continue;
        uintptr_t a; int c; bool hit = false;
        // tick_field gives data + slot*8; for bools the slot is a BIT index -> recompute the dword address
        const uintptr_t data = rq(e + 0x10); if (!data) continue;
        if (tick_field(e, H_HEAD, a, c)) { const int sl = (int)((a - data) / 8); hit |= setbit(data + (sl >> 5) * 8, sl, false); }
        if (tick_field(e, H_CANRES, a, c)) { const int sl = (int)((a - data) / 8); hit |= setbit(data + (sl >> 5) * 8, sl, true); }
        if (hit) ++recs;
    }
    g_rd_sev_done = true; out["records"] = recs; out["patched"] = g_rd_sev.size();
    return out;
}
}
// Dark Talion the trainer added in this world load (player, inventory, world-load counter). Game thread only.
static uintptr_t g_dt_P = 0, g_dt_inv = 0; static uint64_t g_dt_epoch = 0;
json sow_cmd_raise_dead(const json& a) {
    const bool on = a.value("on", true), dt = a.value("dark_talion", true), ups = a.value("upgrades", true), comb = a.value("combine", true), all = a.value("everything", true), decap = a.value("decapitated", true);
    return run_on_main([=]() -> json {
        std::string err; json out = json::object();
        const uintptr_t row = find_record(kTblMenuTreeBase, "SkillRow6_RingWraith", err);
        const uintptr_t raise = find_record(kTblMenuSkills, "SkillRow7_Node_Parent_Raise", err);
        const uintptr_t prest = find_record(kTblMenuSkills, "SkillRow6_Node_Ring_Prestige", err);
        const uintptr_t isdt = find_record(kTblUiPlayerPred, "Is_Dark_Talion", err);
        const uintptr_t hasrd = find_record(kTblUiPlayerPred, "HasSkill_RaiseDeadUnlock", err);
        if (!row || !raise || !prest || !isdt || !hasrd) return fail("NO_RECORD", err);
        if (on) {
            // 1a. Story row slot
            if (!g_rd_row.addr) {
                const uintptr_t data = rq(row + 0x10);
                for (int k = 0; k < 64 && data; ++k) {
                    const uintptr_t v = rq(data + 8 * k);
                    if (v == raise) { g_rd_row = { data + 8 * k, prest }; break; }          // already patched
                    if (v == prest) { if (wq(data + 8 * k, raise)) g_rd_row = { data + 8 * k, prest }; break; }
                }
            }
            out["menu_row"] = g_rd_row.addr ? hexstr(g_rd_row.addr) : "not found";
            // 1b. creation predicate of the Raise node: Is_Dark_Talion -> HasSkill_RaiseDeadUnlock
            if (!g_rd_pred.addr) {
                const uintptr_t nd = rq(raise + 0x10);
                for (int k = 0; k < 48 && nd && !g_rd_pred.addr; ++k) {
                    const uintptr_t s = rq(nd + 8 * k); const uintptr_t sd = s ? rq(s + 0x10) : 0;
                    if (!sd) continue;
                    for (int j = 0; j < 16; ++j) {
                        const uintptr_t v = rq(sd + 8 * j);
                        if (v == hasrd) { g_rd_pred = { sd + 8 * j, isdt }; break; }
                        if (v == isdt) { if (wq(sd + 8 * j, hasrd)) g_rd_pred = { sd + 8 * j, isdt }; break; }
                    }
                }
            }
            out["menu_gate"] = g_rd_pred.addr ? hexstr(g_rd_pred.addr) : "not found";
        } else {
            if (g_rd_row.addr && rq(g_rd_row.addr) == raise) wq(g_rd_row.addr, g_rd_row.orig);
            if (g_rd_pred.addr && rq(g_rd_pred.addr) == hasrd) wq(g_rd_pred.addr, g_rd_pred.orig);
            g_rd_row = {}; g_rd_pred = {};
        }
        out["combine"] = rd_combine(on && comb);
        out["attack_gate"] = rd_attack_gate(on && !dt);
        out["elven_rage"] = rd_rage_fix(on && dt);
        if (sow_world_ready()) out["undead_bodies"] = rd_undead_bodies(on && dt);
        out["everything"] = rd_everything(on && all);
        out["decapitated"] = rd_decap(on && decap);
        // 2 + 3: items (needs a player)
        uintptr_t P, INV;
        if (!player_inv(P, INV, err)) { out["items"] = "no player yet"; return ok(out); }
        auto cnt = [&](uintptr_t rec) -> int64_t { json c = item_count_on_main(INV, rec); return c.value("ok", false) ? c["data"].value("count", (int64_t)0) : -1; };
        json added = json::array();
        if (on && ups) {
            for (const char* n : { "PS_Raise_Parent_UNLOCK", "PS_Raise_Parent_SKILL", "PS_Raise_Child1_UNLOCK", "PS_Raise_Child1_SKILL",
                                   "PS_Raise_Child2_UNLOCK", "PS_Raise_Child2_SKILL", "PS_Raise_Child3_UNLOCK", "PS_Raise_Child3_SKILL" }) {
                const uintptr_t r = find_record(kTblItems, n, err); if (!r || cnt(r) != 0) continue;
                json g = guarded_call(game_base() + kAddItem, { INV, r, 1 }, "u64", true); if (!g.value("ok", false)) return g;
                added.push_back(n);
            }
        }
        const uintptr_t dtr = find_record(kTblItems, "GP_PlayerIsDarkTalion", err);
        if (dtr) {
            const int64_t c = cnt(dtr);
            // Only take Dark Talion away when the trainer gave it; a save that earned it in the story keeps it.
            // Verified 2026-11-15: GP_PlayerIsDarkTalion is NOT written to the save (grant -> quit -> reload = 0; absent
            // from 62 saves incl. 100% story ones) - the game grants it per load. So "ours" only has to last for this
            // world load: remember the world-load counter + player + inventory the trainer granted it on (a reload
            // bumps the counter even when the game reuses the same addresses).
            const uint64_t epoch = sow_world_epoch();
            const bool ours = g_dt_inv && g_dt_inv == INV && g_dt_P == P && g_dt_epoch == epoch;
            if (on && dt && c == 0) { json g = guarded_call(game_base() + kAddItem, { INV, dtr, 1 }, "u64", true); if (!g.value("ok", false)) return g; added.push_back("GP_PlayerIsDarkTalion"); g_dt_P = P; g_dt_inv = INV; g_dt_epoch = epoch; }
            if ((!on || !dt) && c > 0 && ours) {
                // only the one the trainer added: if the game granted its own as well (story-complete save), that stays
                json r = guarded_call(game_base() + kRemoveItem, { INV, dtr, 1 }, "u64", true);
                if (r.value("ok", false)) g_dt_P = g_dt_inv = 0; else out["dark_talion_error"] = r.value("message", std::string("remove failed"));
            }
            else if ((!on || !dt) && c > 0) out["dark_talion_kept"] = "earned in the story";
            out["dark_talion"] = cnt(dtr);
        }
        out["added"] = added;
        return ok(out);
    }, a.value("timeout_ms", 8000));
}
// ================================ Instant Dominate (the fill gauge) ================================
// The domination gauge (hold on a grabbed / standing enemy) is the looping hold attack's LoopingForcedDuration =
// Combat/Stat/Formula/ValueOption HOG_Duration (21 attacks: PC_HoG_Standing_Hold_*, PC_Grab_HOG_Hold_*, Graug /
// Monster / Sauron / statue drain) and HOG_Duration_Critter. ValueOption data +0 = struct; its data: +0 float RawValue,
// +0x10 Formula (Formula_HoG_Duration: ~3 s, longer for tougher targets). Formula null -> RawValue is used.
constexpr uintptr_t kTblValueOption = 0x2700B10;
struct DomFastPatch { uintptr_t at = 0; float raw = 0; uintptr_t formula = 0; };
std::vector<DomFastPatch> g_domfast;
static void domfast_restore() { for (auto it = g_domfast.rbegin(); it != g_domfast.rend(); ++it) { wq(it->at + 0x10, it->formula); safe_write(it->at, &it->raw, 4); } g_domfast.clear(); }
// runs on the game thread (review 2026-11-14: the tick / the game read the same data)
static json sow_cmd_dominate_fast_main(const json& a);
json sow_cmd_dominate_fast(const json& a) { return run_on_main([a]() -> json { return sow_cmd_dominate_fast_main(a); }, 3000); }
static json sow_cmd_dominate_fast_main(const json& a) {
    const bool on = a.value("on", true); json out = json::object(); std::string err;
    if (!on) { out["restored"] = g_domfast.size(); domfast_restore(); return ok(out); }
    const float secs = std::clamp(a.value("seconds", 0.1f), 0.05f, 10.f);
    for (const char* nm : { "HOG_Duration", "HOG_Duration_Critter" }) {
        const uintptr_t rec = find_record(kTblValueOption, nm, err);
        const uintptr_t st = rec ? rq(rq(rec + 0x10)) : 0, at = st ? rq(st + 0x10) : 0;
        if (!at) { out[nm] = "not found"; continue; }
        bool have = false; for (auto& p : g_domfast) if (p.at == at) have = true;
        if (!have) { DomFastPatch p; p.at = at; safe_read(at, &p.raw, 4); p.formula = rq(at + 0x10); g_domfast.push_back(p); }
        wq(at + 0x10, 0); safe_write(at, &secs, 4); out[nm] = secs;
    }
    return ok(out);
}

// ================================ Instant Dominate ================================
// Captains carry Item_Immune_GrabKnockdown / Item_Resistant_GrabKnockdown, which send the grab to PC_Grab_Into_Immune
// (they shake Talion off) until they are weakened / broken. Those two checks are the item lists of the attack predicates
// PredicateInventoryItemTargetIncluded Biped_Immune_GrabKnockdown / Biped_Resistant_GrabKnockdown (record data +0 ->
// ItemList struct, its data = the item pointer array). Swapping the two captain items for I_Am_Nazgul_Sauron_Fair (already
// in the immune list, no orc has it) lets a captain at full health be grabbed like a grunt; the grab's hold then goes
// into domination (PC_Grab_HOG_Loop -> PC_Grab_Domination_Into, captains pass Domination_BrokenOrSquealerOrCaptain).
// Nazgul and Olog checks stay as they are.
constexpr uintptr_t kTblPredItemTgtInc = 0x2700140;
std::vector<RdPatch> g_dom;
// runs on the game thread (review 2026-11-14: the tick / the game read the same data)
static json sow_cmd_instant_dominate_main(const json& a);
json sow_cmd_instant_dominate(const json& a) { return run_on_main([a]() -> json { return sow_cmd_instant_dominate_main(a); }, 3000); }
static json sow_cmd_instant_dominate_main(const json& a) {
    const bool on = a.value("on", true); json out = json::object(); std::string err;
    if (!on) { int n = 0; for (auto it = g_dom.rbegin(); it != g_dom.rend(); ++it) if (wq(it->addr, it->orig)) ++n; g_dom.clear(); out["restored"] = n; return ok(out); }
    if (g_dom.size() == 2) { out["patched"] = g_dom.size(); return ok(out); }
    // review 2026-11-15: only one of the two predicates was patched last time - put it back and try both again
    for (auto it = g_dom.rbegin(); it != g_dom.rend(); ++it) wq(it->addr, it->orig);
    g_dom.clear();
    const uintptr_t never = find_record(kTblItems, "I_Am_Nazgul_Sauron_Fair", err);
    const uintptr_t imm = find_record(kTblItems, "Item_Immune_GrabKnockdown", err), res = find_record(kTblItems, "Item_Resistant_GrabKnockdown", err);
    if (!never || !imm || !res) return fail("not_found", "item not found: " + err);
    const std::pair<const char*, uintptr_t> preds[] = { { "Biped_Immune_GrabKnockdown", imm }, { "Biped_Resistant_GrabKnockdown", res } };
    for (auto& pr : preds) {
        const uintptr_t rec = find_record(kTblPredItemTgtInc, pr.first, err);
        const uintptr_t list = rec ? rq(rq(rec + 0x10)) : 0, arr = list ? rq(list + 0x10) : 0;
        uintptr_t slot = 0;
        for (int k = 0; k < 16 && arr; ++k) { const uintptr_t v = rq(arr + 8 * k); if (v == pr.second) { slot = arr + 8 * k; break; } if (v < 0x10000) break; }
        if (!slot) { out[pr.first] = "slot not found"; continue; }
        g_dom.push_back({ slot, pr.second }); wq(slot, never); out[pr.first] = "off";
    }
    // review 2026-11-15: nothing patched -> report failure so the switch does not show ON
    if (g_dom.empty()) return fail("not_found", "the captain grab rules were not found - nothing changed");
    return ok(out);
}

// SOW.SKILL_COMPAT {on} - fixes for upgrades that block each other with "Choose Multiple Skills" (2026-11-13).
// Shadow Dominate did nothing with every upgrade owned: Combat/Pool PC_ShadowBrand_GetThere_Default (the dash-to-target
// moves Dominate uses) has Predicate.ExcludedItem = PS_ShadowGrab_Child3_SKILL. That PredicateItem struct is SHARED with
// the Kill / KD / OffGrndAi GetThere pools (their Pull alternative still works), so only THIS pool's reference is
// cleared, never the shared struct. User-verified: Dominate, Shadow Strike and Shadow Strike Kill all work.
namespace {
struct CompatFix { const char* pool; uintptr_t addr = 0, orig = 0; };
CompatFix g_compat[] = { { "PC_ShadowBrand_GetThere_Default" } };
}
json sow_cmd_skill_compat(const json& a) {
    const bool on = a.value("on", true);
    return run_on_main([on]() -> json {
        constexpr uintptr_t kTblPool = 0x2700908;
        const uint32_t H_PREDICATE = 0xF6BB663F, H_EXCLUDED_ITEM = 0x9AC05FAD;
        json out = json::array();
        for (auto& f : g_compat) {
            json r = { {"pool", f.pool} };
            if (!on) {
                if (f.addr && f.orig && rq(f.addr) == 0) { wq(f.addr, f.orig); r["state"] = "restored"; }
                else r["state"] = "unchanged";
                f.addr = f.orig = 0; out.push_back(r); continue;
            }
            std::string err; const uintptr_t pool = find_record(kTblPool, f.pool, err);
            if (!pool) { r["state"] = "no record: " + err; out.push_back(r); continue; }
            uintptr_t pa; int pc;
            if (!tick_field(pool, H_PREDICATE, pa, pc) || !rq(pa)) { r["state"] = "no predicate"; out.push_back(r); continue; }
            uintptr_t ea; int ec;
            if (!tick_field(rq(pa), H_EXCLUDED_ITEM, ea, ec)) { r["state"] = "no ExcludedItem field"; out.push_back(r); continue; }
            const uintptr_t v = rq(ea);
            if (v == 0) r["state"] = f.addr ? "in place (applied earlier)" : "already clear";
            else if (wq(ea, 0)) { f.addr = ea; f.orig = v; r["state"] = "applied now"; }
            else r["state"] = "write failed";
            out.push_back(r);
        }
        return ok({ {"fixes", out} });
    }, a.value("timeout_ms", 5000));
}

} // namespace wb
