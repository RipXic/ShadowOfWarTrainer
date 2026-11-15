// sow_army.cpp - Shadow of War army editor through the game's OWN functions (2026-11-12).
//   FM = [[exe+0x26FFAA8] + 0x6C68]; FM+0x98 faction slots (count FM+0xA0, stride 0x290): slot+8 = Faction object F;
//   [F+0x20] = member pool; pool+0x18..+0x20 = vector<FactionMember*>; member id (+0x174) = index. Pools are shared:
//   de-duplicate. Records (class, tribe, traits...) are DB entries: name char* at entry+0x20.
// Every write goes through a game function on the game thread (guarded_call). Read-only listing runs anywhere.
#include "common.h"
#include <set>
#include <fstream>

namespace wb {
namespace {
constexpr uintptr_t kRoot = 0x26FFAA8;
constexpr uintptr_t kSetXPLevel = 0x35188C, kSetFlag = 0x1156740, kApplyReward = 0x3511F4, kSetFollower = 0x7D52B0,
                    kSetBodyguard = 0xF88AF0, kSetIntel = 0x353EC4, kSetPersonality = 0x1BC60A0, kSetName = 0x1BC6040,
                    kBuild = 0x1B5B824, kSetTarget = 0x1B5EE34;
constexpr uintptr_t kTblRewards = 0x2701C48, kTblBuilders = 0x2701BE8, kTblPersonalities = 0x2701A88;

uintptr_t q8(uintptr_t a) { uintptr_t v = 0; return a && safe_read(a, &v, 8) ? v : 0; }
uint32_t q4(uintptr_t a) { uint32_t v = 0; if (a) safe_read(a, &v, 4); return v; }
std::string rname(uintptr_t rec) {
    if (!rec) return "";
    char b[96] = {}; const uintptr_t np = q8(rec + 0x20);
    if (!np || !safe_read(np, b, sizeof b - 1)) return "?";
    return b;
}
std::string cname(uintptr_t m) {                   // localized first name, char[32] Latin-1 at +0x498
    char b[33] = {}; safe_read(m + 0x498, b, 32); std::string s;
    for (char c : b) { if (!c) break; s += (unsigned char)c < 0x80 ? c : '?'; }
    return s;
}
uintptr_t faction_mgr() { const uintptr_t r = q8(game_base() + kRoot); return r ? q8(r + 0x6C68) : 0; }

// every member, de-duplicated across shared pools
std::vector<uintptr_t> all_members() {
    std::vector<uintptr_t> out; const uintptr_t FM = faction_mgr(); if (!FM) return out;
    const uintptr_t slots = q8(FM + 0x98); const uint32_t n = q4(FM + 0xA0);
    std::set<uintptr_t> pools, seen;
    for (uint32_t i = 0; i < n && i < 256; ++i) {
        const uintptr_t F = slots + (uintptr_t)i * 0x290 + 8;                 // the Faction object is INLINE in the slot
        const uintptr_t pool = q8(F + 0x20); if (!pool || !pools.insert(pool).second) continue;
        const uintptr_t b = q8(pool + 0x18), e = q8(pool + 0x20);
        if (!b || e < b || (e - b) / 8 > 20000) continue;
        for (uintptr_t p = b; p < e; p += 8) { const uintptr_t m = q8(p); if (m && seen.insert(m).second) out.push_back(m); }
    }
    return out;
}
uint64_t flags(uintptr_t m) { uint64_t f = 0; safe_read(m + 0x508, &f, 8); return f; }
bool bit(uint64_t f, int b) { return (f >> b) & 1; }

json traits_of(uintptr_t m, bool all) {
    json t = json::array();
    const uintptr_t b = q8(m + 0x1E0), e = q8(m + 0x1E8);
    if (b && e >= b && (e - b) / 0x20 < 200)
        for (uintptr_t p = b; p < e; p += 0x20) { const uintptr_t tr = q8(p + 8); if (tr) t.push_back(rname(tr)); else if (all) t.push_back("(" + rname(q8(p)) + ": empty)"); }
    return t;
}
json summary(uintptr_t m, bool detail) {
    const uint64_t f = flags(m);
    json j = {
        {"id", q4(m + 0x174)}, {"ptr", hexstr(m)}, {"name", cname(m)},
        {"level", (int)q4(m + 0x4B8) + 1}, {"pending_levels", q4(m + 0x470)},
        {"region", rname(q8(m + 0x178))}, {"row", rname(q8(m + 0x268))},
        {"class", rname(q8(m + 0x188))}, {"tribe", rname(q8(m + 0x328))}, {"role", rname(q8(m + 0x318))},
        {"rarity", rname(q8(m + 0x390))},
        {"follower", bit(f, 27)}, {"dead", bit(f, 2)}, {"bodyguard", bit(f, 29)}, {"spy", bit(f, 28)},
    };
    const uint8_t col = 0xFF; uint8_t c = col; safe_read(m + 0x51B, &c, 1); if (c != 0xFF) j["column"] = c;
    if (q8(m + 0x190)) j["preset"] = rname(q8(m + 0x190));
    if (bit(f, 41)) j["level_locked"] = true;
    if (detail) {
        j["personality"] = rname(q8(m + 0x270)); j["model"] = rname(q8(m + 0x278)); j["builder"] = rname(q8(m + 0x3A0));
        j["traits"] = traits_of(m, true);
        json mk = json::array(); const uintptr_t b = q8(m + 0x1B0), e = q8(m + 0x1B8);
        if (b && e >= b && (e - b) / 8 < 300) for (uintptr_t p = b; p < e; p += 8) mk.push_back(rname(q8(p)));
        j["markers"] = mk; j["flags"] = hexstr(f); j["spawned_char"] = hexstr(q8(m + 0x48));
        j["intel"] = [&]() { uint8_t v = 0; safe_read(m + 0x51A, &v, 1); return (int)v; }();
    } else j["traits"] = traits_of(m, false);
    return j;
}
uintptr_t find_member(const json& a, std::string& err) {
    if (a.contains("ptr")) { uintptr_t p; if (!eval_addr(a["ptr"].get<std::string>(), p, &err)) return 0; return p; }
    if (!a.contains("id")) { err = "id (member id from SOW.ARMY) or ptr is required"; return 0; }
    uint32_t id = 0;
    if (a["id"].is_string()) { const std::string t = a["id"].get<std::string>(); if (t.empty() || !isdigit((unsigned char)t[0])) { err = "pick an orc first"; return 0; } id = (uint32_t)strtoul(t.c_str(), nullptr, 10); }
    else id = a["id"].get<uint32_t>();
    const std::string region = a.value("region", std::string());
    for (uintptr_t m : all_members()) if (q4(m + 0x174) == id && (region.empty() || rname(q8(m + 0x178)) == region)) return m;
    err = "no member with id " + std::to_string(id); return 0;
}
uintptr_t find_rec(uintptr_t table_global, const std::string& name, std::string& err) {
    const uintptr_t d = q8(game_base() + table_global); const uint32_t n = q4(d + 0x28); const uintptr_t e = q8(d + 0x38);
    for (uint32_t i = 0; e && i < n; ++i) if (rname(e + (uintptr_t)i * 0x28) == name) return e + (uintptr_t)i * 0x28;
    err = "no record '" + name + "'"; return 0;
}
// shared: member + world-ready + game thread + optional preset guard
json with_member(const json& a, bool allow_preset, std::function<json(uintptr_t m)> fn) {
    if (!sow_world_ready()) return fail("NOT_READY", "not in the world yet (loading?)");
    return run_on_main([=]() -> json {
        std::string err; const uintptr_t m = find_member(a, err);
        if (!m) return fail("NO_MEMBER", err);
        if (!allow_preset && q8(m + 0x190) && !a.value("force", false))
            return fail("PRESET", cname(m) + " is a story/unique character (" + rname(q8(m + 0x190)) + "): quests depend on him - add force:true to edit anyway");
        json r = fn(m);
        if (r.value("ok", false)) r["data"]["member"] = summary(m, false);
        return r;
    }, a.value("timeout_ms", 5000));
}
json call(uintptr_t rva, std::vector<uint64_t> args) { return guarded_call(game_base() + rva, args, "u64"); }
} // namespace

// ------------------------------------------------------------------ read
json sow_cmd_army(const json& a) {
    const bool followers = a.value("followers", true), dead = a.value("dead", true);
    std::string f = a.value("filter", std::string()); std::transform(f.begin(), f.end(), f.begin(), ::tolower);
    json out = json::array(); size_t total = 0;
    for (uintptr_t m : all_members()) {
        ++total; const uint64_t fl = flags(m);
        if (followers && !bit(fl, 27)) continue;
        if (!dead && bit(fl, 2)) continue;
        json s = summary(m, false);
        if (!f.empty()) { std::string blob = s.dump(); std::transform(blob.begin(), blob.end(), blob.begin(), ::tolower); if (blob.find(f) == std::string::npos) continue; }
        out.push_back(s);
        if (out.size() >= (size_t)a.value("max", 400)) break;
    }
    return ok({ {"total_members", total}, {"shown", out.size()}, {"members", out} });
}
json sow_cmd_army_options(const json& a) {
    const bool all = a.value("all", false); std::string txt;
    for (uintptr_t m : all_members()) {
        const uint64_t f = flags(m); if (!all && !bit(f, 27)) continue; if (bit(f, 2)) continue;
        std::string cls = rname(q8(m + 0x188)); if (cls.rfind("AI_", 0) == 0) cls = cls.substr(3);
        std::string reg = rname(q8(m + 0x178)); if (reg.rfind("Faction_", 0) == 0) reg = reg.substr(8);
        txt += std::to_string(q4(m + 0x174)) + ": " + cname(m) + " (Lv " + std::to_string((int)q4(m + 0x4B8) + 1) + " " + cls + ") " + reg + (q8(m + 0x190) ? " [story]" : "") + '\n';
    }
    return ok({ {"text", txt} });
}
json sow_cmd_orc_card(const json& a) {
    std::string err; const uintptr_t m = find_member(a, err); if (!m) return ok({ {"text", err} });
    const uint64_t f = flags(m); std::string cls = rname(q8(m + 0x188)), tr = rname(q8(m + 0x328)), ro = rname(q8(m + 0x318)), ra = rname(q8(m + 0x390));
    std::string t = cname(m) + " - Lv " + std::to_string((int)q4(m + 0x4B8) + 1) + " " + cls + " / " + tr + " / " + ra + (bit(f, 29) ? " / bodyguard" : "") + (bit(f, 41) ? " / level-locked" : "") + (bit(f, 10) ? " / no level cap" : "");
    return ok({ {"text", t} });
}
json sow_cmd_orc(const json& a) {
    std::string err; const uintptr_t m = find_member(a, err);
    if (!m) return fail("NO_MEMBER", err);
    return ok(summary(m, true));
}

// ------------------------------------------------------------------ edits (game functions, game thread)
// SOW.ORC_LEVEL {id, level 1..60+, unlock:true}: FactionMember::SetXPLevel 0x35188C(m, level-1); clears the game's
// CanNotChangeXPLevel flag (bit 41) first when unlock (it silently blocks raises). Clamped by the game to the cap.
json sow_cmd_orc_level(const json& a) {
    const int lvl = a["level"].is_string() ? atoi(a["level"].get<std::string>().c_str()) : a.value("level", 0);
    if (lvl < 1 || lvl > 100) return fail("BAD_ARGS", "level 1..100 (the game clamps to its cap)");
    return with_member(a, false, [=](uintptr_t m) -> json {
        if (bit(flags(m), 41) && a.value("unlock", true)) { json r = call(kSetFlag, { m, 41, 0 }); if (!r.value("ok", false)) return r; }
        if (a.value("no_cap", false) && !bit(flags(m), 10)) { json r = call(kSetFlag, { m, 10, 1 }); if (!r.value("ok", false)) return r; }
        json r = call(kSetXPLevel, { m, (uint64_t)(lvl - 1) }); if (!r.value("ok", false)) return r;
        return ok({ {"level", (int)q4(m + 0x4B8) + 1} });
    });
}
// SOW.ORC_REWARD {id, reward}: the game's ApplyReward 0x3511F4(m, Faction/Rewards record, 0, 1) - traits, tribe,
// personality, level delta, items, relationships exactly as the game's own reward data defines them.
json sow_cmd_orc_reward(const json& a) {
    const std::string rw = a.value("reward", std::string());
    if (rw.empty()) return fail("BAD_ARGS", "reward is required (SOW.REWARDS lists them)");
    return with_member(a, false, [=](uintptr_t m) -> json {
        std::string err; const uintptr_t rec = find_rec(kTblRewards, rw, err); if (!rec) return fail("NO_RECORD", err);
        json before = traits_of(m, false);
        json r = call(kApplyReward, { m, rec, 0, 1 }); if (!r.value("ok", false)) return r;
        return ok({ {"traits_before", before}, {"traits_after", traits_of(m, false)} });
    });
}
// SOW.ORC_FOLLOWER {id, on}: FactionMember::SetPlayerFollower 0x7D52B0 (the game's recruit: intel, level sync, relations)
json sow_cmd_orc_follower(const json& a) {
    const bool on = a.value("on", true);
    return with_member(a, false, [=](uintptr_t m) -> json {
        json r = call(kSetFollower, { m, (uint64_t)on }); if (!r.value("ok", false)) return r;
        return ok({ {"follower", bit(flags(m), 27)} });
    });
}
// SOW.ORC_BODYGUARD {id, on}: SetPlayerBodyguard 0xF88AF0
json sow_cmd_orc_bodyguard(const json& a) {
    const bool on = a.value("on", true);
    return with_member(a, false, [=](uintptr_t m) -> json {
        json r = call(kSetBodyguard, { m, (uint64_t)on }); if (!r.value("ok", false)) return r;
        return ok({ {"bodyguard", bit(flags(m), 29)} });
    });
}
// SOW.ORC_INTEL {id}: 0x353EC4(m, 0xFF, 0, 0) - everything known (strengths, weaknesses)
json sow_cmd_orc_intel(const json& a) {
    return with_member(a, true, [=](uintptr_t m) -> json {
        json r = call(kSetIntel, { m, 0xFF, 0, 0 }); if (!r.value("ok", false)) return r;
        uint8_t v = 0; safe_read(m + 0x51A, &v, 1); return ok({ {"intel", (int)v} });
    });
}
// SOW.ORC_PERSONALITY {id, personality}: 0x1BC60A0(m, Faction/Personalities record)
json sow_cmd_orc_personality(const json& a) {
    const std::string pn = a.value("personality", std::string());
    return with_member(a, false, [=](uintptr_t m) -> json {
        std::string err; const uintptr_t rec = find_rec(kTblPersonalities, pn, err); if (!rec) return fail("NO_RECORD", err);
        json r = call(kSetPersonality, { m, rec }); if (!r.value("ok", false)) return r;
        return ok({ {"personality", rname(q8(m + 0x270))} });
    });
}
// SOW.ORC_BUILD {builder, count}: a NEW orc from a Faction/FactionMemberBuilder record in the current region, through
// the game's own creator 0x1B5B824(FM, builder, [FM+0x1B8], 0) (unique-instance + slot checks included)
json sow_cmd_orc_build(const json& a) {
    const std::string bn = a.value("builder", std::string());
    if (bn.empty()) return fail("BAD_ARGS", "builder is required (SOW.BUILDERS lists them)");
    if (!sow_world_ready()) return fail("NOT_READY", "not in the world yet (loading?)");
    return run_on_main([=]() -> json {
        std::string err; const uintptr_t rec = find_rec(kTblBuilders, bn, err); if (!rec) return fail("NO_RECORD", err);
        const uintptr_t FM = faction_mgr(); if (!FM) return fail("NO_FACTIONS", "faction manager missing");
        const size_t before = all_members().size();
        json r = call(kBuild, { FM, rec, q8(FM + 0x1B8), 0 }); if (!r.value("ok", false)) return r;
        const uintptr_t m = (uintptr_t)r["data"]["ret_i"].get<int64_t>();
        json d = { {"members_before", before}, {"members_after", all_members().size()}, {"ret", hexstr(m)} };
        if (m) { bool known = false; for (uintptr_t x : all_members()) if (x == m) { known = true; break; } if (known) d["member"] = summary(m, true); }
        return ok(d);
    }, 5000);
}

static json list_table(uintptr_t g, const json& a) {
    std::string f = a.value("filter", std::string()); std::transform(f.begin(), f.end(), f.begin(), ::tolower);
    const uintptr_t d = q8(game_base() + g); const uint32_t n = q4(d + 0x28); const uintptr_t e = q8(d + 0x38);
    json out = json::array(); const size_t mx = a.value("max", 300);
    for (uint32_t i = 0; e && i < n && out.size() < mx; ++i) { std::string s = rname(e + (uintptr_t)i * 0x28), lo = s; std::transform(lo.begin(), lo.end(), lo.begin(), ::tolower); if (f.empty() || lo.find(f) != std::string::npos) out.push_back(s); }
    return ok({ {"total", n}, {"names", out} });
}
json sow_cmd_rewards(const json& a) { return list_table(kTblRewards, a); }
json sow_cmd_builders(const json& a) { return list_table(kTblBuilders, a); }

// ------------------------------------------------------------------ gear, role, name, rarity
// m+0x50 = embedded equipment: 10 weapon slot ptrs (EInventorySlot) then 15 armor slot ptrs (EInventoryArmorSlot).
// item object: +0x38 Inventory/Items rec, +0x40 EquippedWeaponData, +0x48 ArmorData, +0x100 u8 source.
namespace {
constexpr uintptr_t kTblItems = 0x2700530, kTblRoles = 0x2701D70;
constexpr uintptr_t kGiveItem = 0x355474, kRebuild = 0x61BAD4, kAddItemPlayer = 0x1C01E0, kSetRole = 0x1BC6698;
const char* kWeaponSlots[10] = { "Melee", "MeleeAux", "Ranged", "RangedAux", "Wraith", "QuickThrow", "Utility", "Tribe", "Shield", "MeleeOffhand" };
const char* kArmorSlots[15] = { "Helmet", "Shoulder", "Chest", "Arms", "Pants", "Legs", "Accessory1", "Accessory2", "LowLOD", "HeadAccessory1", "BodyReplacement", "HeadReplacement", "Ring", "Accessory3", "Accessory4" };
json gear_of(uintptr_t m) {
    json g = json::object();
    for (int i = 0; i < 25; ++i) {
        const uintptr_t it = q8(m + 0x50 + (uintptr_t)i * 8); if (!it) continue;
        uint8_t src = 0; safe_read(it + 0x100, &src, 1);
        g[i < 10 ? kWeaponSlots[i] : kArmorSlots[i - 10]] = rname(q8(it + 0x38)) + (src == 9 ? " (given)" : src == 5 ? " (role)" : "");
    }
    return g;
}
// data lists shipped in data\: cached on first use
const std::vector<std::string>& data_lines(const std::string& file) {
    static std::mutex mtx; static std::map<std::string, std::vector<std::string>> cache;
    std::lock_guard<std::mutex> lk(mtx);
    auto it = cache.find(file); if (it != cache.end()) return it->second;
    std::vector<std::string> v; std::ifstream f(g_cfg.root + "\\data\\" + file); std::string l;
    while (std::getline(f, l)) { if (!l.empty() && l[0] != '#') v.push_back(l); }
    return cache[file] = v;
}
std::vector<std::string> cols(const std::string& l) { std::vector<std::string> c; size_t p = 0; while (true) { size_t q = l.find('\t', p); c.push_back(l.substr(p, q == std::string::npos ? std::string::npos : q - p)); if (q == std::string::npos) break; p = q + 1; } return c; }
std::string first_word(std::string s) { const size_t sp = s.find(" ("); if (sp != std::string::npos) s = s.substr(0, sp); const size_t c = s.find(": "); return c != std::string::npos && c < 8 ? s.substr(c + 2) : s; }
} // namespace

json sow_cmd_orc_gear(const json& a) {
    std::string err; const uintptr_t m = find_member(a, err); if (!m) return fail("NO_MEMBER", err);
    json g = gear_of(m); std::string t;
    for (auto& [k, v] : g.items()) t += k + ": " + v.get<std::string>() + "\n";
    return ok({ {"gear", g}, {"text", t.empty() ? std::string("(no gear)") : t} });
}
// SOW.ORC_GIVE_ITEM {id, item}: the game's GiveItem 0x355474(m+0x50, m, rec, 9 = reward source, SMC) -> model rebuild
// 0x61BAD4; if spawned also the live character's inventory, exactly like the reward ItemsToGive path. A piece only
// replaces a role piece with an equal or higher FactionMemberItemReplaceValue (_DoNotReplace = 10).
json sow_cmd_orc_give_item(const json& a) {
    const std::string item = first_word(a.value("item", std::string()));
    if (item.empty()) return fail("BAD_ARGS", "item is required");
    return with_member(a, false, [=](uintptr_t m) -> json {
        std::string err; const uintptr_t rec = find_rec(kTblItems, item, err); if (!rec) return fail("NO_RECORD", err);
        json before = gear_of(m);
        json r = call(kGiveItem, { m + 0x50, m, rec, 9, q8(m + 0x2D8) }); if (!r.value("ok", false)) return r;
        const bool changed = (r["data"]["ret_i"].get<int64_t>() & 0xFF) != 0;
        if (changed) { json rb = call(kRebuild, { m + 0x50, q8(m + 0x2D8) }); if (!rb.value("ok", false)) return rb; }
        const uintptr_t ch = q8(m + 0x48);
        if (changed && ch && q8(ch + 0x2348)) call(kAddItemPlayer, { q8(ch + 0x2348), rec, 1 });
        return ok({ {"changed", changed}, {"gear_before", before}, {"gear_after", gear_of(m)}, {"spawned", ch != 0} });
    });
}
// SOW.ORC_ROLE {id, role}: SetRole 0x1BC6698(m, Faction/Roles/Definitions rec) -> title + the role's gear set
json sow_cmd_orc_role(const json& a) {
    std::string role = a.value("role", std::string()); { const size_t sp = role.find(" - "); if (sp != std::string::npos) role = role.substr(0, sp); }
    if (role.empty()) return fail("BAD_ARGS", "role is required");
    return with_member(a, false, [=](uintptr_t m) -> json {
        std::string err; const uintptr_t rec = find_rec(kTblRoles, role, err); if (!rec) return fail("NO_RECORD", err);
        json r = call(kSetRole, { m, rec }); if (!r.value("ok", false)) return r;
        return ok({ {"role", rname(q8(m + 0x318))}, {"gear", gear_of(m)} });
    });
}
// SOW.ORC_RENAME {id, name | hash}: SetName 0x1BC6040(m, hash) - hash from bin\data\orc_names.txt
json sow_cmd_orc_rename(const json& a) {
    uint32_t hash = 0; std::string want = a.value("name", std::string());
    if (a.contains("hash")) hash = a["hash"].is_string() ? (uint32_t)strtoul(a["hash"].get<std::string>().c_str(), nullptr, 16) : a["hash"].get<uint32_t>();
    else { { const size_t t = want.find('\t'); if (t != std::string::npos) want = want.substr(0, t); }
           for (auto& l : data_lines("orc_names.txt")) { auto c = cols(l); if (c.size() >= 2 && (c[1] == want || c[0] == want)) { hash = (uint32_t)strtoul(c[0].c_str(), nullptr, 16); break; } } }
    if (!hash) return fail("BAD_ARGS", "unknown name '" + want + "' (bin\\data\\orc_names.txt)");
    return with_member(a, false, [=](uintptr_t m) -> json {
        json r = call(kSetName, { m, hash }); if (!r.value("ok", false)) return r;
        return ok({ {"name", cname(m)} });
    });
}
// SOW.ORC_RARITY {id, to: epic|legendary, keep_role: true}: the game's upgrade rewards (Upgrade_CommonOrcToEpic,
// LT_MarkerSwap); they re-roll the role, so the previous role (title + gear) is put back with SetRole.
json sow_cmd_orc_rarity(const json& a) {
    const std::string to = a.value("to", std::string());
    if (to != "epic" && to != "legendary") return fail("BAD_ARGS", "to: epic | legendary");
    const bool keep = a.value("keep_role", true);
    return with_member(a, false, [=](uintptr_t m) -> json {
        const uintptr_t role0 = q8(m + 0x318); const std::string r0 = rname(q8(m + 0x390));
        std::string err; const uintptr_t rec = find_rec(kTblRewards, to == "epic" ? "Upgrade_CommonOrcToEpic" : "LT_MarkerSwap", err);
        if (!rec) return fail("NO_RECORD", err);
        if (to == "epic" && r0.find("Common") == std::string::npos) return fail("NOT_COMMON", "the Epic upgrade only works on Common orcs (this one is " + r0 + ")");
        json r = call(kApplyReward, { m, rec, 0, 1 }); if (!r.value("ok", false)) return r;
        if (keep && role0 && q8(m + 0x318) != role0) { json rr = call(kSetRole, { m, role0 }); if (!rr.value("ok", false)) return rr; }
        return ok({ {"rarity_before", r0}, {"rarity", rname(q8(m + 0x390))}, {"role", rname(q8(m + 0x318))} });
    });
}
// picker texts for the panel
json sow_cmd_army_lists(const json& a) {
    const std::string which = a.value("list", std::string()); std::string f = a.value("filter", std::string()); std::string txt;
    if (which == "names") { for (auto& l : data_lines("orc_names.txt")) { auto c = cols(l); if (c.size() >= 3) txt += c[1] + "\t(" + c[2] + ")\n"; } }
    else if (which == "roles") { for (auto& l : data_lines("orc_roles_titles.txt")) { auto c = cols(l); if (c.size() >= 3 && !c[2].empty()) txt += c[0] + " - " + c[2] + "\n"; } }
    else if (which == "gear") { for (auto& l : data_lines("orc_gear_items.txt")) { auto c = cols(l); if (c.size() >= 3 && (f.empty() || c[0].find(f) != std::string::npos)) txt += c[1] + " (" + c[0] + (c[2] != "0" ? ", priority " + c[2] : "") + ")\n"; } }
    else return fail("BAD_ARGS", "list: names | roles | gear");
    return ok({ {"text", txt} });
}
} // namespace wb
