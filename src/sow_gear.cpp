// sow_gear.cpp - Talion's gear editor.
//   Player P = [[exe+0x26FFA98]+0x888]; inventory INV = [P+0x2348].
//   Owned gear: 25 lists at INV+0x4D0+slot*24 (ten weapon lists, then fifteen armor lists); each list is a
//   begin/end pair of equipment pointers. Equipment: +0x20 id (u32, unique, survives restarts), +0x24 level (u32),
//   +0x28 upgrade, +0x38 Item record, +0x40 WeaponData, +0x48 ArmorData, +0x78 / +0x90 affix pointer vectors
//   (begin/end), +0xF8 nemesis upgrade level, +0xF9 is-nemesis-gear.  Affix (16 B): +0 Inventory/Affix/Definition
//   record, +8 base value (float), +0xC added value (float).
//   The SAVE stores the item record, level, upgrade and WHICH affixes - not the numbers. On load each value is
//   re-derived from definition + level (some with a small random spread). So level and affix-definition edits are
//   persistent; raw values are live only. Equipped items apply stats at equip time (0x40014C returns early when the
//   same item is already equipped), so edits to an equipped item show after re-equipping or a reload.
#include "common.h"
#include <set>
#include <fstream>
#include <algorithm>
#include <cmath>

namespace wb {
namespace {
constexpr uintptr_t kPlayerRoot = 0x26FFA98, kTblAffix = 0x2700488;
uintptr_t gq(uintptr_t a) { uintptr_t v = 0; return a && safe_read(a, &v, 8) ? v : 0; }
uint32_t gd(uintptr_t a) { uint32_t v = 0; if (a) safe_read(a, &v, 4); return v; }
float gf(uintptr_t a) { float v = 0; if (a) safe_read(a, &v, 4); return v; }
std::string rname(uintptr_t rec) {
    char b[128] = {}; const uintptr_t np = gq(rec + 0x20);
    if (!rec || !np || !safe_read(np, b, sizeof b)) return "?"; b[127] = 0; return b;
}
const char* kSlotName[25] = { "Sword", "Dagger", "Bow", "Bow (Celebrimbor)", "Weapon 4", "Weapon 5", "Glaive", "Glaive", "Weapon 8",
    "Weapon 9", "Armor 0", "Cloak", "Armor", "Armor 3", "Hands", "Armor 5", "Scabbard", "Scabbard", "Armor 8", "Hood",
    "Armor 10", "Armor 11", "Ring", "Armor 13", "Armor 14" };

struct Gear { uintptr_t eq; int slot; uint32_t id; };
bool player_inventory(uintptr_t& INV, std::string& err) {
    const uintptr_t root = gq(game_base() + kPlayerRoot), P = root ? gq(root + 0x888) : 0;
    INV = P ? gq(P + 0x2348) : 0;
    if (!INV) { err = "no player (not in the world yet?)"; return false; }
    return true;
}
std::vector<Gear> gear_list(uintptr_t INV) {
    std::vector<Gear> out;
    for (int s = 0; s < 25; ++s) {
        const uintptr_t b = gq(INV + 0x4D0 + s * 24), e = gq(INV + 0x4D0 + s * 24 + 8);
        if (!b || e < b || e - b > 0x8000 || (e - b) % 8) continue;
        for (uintptr_t p = b; p < e; p += 8) { const uintptr_t eq = gq(p); if (eq) out.push_back({ eq, s, gd(eq + 0x20) }); }
    }
    return out;
}
std::vector<uintptr_t> affixes(uintptr_t eq) {   // primary (+0x78) then modifier (+0x90)
    std::vector<uintptr_t> out;
    for (uintptr_t off : { 0x78, 0x90 }) {
        const uintptr_t b = gq(eq + off), e = gq(eq + off + 8);
        if (!b || e < b || e - b > 0x400 || (e - b) % 8) continue;
        for (uintptr_t p = b; p < e; p += 8) { const uintptr_t a = gq(p); if (a) out.push_back(a); }
    }
    return out;
}
std::vector<Gear> gear_list(uintptr_t INV);
// Equipped = referenced from the inventory header (INV+0..0x4D0): armor / cloak / ring / cosmetics directly, weapons
// through their slot object at +0x140 (slots at INV+0x38 + n*0x28). Verified live: 97 sword, 101 dagger, 106 bow,
// 91 cloak, 93 armor, 96 ring.
std::set<uintptr_t> equipped(uintptr_t INV) {
    std::set<uintptr_t> all, s;
    for (auto& g : gear_list(INV)) all.insert(g.eq);
    for (uintptr_t off = 0; off < 0x4D0; off += 8) {
        const uintptr_t v = gq(INV + off); if (!v) continue;
        if (all.count(v)) { s.insert(v); continue; }
        const uintptr_t w = gq(v + 0x140); if (w && all.count(w)) s.insert(w);
    }
    return s;
}
uint32_t parse_id(const json& a) {   // "97" or "97: Primary_..." or a number
    if (a.contains("id") && a["id"].is_number()) return (uint32_t)a["id"].get<int64_t>();
    const std::string s = a.value("id", std::string()); return s.empty() ? 0xFFFFFFFF : (uint32_t)strtoul(s.c_str(), nullptr, 10);
}
bool find_gear(uint32_t id, Gear& g, uintptr_t& INV, std::string& err) {
    if (id == 0xFFFFFFFF) { err = "choose an item first"; return false; }
    if (!player_inventory(INV, err)) return false;
    for (auto& x : gear_list(INV)) if (x.id == id) { g = x; return true; }
    err = "no item with id " + std::to_string(id) + " in your inventory"; return false;
}
uintptr_t affix_record(const std::string& name, std::string& err) {   // exact name in Inventory/Affix/Definition
    const uintptr_t desc = gq(game_base() + kTblAffix); const uint32_t n = gd(desc + 0x28); const uintptr_t ents = gq(desc + 0x38);
    if (!desc || !ents || n == 0 || n > 20000) { err = "effect table not loaded"; return 0; }
    for (uint32_t i = 0; i < n; ++i) { const uintptr_t e = ents + (uintptr_t)i * 0x28; if (rname(e) == name) return e; }
    err = "no effect named '" + name + "'"; return 0;
}
// item-type prefix of an item's own effects (e.g. "1Sword_"), used to sort the effect catalog
std::string type_prefix(const std::string& item) {
    for (const char* p : { "1Sword", "2Dagger", "3Bow", "4Armor", "5Cape", "6Ring" }) if (item.find(p) != std::string::npos) return std::string(p) + "_";
    return "";
}

constexpr uintptr_t kTblItems = 0x2700530, kTblWeaponLook = 0x27004F0, kTblArmorLook = 0x27004F8;
constexpr uintptr_t kHeapAlloc = 0x3DE290, kAffixAppend = 0xFB4E10, kAddItemFn = 0x1C01E0;
// every record name in a DB table (skips DEFAULT_RECORD*)
std::vector<std::pair<std::string, uintptr_t>> table_records(uintptr_t global) {
    std::vector<std::pair<std::string, uintptr_t>> out;
    const uintptr_t desc = gq(game_base() + global); const uint32_t n = gd(desc + 0x28); const uintptr_t ents = gq(desc + 0x38);
    if (!desc || !ents || n == 0 || n > 200000) return out;
    for (uint32_t i = 0; i < n; ++i) { const uintptr_t e = ents + (uintptr_t)i * 0x28; std::string nm = rname(e);
        if (nm != "?" && nm.rfind("DEFAULT_RECORD", 0) != 0) out.push_back({ nm, e }); }
    return out;
}
uintptr_t record_in(uintptr_t global, const std::string& name) {
    for (auto& r : table_records(global)) if (r.first == name) return r.second;
    return 0;
}
// what kind of item this is, for filtering looks / items
std::string kind_of(const std::string& item) {
    if (item.find("1Sword") != std::string::npos) return "sword";
    if (item.find("2Dagger") != std::string::npos) return "dagger";
    if (item.find("3Bow") != std::string::npos) return "bow";
    if (item.find("5Cape") != std::string::npos) return "cape";
    if (item.find("4Armor") != std::string::npos) return "armor";
    if (item.find("6Ring") != std::string::npos) return "ring";
    return "";
}
bool look_matches(const std::string& kind, const std::string& look) {
    if (look.find("_Test") != std::string::npos || look.find("_Menu") != std::string::npos) return false;   // the game's test tints, menu-preview models
    if (kind == "sword") return look.rfind("Player_1HS_", 0) == 0;
    if (kind == "dagger") return look.rfind("Player_1HD_", 0) == 0;
    if (kind == "bow") return look.rfind("Player_2HR_", 0) == 0;
    if (kind == "cape") return look.find("Cloak") != std::string::npos;
    if (kind == "armor") return look.find("_Armor") != std::string::npos;
    if (kind == "ring") return look.find("Ring") != std::string::npos;
    return false;
}

// Live refresh of a WORN item: the game's equip 0x40014C(inventory, equipment) returns early when the same item is
// already in its slot, so equip another owned item of the same kind first, then the edited one again - the exact
// path of re-equipping by hand (the script binding at 0x1BCA314 calls it the same way). Game thread only.
constexpr uintptr_t kEquip = 0x40014C;
// field of a DB record by name hash: entry +8 field map (16 B: hash, .., +12 count, +14 slot), +0x10 data, +0x18 owner
bool look_field(uintptr_t rec, uint32_t hash, uintptr_t& addr, int& cnt) {
    const uintptr_t own = gq(rec + 0x18), t = own ? gq(own + 0x20) : 0, m = gq(rec + 8), data = gq(rec + 0x10);
    if (!t || !m || !data) return false;
    const uint32_t nf = gd(t + 8); if (nf == 0 || nf > 400) return false;
    for (uint32_t i = 0; i < nf; ++i) {
        uint8_t b[16]; if (!safe_read(m + i * 16, b, 16)) return false;
        uint32_t h; memcpy(&h, b, 4); if (h != hash) continue;
        uint16_t c, sl; memcpy(&c, b + 12, 2); memcpy(&sl, b + 14, 2);
        addr = data + (uintptr_t)sl * 8; cnt = c; return true;
    }
    return false;
}
json refresh_worn(uintptr_t INV, const Gear& g) {
    if (!equipped(INV).count(g.eq)) return json{ {"refreshed", false}, {"why", "not equipped (shows when you equip it)"} };
    uintptr_t other = 0;
    for (auto& o : gear_list(INV)) if (o.slot == g.slot && o.eq != g.eq) { other = o.eq; break; }
    if (!other) return json{ {"refreshed", false}, {"why", "no other item of this kind to swap with - re-equip or reload to see it"} };
    json a = guarded_call(game_base() + kEquip, { INV, other }, "u64", true);
    if (!a.value("ok", false)) return json{ {"refreshed", false}, {"why", "equip swap failed: " + a.value("message", std::string())} };
    json b = guarded_call(game_base() + kEquip, { INV, g.eq }, "u64", true);
    if (!b.value("ok", false)) return json{ {"refreshed", false}, {"why", "re-equip failed: " + b.value("message", std::string())} };
    const bool back = equipped(INV).count(g.eq) > 0;
    json out{ {"refreshed", back}, {"why", back ? "re-equipped live" : "the game did not put the item back - equip it from the menu"} };
    // Looks carry a part the game adds on equip (AddItemOnEquip): weapon looks a scabbard, cloak looks the hood
    // (Talion_Hood_Right, tagged IHasNoHair) or the hair (Talion_Hair / Talion_Hair_Right). The game adds it to the
    // inventory on equip but leaves an already-worn part on (a hood cloak -> plain cloak swap left Talion bald), so
    // equip the part that belongs to the current look ourselves.
    if (back) {
        const uintptr_t look = gq(g.eq + 0x40) ? gq(g.eq + 0x40) : gq(g.eq + 0x48);
        uintptr_t fa = 0; int fc = 0;
        if (look && look_field(look, 0xA35DEABD, fa, fc) && fc == 1) {
            uintptr_t want = gq(fa);
            uintptr_t sc = 0;
            if (want) for (auto& o : gear_list(INV)) if (gq(o.eq + 0x38) == want) { sc = o.eq; break; }
            if (want && !sc) {                     // Talion_Hair not owned: its _Right twin is the same part
                const std::string wn = rname(want);
                for (auto& o : gear_list(INV)) { const std::string on = rname(gq(o.eq + 0x38)); if (on.size() > wn.size() && on.compare(0, wn.size(), wn) == 0 && on[wn.size()] == '_') { sc = o.eq; break; } }
            }
            if (sc && equipped(INV).count(sc)) { out["part"] = rname(gq(sc + 0x38)) + " (already worn)"; sc = 0; want = 0; }
            if (sc) {
                json c = guarded_call(game_base() + kEquip, { INV, sc }, "u64", true);
                out["part"] = c.value("ok", false) ? rname(gq(sc + 0x38)) : "equip failed";
            } else if (want) out["part"] = "not owned: " + rname(want);
        }
    }
    return out;
}

// ---- display names: bin\data\gear_names.json (built offline from the game's own English string table)
//   {"affix": {record: {"t": text template, "s": UIValueScalar}}, "look": {record: text}, "item": {record: text}}
const json& gear_names() {
    static json j; static bool loaded = false;
    if (!loaded) {
        loaded = true;
        try { std::ifstream f(g_cfg.root + "\\data\\gear_names.json"); if (f) j = json::parse(f); } catch (...) { j = json::object(); }
        if (!j.is_object()) j = json::object();
    }
    return j;
}
std::string name_in(const char* group, const std::string& rec) {
    const json& j = gear_names(); if (!j.contains(group) || !j[group].contains(rec)) return "";
    const json& v = j[group][rec]; return v.is_string() ? v.get<std::string>() : v.value("t", std::string());
}
// "{1!.0f}% increased damage" + 0.15 (scale 100) -> "15% increased damage"; value < 0 means "no value" -> "X"
std::string effect_text(const std::string& rec, double value, bool with_value, const char* ph = "X") {
    const json& j = gear_names();
    if (!j.contains("affix") || !j["affix"].contains(rec)) return "";
    const json& v = j["affix"][rec]; std::string t = v.value("t", std::string()); const double sc = v.value("s", 1.0);
    std::string out; size_t i = 0;
    while (i < t.size()) {
        if (t[i] == '{') {
            const size_t e = t.find('}', i);
            if (e != std::string::npos) {
                const std::string spec = t.substr(i + 1, e - i - 1);   // e.g. "1!.0f"
                if (!with_value) out += ph;
                else {
                    int prec = 0; const size_t dot = spec.find('.'); if (dot != std::string::npos && dot + 1 < spec.size() && isdigit((unsigned char)spec[dot + 1])) prec = spec[dot + 1] - '0';
                    char b[64]; snprintf(b, sizeof b, "%.*f", prec, value * sc); out += b;
                }
                i = e + 1; continue;
            }
        }
        out += t[i++];
    }
    return out;
}
// "Arcane Cloak" + AD_ElvenT4_Cloak_Tint_Mystic -> "Arcane Cloak (tier 4)"; Gondorian ..._T1_Cloak_Tint10_2Common -> "(tier 1, tint 10, common)"
std::string look_detail(const std::string& rec) {
    std::string out; int tier = -1, tint = -1;
    for (size_t i = 1; i + 1 < rec.size(); ++i) if (rec[i] == 'T' && isdigit((unsigned char)rec[i + 1]) && isalpha((unsigned char)rec[i - 1]) && (i + 2 >= rec.size() || !isdigit((unsigned char)rec[i + 2]))) { tier = rec[i + 1] - '0'; break; }
    const size_t tp = rec.find("_Tint"); if (tp != std::string::npos && tp + 5 < rec.size() && isdigit((unsigned char)rec[tp + 5])) tint = atoi(rec.c_str() + tp + 5);
    if (tier >= 0) out += "tier " + std::to_string(tier);
    if (tint >= 0) out += (out.empty() ? "" : ", ") + std::string("tint ") + std::to_string(tint);
    if (rec.find("_2Common") != std::string::npos) out += (out.empty() ? "" : ", ") + std::string("common");
    return out.empty() ? "" : " (" + out + ")";
}
// picker lines are "Display text  [Record]" - commands accept either that or the bare record name
std::string strip_label(const std::string& s) {
    const size_t e = s.rfind(']'), b = s.rfind('[');
    if (e != std::string::npos && b != std::string::npos && b < e && e == s.size() - 1) return s.substr(b + 1, e - b - 1);
    return s;
}
std::string labelled(const std::string& text, const std::string& rec) { return text.empty() ? rec : text + "  [" + rec + "]"; }

// ---- originals: bin\data\gear_originals.json = {"<id>": {item, level, look, look_armor, fx: [[group, affix, base, added]...]}}
// Snapshot taken the first time the trainer edits an item (by record NAME, so it survives restarts; ids are stable).
std::string originals_path() { return g_cfg.root + "\\data\\gear_originals.json"; }
json& originals() {
    static json j; static bool loaded = false;
    if (!loaded) { loaded = true; try { std::ifstream f(originals_path()); if (f) j = json::parse(f); } catch (...) {} if (!j.is_object()) j = json::object(); }
    return j;
}
void originals_save() { try { std::ofstream f(originals_path()); f << originals().dump(1); } catch (...) {} }
// the item as it is now, by record names: {item, level, look, look_armor, fx: [[group, affix, base, added]...]}
json capture(const Gear& g) {
    json fx = json::array();
    for (int grp = 0; grp < 2; ++grp) {
        const uintptr_t vec = g.eq + (grp ? 0x90 : 0x78), b = gq(vec), e = gq(vec + 8);
        if (!b || e < b || (e - b) % 8 || e - b > 0x400) continue;
        for (uintptr_t q = b; q < e; q += 8) { const uintptr_t af = gq(q); if (af) fx.push_back(json::array({ grp, rname(gq(af)), gf(af + 8), gf(af + 0xC) })); }
    }
    return { {"item", rname(gq(g.eq + 0x38))}, {"level", gd(g.eq + 0x24)},
             {"look", gq(g.eq + 0x40) ? rname(gq(g.eq + 0x40)) : ""}, {"look_armor", gq(g.eq + 0x48) ? rname(gq(g.eq + 0x48)) : ""}, {"fx", fx} };
}
void snapshot(const Gear& g) {
    const std::string key = std::to_string(g.id); json& o = originals();
    if (o.contains(key)) return;
    o[key] = capture(g);
    originals_save();
}
// write a captured state onto an item: look, core (item record + level), effects (exact values). Game thread.
// Returns "" or an error. `done` lists what changed.
std::string apply_state(const Gear& g, const json& st, bool look, bool core, bool effects, json& done) {
    std::string err;
    // review 2026-11-15: resolve every effect and allocate its memory FIRST, before look / core are written, so a
    // failure really leaves the item unchanged
    struct Prep { int grp; uintptr_t mem; };
    std::vector<Prep> prep; int unknown = 0;
    if (effects) {
        const json fxs = st.value("fx", json::array());
        for (auto& f : fxs) {
            const int grp = f[0].get<int>(); const std::string nm = f[1].get<std::string>();
            const uintptr_t def = affix_record(nm, err); if (!def) { ++unknown; continue; }
            json r = guarded_call(game_base() + kHeapAlloc, { 0, 0x10 }, "u64", true); if (!r.value("ok", false)) return "allocator call failed (item unchanged)";
            const uintptr_t mem = (uintptr_t)(uint64_t)r["data"]["ret_i"].get<int64_t>(); if (!mem) return "the game's allocator returned nothing (item unchanged)";
            struct { uintptr_t def; float base, added; } fx = { def, f[2].get<float>(), f[3].get<float>() };
            safe_write(mem, &fx, 16);
            prep.push_back({ grp, mem });
        }
        if (!fxs.empty() && prep.empty()) return "none of the saved effects exist in this game (item unchanged)";
    }
    // review 2026-11-15: snapshot what this call may change, so a failed effect append can put the item back
    uintptr_t old_lw = gq(g.eq + 0x40), old_la = gq(g.eq + 0x48), old_rec = gq(g.eq + 0x38); uint32_t old_lv = gd(g.eq + 0x24);
    std::vector<uintptr_t> old_fx[2];
    for (int grp = 0; grp < 2; ++grp) {
        const uintptr_t vec = g.eq + (grp ? 0x90 : 0x78), b = gq(vec), e = gq(vec + 8);
        const bool sane = b && e >= b && (e - b) % 8 == 0 && e - b <= 0x400;
        if (sane) for (uintptr_t q = b; q < e; q += 8) old_fx[grp].push_back(gq(q));
        else if (effects && b) return "this item's effect list looks unusual - not edited (item unchanged)";   // rollback could not restore it
    }
    auto append_fx = [&](int grp, uintptr_t mem) -> bool {
        static uintptr_t s_slot; s_slot = mem;
        return guarded_call(game_base() + kAffixAppend, { g.eq + (grp ? 0x90 : 0x78), 0, (uint64_t)&s_slot }, "u64", true).value("ok", false);
    };
    if (look) {
        const std::string lw = st.value("look", std::string()), la = st.value("look_armor", std::string());
        if (!lw.empty()) { const uintptr_t r = record_in(kTblWeaponLook, lw); if (r) { safe_write(g.eq + 0x40, &r, 8); done.push_back("look"); } }
        if (!la.empty()) { const uintptr_t r = record_in(kTblArmorLook, la); if (r) { safe_write(g.eq + 0x48, &r, 8); done.push_back("look"); } }
    }
    if (core) {
        const uintptr_t r = record_in(kTblItems, st.value("item", std::string()));
        if (r) { safe_write(g.eq + 0x38, &r, 8); done.push_back("rarity/type"); }
        const uint32_t lv = st.value("level", (uint32_t)0); if (lv) { safe_write(g.eq + 0x24, &lv, 4); done.push_back("level"); }
    }
    if (effects) {
        // (effects were resolved and allocated above) empty both lists (the old 16-byte effects are left alone, never freed), then rebuild
        for (uintptr_t off : { 0x78, 0x90 }) { const uintptr_t vec = g.eq + off, b = gq(vec); if (b) safe_write(vec + 8, &b, 8); }
        int n = 0;
        for (auto& p : prep) {
            if (!append_fx(p.grp, p.mem)) {
                // put the item back as it was: look, record, level and the original effect list
                safe_write(g.eq + 0x40, &old_lw, 8); safe_write(g.eq + 0x48, &old_la, 8); safe_write(g.eq + 0x38, &old_rec, 8); safe_write(g.eq + 0x24, &old_lv, 4);
                for (uintptr_t off : { 0x78, 0x90 }) { const uintptr_t vec = g.eq + off, b = gq(vec); if (b) safe_write(vec + 8, &b, 8); }
                int lost = 0;
                for (int grp = 0; grp < 2; ++grp) for (uintptr_t m : old_fx[grp]) if (m && !append_fx(grp, m)) ++lost;
                done = json::array();
                if (lost) return "effect append failed and " + std::to_string(lost) + " original effect(s) could not be put back - use Reset Effects";
                return "effect append failed (item put back as it was)";
            }
            ++n;
        }
        if (unknown) done.push_back(std::to_string(unknown) + " unknown effect(s) skipped");
        done.push_back("effects (" + std::to_string(n) + ")");
    }
    return "";
}
} // namespace

// SOW.GEAR_OPTIONS -> {text: "id: Item (Slot, lvl N) [equipped]\n..."}
json sow_cmd_gear_options(const json&) {
    return run_on_main([]() -> json {
        uintptr_t INV; std::string err; if (!player_inventory(INV, err)) return fail("NO_PLAYER", err);
        const auto eqd = equipped(INV); std::string t; int n = 0;
        for (auto& g : gear_list(INV)) {
            if (g.slot >= 13 && g.slot != 22 && affixes(g.eq).empty()) continue;   // cosmetic parts (hands, scabbards, hood)
            const std::string rec = rname(gq(g.eq + 0x38));
            std::string shown = name_in("item", rec); { const uintptr_t lk = gq(g.eq + 0x40) ? gq(g.eq + 0x40) : gq(g.eq + 0x48); const std::string ln = lk ? name_in("look", rname(lk)) : "";
                if (!ln.empty() && shown.find(ln) == std::string::npos) shown = ln + (shown.empty() ? "" : " - " + shown); }
            if (shown.empty()) shown = rec;
            t += std::to_string(g.id) + ": " + shown + " (" + kSlotName[g.slot] + ", lvl " + std::to_string(gd(g.eq + 0x24)) + ")"
               + (eqd.count(g.eq) ? " [equipped]" : "") + "\n"; ++n;
        }
        return ok({ {"text", t}, {"count", n} });
    }, 4000);
}

// SOW.GEAR_CARD {id} -> {text, level, effects[]}
json sow_cmd_gear_card(const json& a) {
    const uint32_t id = parse_id(a);
    return run_on_main([id]() -> json {
        Gear g; uintptr_t INV; std::string err; if (!find_gear(id, g, INV, err)) return fail("NO_ITEM", err);
        const bool eqd = equipped(INV).count(g.eq) > 0;
        const std::string irec = rname(gq(g.eq + 0x38)), iname = name_in("item", irec);
        json fx = json::array(); std::string t = (iname.empty() ? irec : iname) + "  |  " + kSlotName[g.slot] + ", level " + std::to_string(gd(g.eq + 0x24))
            + (eqd ? ", EQUIPPED" : "") + "\n";
        { const uintptr_t w = gq(g.eq + 0x40), ar = gq(g.eq + 0x48);
          if (w || ar) { const std::string lr = rname(w ? w : ar), ln = name_in("look", lr); t += "Look: " + (ln.empty() ? lr : ln + "  [" + lr + "]") + "\n"; } }
        int i = 0;
        for (uintptr_t af : affixes(g.eq)) {
            const std::string n = rname(gq(af)); const float v = gf(af + 8), ad = gf(af + 0xC);
            const std::string shown = effect_text(n, v + ad, (v + ad) != 0, "?");   // 0 = set bonus etc.: the game computes that number
            char line[512];
            if (!shown.empty()) snprintf(line, sizeof line, "#%d  %s\n", i, shown.c_str());
            else snprintf(line, sizeof line, "#%d  %s = %g%s\n", i, n.c_str(), v, ad != 0 ? (" (+" + std::to_string(ad) + ")").c_str() : "");
            t += line; fx.push_back({ {"index", i}, {"affix", n}, {"text", shown}, {"value", v}, {"added", ad} }); ++i;
        }
        if (i == 0) t += "(no effects)\n";
        return ok({ {"text", t}, {"level", (int)gd(g.eq + 0x24)}, {"effects", fx}, {"equipped", eqd}, {"item", rname(gq(g.eq + 0x38))} });
    }, 4000);
}

// SOW.GEAR_LEVEL {id, level} - item level (saved). Values are re-derived from level on the next load.
json sow_cmd_gear_level(const json& a) {
    const uint32_t id = parse_id(a); const int64_t lv = a.value("level", (int64_t)0);
    if (lv < 1 || lv > 120) return fail("BAD_ARGS", "level 1..120");
    return run_on_main([id, lv]() -> json {
        Gear g; uintptr_t INV; std::string err; if (!find_gear(id, g, INV, err)) return fail("NO_ITEM", err); snapshot(g);
        const uint32_t before = gd(g.eq + 0x24), v = (uint32_t)lv;
        if (!safe_write(g.eq + 0x24, &v, 4)) return fail("WRITE", "could not write the level");
        return ok({ {"before", before}, {"after", gd(g.eq + 0x24)},
                    {"note", "saved with the game; stats are recalculated from the new level after you save and reload"} });
    }, 4000);
}

// SOW.GEAR_AFFIX_OPTIONS {id} -> {text} every Inventory/Affix/Definition, the item's own type first
json sow_cmd_gear_affix_options(const json& a) {
    const uint32_t id = parse_id(a);
    return run_on_main([id]() -> json {
        std::string pre;
        if (id != 0xFFFFFFFF) { Gear g; uintptr_t INV; std::string err; if (find_gear(id, g, INV, err)) pre = type_prefix(rname(gq(g.eq + 0x38))); }
        const uintptr_t desc = gq(game_base() + kTblAffix); const uint32_t n = gd(desc + 0x28); const uintptr_t ents = gq(desc + 0x38);
        if (!desc || !ents || n == 0 || n > 20000) return fail("NO_TABLE", "effect table not loaded");
        std::vector<std::string> mine, other;
        for (uint32_t i = 0; i < n; ++i) {
            const std::string nm = rname(ents + (uintptr_t)i * 0x28); if (nm == "?" || nm.rfind("DEFAULT_RECORD", 0) == 0) continue;
            (!pre.empty() && nm.rfind(pre, 0) == 0 ? mine : other).push_back(labelled(effect_text(nm, 0, false), nm));
        }
        std::sort(mine.begin(), mine.end()); std::sort(other.begin(), other.end());
        std::string t; for (auto& s : mine) t += s + "\n"; for (auto& s : other) t += s + "\n";
        return ok({ {"text", t}, {"count", mine.size() + other.size()}, {"same_type", mine.size()} });
    }, 6000);
}

// SOW.GEAR_AFFIX {id, index, affix} - replace effect #index with another effect definition (saved)
json sow_cmd_gear_affix(const json& a) {
    const uint32_t id = parse_id(a); const int idx = (int)a.value("index", (int64_t)-1); const std::string want = strip_label(a.value("affix", std::string()));
    if (idx < 0 || want.empty()) return fail("BAD_ARGS", "index and affix are required");
    return run_on_main([id, idx, want]() -> json {
        Gear g; uintptr_t INV; std::string err; if (!find_gear(id, g, INV, err)) return fail("NO_ITEM", err); snapshot(g);
        const auto fx = affixes(g.eq); if (idx >= (int)fx.size()) return fail("BAD_ARGS", "this item has " + std::to_string(fx.size()) + " effects (#0..#" + std::to_string((int)fx.size() - 1) + ")");
        const uintptr_t def = affix_record(want, err); if (!def) return fail("NO_RECORD", err);
        const std::string before = rname(gq(fx[idx]));
        if (!safe_write(fx[idx], &def, 8)) return fail("WRITE", "could not write the effect");
        return ok({ {"before", before}, {"after", rname(gq(fx[idx]))},
                    {"note", "saved with the game; its value is derived from the item level after you save and reload (the old number stays until then)"} });
    }, 4000);
}

// The effects list shows values times the effect's display scale (gear_names.json "s": 100 for percent effects, which
// the game stores as fractions: 25% = 0.25). The Value box uses the same units as the list (user report 2026-11-28:
// typing 50 for 50% gave 5000%).
static double affix_scale(const std::string& rec) {
    const json& j = gear_names();
    if (!j.contains("affix") || !j["affix"].contains(rec)) return 1.0;
    const double sc = j["affix"][rec].value("s", 1.0);
    return (std::isfinite(sc) && sc != 0.0) ? sc : 1.0;
}
// SOW.GEAR_VALUE {id, index, value} - effect value, LIVE ONLY (the game re-derives values on load).
// value is in the units the effects list shows (50 = 50%); raw:true writes the stored number as it is.
json sow_cmd_gear_value(const json& a) {
    const uint32_t id = parse_id(a); const int idx = (int)a.value("index", (int64_t)-1); const double val = a.value("value", 0.0);
    const bool raw = a.value("raw", false);
    if (idx < 0) return fail("BAD_ARGS", "check an effect in the list first");
    if (!std::isfinite(val)) return fail("BAD_ARGS", "a finite value is required");
    return run_on_main([id, idx, val, raw]() -> json {
        Gear g; uintptr_t INV; std::string err; if (!find_gear(id, g, INV, err)) return fail("NO_ITEM", err); snapshot(g);
        const auto fx = affixes(g.eq); if (idx >= (int)fx.size()) return fail("BAD_ARGS", "no effect #" + std::to_string(idx));
        const std::string rec = rname(gq(fx[idx]));
        const double sc = raw ? 1.0 : affix_scale(rec);
        const float before = gf(fx[idx] + 8), v = (float)(val / sc);
        if (!safe_write(fx[idx] + 8, &v, 4)) return fail("WRITE", "could not write the value");
        const float after = gf(fx[idx] + 8);
        return ok({ {"effect", rec}, {"scale", sc}, {"before", before * sc}, {"after", after * sc}, {"stored", after},
                    {"text", effect_text(rec, after, true)},
                    {"note", "live only: the game recalculates values when it loads a save"} });
    }, 4000);
}

// SOW.GEAR_ADD_AFFIX {id, affix} - add an extra effect (saved). The 16-byte effect is allocated with the game's own
// heap allocator (0x3DE290, as the save loader does) and appended with the game's own list append (0xFB4E10 ->
// 0x64A018, which takes ownership and grows the list). Its value comes from the item level on the next load; until
// then it copies the value of the same effect on another item of the same level, else 0.
json sow_cmd_gear_add_affix(const json& a) {
    // {replace: n} (n >= 0, the panel's checked effect) = replace effect #n instead of adding one
    if (a.contains("replace") && a["replace"].is_number() && a["replace"].get<int64_t>() >= 0) {
        json b = a; b["index"] = a["replace"]; json r = sow_cmd_gear_affix(b);
        if (r.value("ok", false)) r["data"]["replaced"] = a["replace"];
        return r;
    }
    const uint32_t id = parse_id(a); const std::string want = strip_label(a.value("affix", std::string()));
    if (want.empty()) return fail("BAD_ARGS", "affix is required");
    return run_on_main([id, want]() -> json {
        Gear g; uintptr_t INV; std::string err; if (!find_gear(id, g, INV, err)) return fail("NO_ITEM", err); snapshot(g);
        if (affixes(g.eq).size() >= 12) return fail("LIMIT", "this item already has 12 effects");
        const uintptr_t def = affix_record(want, err); if (!def) return fail("NO_RECORD", err);
        float val = 0; const uint32_t lvl = gd(g.eq + 0x24);
        for (auto& o : gear_list(INV)) {
            if (gd(o.eq + 0x24) != lvl) continue;
            for (uintptr_t af : affixes(o.eq)) if (gq(af) == def) { val = gf(af + 8); break; }
            if (val != 0) break;
        }
        json r = guarded_call(game_base() + kHeapAlloc, { 0, 0x10 }, "u64", true);
        if (!r.value("ok", false)) return r;
        const uintptr_t mem = (uintptr_t)(uint64_t)r["data"]["ret_i"].get<int64_t>();
        if (!mem) return fail("ALLOC", "the game's allocator returned nothing");
        struct { uintptr_t def; float base, added; } fx = { def, val, 0.0f };
        if (!safe_write(mem, &fx, 16)) return fail("WRITE", "could not fill the new effect");
        static uintptr_t s_slot; s_slot = mem;
        const uintptr_t vec = g.eq + 0x78, endBefore = gq(vec + 8);
        json ap = guarded_call(game_base() + kAffixAppend, { vec, 0, (uint64_t)&s_slot }, "u64", true);
        if (!ap.value("ok", false)) return ap;
        const uintptr_t endAfter = gq(vec + 8);
        if (s_slot != 0 || endAfter == endBefore || gq(endAfter - 8) != mem)
            return fail("APPEND", "the game did not take the new effect (left unused)");
        return ok({ {"added", want}, {"value", val}, {"effects", affixes(g.eq).size()},
                    {"note", val != 0 ? "saved with the game" : "saved with the game; its value appears after the next load (derived from the item level)"} });
    }, 5000);
}

// SOW.GEAR_REMOVE_AFFIX {id, index} - take effect #index off the item (saved). The 16 bytes are not freed (a tiny
// leak is safer than freeing through the wrong path).
json sow_cmd_gear_remove_affix(const json& a) {
    const uint32_t id = parse_id(a); const int idx = (int)a.value("index", (int64_t)-1);
    if (idx < 0) return fail("BAD_ARGS", "index is required");
    return run_on_main([id, idx]() -> json {
        Gear g; uintptr_t INV; std::string err; if (!find_gear(id, g, INV, err)) return fail("NO_ITEM", err); snapshot(g);
        int k = idx;
        for (uintptr_t off : { 0x78, 0x90 }) {
            const uintptr_t vec = g.eq + off, b = gq(vec), e = gq(vec + 8);
            if (!b || e < b || (e - b) % 8 || e - b > 0x400) continue;
            const int n = (int)((e - b) / 8);
            if (k >= n) { k -= n; continue; }
            const std::string gone = rname(gq(gq(b + 8 * k)));
            for (int j = k; j < n - 1; ++j) { const uintptr_t nx = gq(b + 8 * (j + 1)); safe_write(b + 8 * j, &nx, 8); }
            const uintptr_t zero = 0, ne = e - 8; safe_write(b + 8 * (n - 1), &zero, 8); safe_write(vec + 8, &ne, 8);
            return ok({ {"removed", gone}, {"effects", affixes(g.eq).size()} });
        }
        return fail("BAD_ARGS", "no effect #" + std::to_string(idx));
    }, 4000);
}

// SOW.GEAR_LOOK_OPTIONS {id} -> {text} appearances for this kind of item (transmog)
json sow_cmd_gear_look_options(const json& a) {
    const uint32_t id = parse_id(a);
    return run_on_main([id]() -> json {
        Gear g; uintptr_t INV; std::string err; if (!find_gear(id, g, INV, err)) return fail("NO_ITEM", err);
        const std::string kind = kind_of(rname(gq(g.eq + 0x38)));
        if (kind.empty()) return ok({ {"text", ""}, {"count", 0} });
        const bool weapon = kind == "sword" || kind == "dagger" || kind == "bow";
        std::vector<std::string> v;
        for (auto& r : table_records(weapon ? kTblWeaponLook : kTblArmorLook)) if (look_matches(kind, r.first)) { const std::string ln = name_in("look", r.first); v.push_back(labelled(ln.empty() ? "" : ln + look_detail(r.first), r.first)); }
        std::sort(v.begin(), v.end()); std::string t; for (auto& x : v) t += x + "\n";
        return ok({ {"text", t}, {"count", v.size()}, {"kind", kind} });
    }, 6000);
}

// SOW.GEAR_LOOK_PREVIEW {id, look} - put the look on the worn item live, to see it before APPLY. The item's own look
// is remembered: APPLY (SOW.GEAR_LOOK) keeps the preview, RESET LOOK or closing the panel puts the old look back.
namespace { struct { uintptr_t eq = 0, orig = 0, slot = 0; uint32_t id = 0; } g_pv; }
static void preview_end(bool revert) {
    if (!g_pv.eq) return;
    if (revert) {
        uintptr_t INV; std::string err; Gear g;
        if (find_gear(g_pv.id, g, INV, err) && g.eq == g_pv.eq) { safe_write(g_pv.slot, &g_pv.orig, 8); refresh_worn(INV, g); }
    }
    g_pv = {};
}
void gear_preview_tick() { if (g_pv.eq && !overlay_visible()) preview_end(true); }
json sow_cmd_gear_look_preview(const json& a) {
    const uint32_t id = parse_id(a); const std::string look = strip_label(a.value("look", std::string()));
    if (look.empty()) return fail("BAD_ARGS", "look is required");
    return run_on_main([id, look]() -> json {
        Gear g; uintptr_t INV; std::string err; if (!find_gear(id, g, INV, err)) return fail("NO_ITEM", err);
        if (g_pv.eq && g_pv.eq != g.eq) preview_end(true);
        if (!equipped(INV).count(g.eq)) return ok({ {"preview", false}, {"why", "equip the item to preview looks on Talion"} });
        const std::string kind = kind_of(rname(gq(g.eq + 0x38)));
        if (!look_matches(kind, look)) return fail("BAD_ARGS", "not a " + kind + " appearance");
        const bool weapon = kind == "sword" || kind == "dagger" || kind == "bow";
        const uintptr_t rec = record_in(weapon ? kTblWeaponLook : kTblArmorLook, look);
        if (!rec) return fail("NO_RECORD", "no appearance named '" + look + "'");
        const uintptr_t slot = g.eq + (weapon ? 0x40 : 0x48);
        if (!g_pv.eq) { snapshot(g); g_pv.eq = g.eq; g_pv.id = id; g_pv.slot = slot; g_pv.orig = gq(slot); }
        if (gq(slot) == rec) return ok({ {"preview", true}, {"look", look} });
        if (!safe_write(slot, &rec, 8)) return fail("WRITE", "could not write the appearance");
        return ok({ {"preview", true}, {"look", look}, {"live", refresh_worn(INV, g)} });
    }, 6000);
}

// SOW.GEAR_LOOK {id, look} - transmog: another appearance of the same kind (saved). Shows after re-equip / reload.
json sow_cmd_gear_look(const json& a) {
    const uint32_t id = parse_id(a); const std::string look = strip_label(a.value("look", std::string())); const bool live = a.value("live", true);
    if (look.empty()) return fail("BAD_ARGS", "look is required");
    return run_on_main([id, look, live]() -> json {
        Gear g; uintptr_t INV; std::string err; if (!find_gear(id, g, INV, err)) return fail("NO_ITEM", err); snapshot(g);
        const std::string kind = kind_of(rname(gq(g.eq + 0x38)));
        if (!look_matches(kind, look)) return fail("BAD_ARGS", "'" + look + "' is not a " + (kind.empty() ? std::string("matching") : kind) + " appearance");
        const bool weapon = kind == "sword" || kind == "dagger" || kind == "bow";
        const uintptr_t rec = record_in(weapon ? kTblWeaponLook : kTblArmorLook, look);
        if (!rec) return fail("NO_RECORD", "no appearance named '" + look + "'");
        const uintptr_t slot = g.eq + (weapon ? 0x40 : 0x48); const std::string before = rname(gq(slot));
        if (g_pv.eq == g.eq) g_pv = {};               // a preview of this item becomes the real look
        if (!safe_write(slot, &rec, 8)) return fail("WRITE", "could not write the appearance");
        json out = { {"before", before}, {"after", rname(gq(slot))}, {"note", "saved with the game"} };
        if (live) out["live"] = refresh_worn(INV, g);
        return ok(out);
    }, 6000);
}

// SOW.GEAR_ITEM_OPTIONS {id} -> {text} item records of the same kind (rarity / set / variant)
json sow_cmd_gear_item_options(const json& a) {
    const uint32_t id = parse_id(a);
    return run_on_main([id]() -> json {
        Gear g; uintptr_t INV; std::string err; if (!find_gear(id, g, INV, err)) return fail("NO_ITEM", err);
        const std::string cur = rname(gq(g.eq + 0x38)), kind = kind_of(cur);
        if (kind.empty()) return ok({ {"text", ""}, {"count", 0} });
        std::vector<std::string> v;
        for (auto& r : table_records(kTblItems)) {
            const std::string& n = r.first;
            if (kind_of(n) != kind) continue;
            if (n.rfind("Primary_", 0) != 0 && n.rfind("PrimHam_", 0) != 0 && n.rfind("PrimSnipe_", 0) != 0) continue;
            if (n.find("DebugGrant") != std::string::npos) continue;
            v.push_back(labelled(name_in("item", n), n));
        }
        std::sort(v.begin(), v.end()); std::string t; for (auto& x : v) t += x + "\n";
        return ok({ {"text", t}, {"count", v.size()}, {"current", cur} });
    }, 8000);
}

// SOW.GEAR_ITEM {id, item} - change the item record: rarity, set, name, variant (saved). Effects stay; edit them
// separately. Shows after re-equip / reload.
json sow_cmd_gear_item(const json& a) {
    const uint32_t id = parse_id(a); const std::string item = strip_label(a.value("item", std::string()));
    if (item.empty()) return fail("BAD_ARGS", "item is required");
    return run_on_main([id, item]() -> json {
        Gear g; uintptr_t INV; std::string err; if (!find_gear(id, g, INV, err)) return fail("NO_ITEM", err); snapshot(g);
        const std::string cur = rname(gq(g.eq + 0x38));
        if (kind_of(item) != kind_of(cur) || kind_of(cur).empty()) return fail("BAD_ARGS", "only items of the same kind (" + kind_of(cur) + ")");
        const uintptr_t rec = record_in(kTblItems, item); if (!rec) return fail("NO_RECORD", "no item named '" + item + "'");
        if (!safe_write(g.eq + 0x38, &rec, 8)) return fail("WRITE", "could not write the item");
        return ok({ {"before", cur}, {"after", rname(gq(g.eq + 0x38))},
                    {"note", "saved with the game; name, rarity and set change after re-equipping or reloading. Effects are unchanged - edit them separately."} });
    }, 8000);
}


// SOW.GEAR_REFRESH {id} - re-equip a worn item live so look / rarity / effect edits show right away
json sow_cmd_gear_refresh(const json& a) {
    const uint32_t id = parse_id(a);
    return run_on_main([id]() -> json {
        Gear g; uintptr_t INV; std::string err; if (!find_gear(id, g, INV, err)) return fail("NO_ITEM", err);
        json r = refresh_worn(INV, g);
        return r.value("refreshed", false) ? ok(r) : fail("NOT_REFRESHED", r.value("why", std::string("not refreshed")));
    }, 5000);
}


// SOW.GEAR_RESET {id, what: "look" | "effects" | "all"} - back to the item as it was before the trainer first edited it
json sow_cmd_gear_reset(const json& a) {
    const uint32_t id = parse_id(a); const std::string what = a.value("what", std::string("all"));
    if (what != "look" && what != "effects" && what != "all") return fail("BAD_ARGS", "what = look, effects or all");
    return run_on_main([id, what]() -> json {
        Gear g; uintptr_t INV; std::string err; if (!find_gear(id, g, INV, err)) return fail("NO_ITEM", err);
        const std::string key = std::to_string(id); json& o = originals();
        if (!o.contains(key)) return fail("NO_ORIGINAL", "this item was not edited by the trainer since resets were added - nothing to restore");
        const json orig = o[key]; json done = json::array();
        if (g_pv.eq == g.eq) g_pv = {};
        const std::string e2 = apply_state(g, orig, what != "effects", what == "all", what != "look", done);
        if (!e2.empty()) return fail("RESET", e2);
        json out = { {"restored", done} };
        out["live"] = refresh_worn(INV, g);
        if (what == "all") { o.erase(key); originals_save(); }   // fully back to normal: next edit takes a fresh snapshot
        return ok(out);
    }, 8000);
}

// ================================ No summon cooldown ================================
// Player summon cooldowns are Combat/Cooldown records (table exe+0x2700848): PLBar_Summon_Faction_Slot_0..2,
// PC_Summon_Faction (followers / bodyguards), PC_Summon_GP_Buddy, PC_Summon_Mount(_Troll/_FellBeast), PC_WargHowl_Summon_1..3.
// MinTime / MaxTime are CombatStatValueOption {Formula, Variable, RawValue}; the time comes from the formula
// FollowerCoolDown. With the Formula cleared the game uses RawValue (0), so the next cooldown is 0 s. Originals are
// kept and written back on off / reload.
namespace {
constexpr uintptr_t kTblCooldown = 0x2700848;
constexpr uint32_t kCdMin = 0xE3B6584F, kCdMax = 0x120223E1, kSvFormula = 0xAEE96F46, kSvRaw = 0x0A9F2D31;
std::vector<std::pair<uintptr_t, uint64_t>> g_cd;     // (field address, original 8 bytes)
bool is_player_summon_cd(const std::string& n) {
    return n.rfind("PLBar_Summon_", 0) == 0 || n.rfind("PC_Summon_", 0) == 0 || n.rfind("PC_WargHowl_Summon", 0) == 0;
}
void cd_set(uintptr_t a, uint64_t v) {
    for (auto& x : g_cd) if (x.first == a) { safe_write(a, &v, 8); return; }
    g_cd.push_back({ a, gq(a) }); safe_write(a, &v, 8);
}
void cd_restore() { for (auto& x : g_cd) safe_write(x.first, &x.second, 8); g_cd.clear(); }
}
// SOW.SUMMON_NOCD {on}
json sow_cmd_summon_nocd(const json& a) {
    const bool on = a.value("on", true);
    return run_on_main([on]() -> json {
        if (!on) { const size_t n = g_cd.size(); cd_restore(); return ok({ {"on", false}, {"restored", n} }); }
        json done = json::array();
        for (auto& r : table_records(kTblCooldown)) {
            if (!is_player_summon_cd(r.first)) continue;
            int hit = 0;
            for (uint32_t h : { kCdMin, kCdMax }) {
                uintptr_t fa; int fc; if (!look_field(r.second, h, fa, fc) || fc != 1) continue;
                const uintptr_t sv = gq(fa); if (!sv) continue;
                uintptr_t fo, ra; int c1, c2;
                if (!look_field(sv, kSvFormula, fo, c1) || !look_field(sv, kSvRaw, ra, c2) || c1 != 1 || c2 != 1) continue;
                cd_set(fo, 0); cd_set(ra, 0); ++hit;
            }
            if (hit) done.push_back(r.first);
        }
        if (done.empty()) return fail("NOT_FOUND", "no summon cooldown records found (game updated?)");
        return ok({ {"on", true}, {"cooldowns", done} });
    }, 8000);
}

// ================================ Loot drops ================================
// The game picks a captain's gear drop at exe+0x6483FC: call 0x579F90(&tmp, picker, 0) -> a Faction/Loot/NemesisGear
// record (SeiKur0 CT "Epic/Legendary Uruks drop specific item", found again in this build by its byte pattern,
// sig lootPick). We redirect that one call (rel32 -> a stub next to the exe -> hk_loot_pick): the original runs,
// and when it returned NemesisGear and the roll passes, our chosen record is returned instead. The game then builds
// and drops that gear itself. A "template" (a saved edited item) is copied onto the dropped item when it lands in
// the inventory (main tick: a new item whose record is the dropped gear's Item).
namespace {
constexpr uintptr_t kTblNemGear = 0x2701FC0;
constexpr uint32_t kNgItem = 0xA15E0193, kNgAffix = 0xB340EDF2;
std::atomic<bool> L_on{ false };
std::atomic<uintptr_t> L_rec{ 0 }, L_owner{ 0 };
std::atomic<int> L_chance{ 100 }, L_seen{ 0 }, L_hits{ 0 }, L_pending{ 0 };
std::atomic<uint64_t> L_since{ 0 };
uintptr_t L_site = 0, L_orig = 0; uint8_t* L_stub = nullptr; bool L_tick = false;
std::string L_tmpl, L_last; json L_tmpl_state; std::set<uint32_t> L_known;   // game thread only
uint64_t L_rng = 0x9E3779B97F4A7C15ull;
using pick_t = uintptr_t(__fastcall*)(uintptr_t, uintptr_t, uintptr_t);
int roll100() { L_rng ^= L_rng << 13; L_rng ^= L_rng >> 7; L_rng ^= L_rng << 17; return (int)(L_rng % 100); }

std::atomic<int> L_calls{ 0 }; uintptr_t L_ring[8][2] = {}; std::atomic<int> L_ri{ 0 };   // last picks: (picker, result)
uintptr_t __fastcall hk_loot_pick(uintptr_t a, uintptr_t b, uintptr_t c) {
    uintptr_t r = ((pick_t)L_orig)(a, b, c);
    { const int i = L_ri++ & 7; L_ring[i][0] = b; L_ring[i][1] = r; ++L_calls; }
    if (L_on && r && L_rec && L_owner) {
        uintptr_t own = 0; safe_read(r + 0x18, &own, 8);
        if (own == L_owner) {
            ++L_seen;
            if (roll100() < L_chance) { r = L_rec; ++L_hits; ++L_pending; L_since = GetTickCount64(); }
        }
    }
    return r;
}
uint8_t* alloc_near(uintptr_t at) {
    SYSTEM_INFO si; GetSystemInfo(&si); const uintptr_t gran = si.dwAllocationGranularity, base = at & ~(gran - 1);
    for (uintptr_t d = gran; d < 0x70000000; d += gran)
        for (int s = 0; s < 2; ++s) {
            const uintptr_t p = s ? base + d : base - d;
            if (void* m = VirtualAlloc((void*)p, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE)) return (uint8_t*)m;
        }
    return nullptr;
}
void put_rel(uintptr_t site, uintptr_t target) {
    const int32_t rel = (int32_t)((intptr_t)target - (intptr_t)(site + 5)); DWORD old;
    VirtualProtect((void*)(site + 1), 4, PAGE_EXECUTE_READWRITE, &old); memcpy((void*)(site + 1), &rel, 4);
    VirtualProtect((void*)(site + 1), 4, old, &old); FlushInstructionCache(GetCurrentProcess(), (void*)site, 5);
}
uintptr_t rec_ref(uintptr_t rec, uint32_t hash) { uintptr_t a; int c; return look_field(rec, hash, a, c) && c == 1 ? gq(a) : 0; }
std::string rarity_of(const std::string& item, const std::string& ng) {
    if (item.find("5Lgnd") != std::string::npos || ng.find("5Lgnd") != std::string::npos) return "Legendary";
    if (item.find("4Epic") != std::string::npos || ng.find("_4E") != std::string::npos) return "Epic";
    if (item.find("3Rare") != std::string::npos || ng.find("_3R") != std::string::npos) return "Rare";
    return "Other";
}
std::string kind_label(const std::string& k) {
    if (k == "sword") return "Sword"; if (k == "dagger") return "Dagger"; if (k == "bow") return "Bow";
    if (k == "armor") return "Armor"; if (k == "cape") return "Cloak"; if (k == "ring") return "Ring"; return "Other";
}
std::string templates_path() { return g_cfg.root + "\\data\\loot_templates.json"; }
json& templates() {
    static json j; static bool loaded = false;
    if (!loaded) { loaded = true; try { std::ifstream f(templates_path()); if (f) j = json::parse(f); } catch (...) {} if (!j.is_object()) j = json::object(); }
    return j;
}
void templates_save() { try { std::ofstream f(templates_path()); f << templates().dump(1); } catch (...) {} }
constexpr const char* kNoTemplate = "(none - the normal item)";
// Items.MaxQuantity: 48 legendary pieces are 1 (own one at most) - a second copy is thrown away on pickup. While custom
// drops are on, the dropped item (and the template's) get 0 = unlimited. In-memory DB value, restored when off.
constexpr uint32_t kItemMaxQty = 0x8CD69037;
std::vector<std::pair<uintptr_t, int32_t>> L_maxq;   // field address, original
void unlimit(uintptr_t item) {
    uintptr_t a; int c; if (!item || !look_field(item, kItemMaxQty, a, c) || c != 1) return;
    for (auto& m : L_maxq) if (m.first == a) { const int32_t z = 0; safe_write(a, &z, 4); return; }
    const int32_t orig = (int32_t)gd(a); if (orig == 0) return;
    L_maxq.push_back({ a, orig }); const int32_t z = 0; safe_write(a, &z, 4);
}
bool all_unlimited = false;   // not L_maxq.empty(): Give / Copy may have unlimited one item already
void relimit() { for (auto& m : L_maxq) safe_write(m.first, &m.second, 4); L_maxq.clear(); all_unlimited = false; }

void loot_tick() {
    static uint64_t last = 0; const uint64_t now = GetTickCount64(); if (now - last < 500) return; last = now;
    gear_preview_tick();
    if (!L_on && L_pending <= 0) return;
    uintptr_t INV; std::string err; if (!player_inventory(INV, err)) return;
    if (L_pending > 0 && now - L_since > 600000) { L_pending = 0; L_last = "a custom drop was not picked up within 10 minutes"; }
    const auto list = gear_list(INV);
    if (L_known.empty()) { for (auto& g : list) L_known.insert(g.id); return; }
    const uintptr_t want = L_rec ? rec_ref(L_rec, kNgItem) : 0;
    for (auto& g : list) {
        if (L_known.count(g.id)) continue;
        L_known.insert(g.id);
        if (L_pending <= 0 || !want || gq(g.eq + 0x38) != want) continue;
        --L_pending;
        if (L_tmpl_state.is_object()) {
            json done = json::array(); const std::string e = apply_state(g, L_tmpl_state, true, true, true, done);
            L_last = e.empty() ? "picked up item " + std::to_string(g.id) + " - template '" + L_tmpl + "' applied" : "picked up item " + std::to_string(g.id) + " - template failed: " + e;
            toast(e.empty() ? "Custom drop: '" + L_tmpl + "' applied" : "Custom drop: template failed");
        } else L_last = "picked up item " + std::to_string(g.id);
    }
}
bool loot_install(std::string& err) {
    if (L_stub) return true;
    const uintptr_t site = sig_addr("lootPick");
    if (!site) { err = "the drop code was not found in this game version (sig lootPick)"; return false; }
    uint8_t op = 0; safe_read(site, &op, 1); if (op != 0xE8) { err = "unexpected code at the drop site"; return false; }
    int32_t rel = 0; safe_read(site + 1, &rel, 4);
    uintptr_t tgt = site + 5 + (intptr_t)rel; size_t gsz = 0; const uintptr_t gb = game_base(&gsz);
    if (tgt < gb || tgt >= gb + gsz) {   // left redirected by an older bridge copy: its stub keeps the original at +14
        uintptr_t o = 0; safe_read(tgt + 14, &o, 8);
        if (o < gb || o >= gb + gsz) { err = "the drop call is redirected by something else"; return false; }
        tgt = o;
    }
    uint8_t* stub = alloc_near(site); if (!stub) { err = "no memory near the game code for the drop hook"; return false; }
    const uintptr_t hk = (uintptr_t)&hk_loot_pick;
    stub[0] = 0xFF; stub[1] = 0x25; memset(stub + 2, 0, 4); memcpy(stub + 6, &hk, 8); memcpy(stub + 14, &tgt, 8);
    FlushInstructionCache(GetCurrentProcess(), stub, 22);
    if (llabs((intptr_t)stub - (intptr_t)(site + 5)) > 0x7FFF0000LL) { err = "hook memory too far from the game code"; return false; }
    L_orig = tgt; L_site = site; L_stub = stub;
    put_rel(site, (uintptr_t)stub);
    if (!L_tick) { L_tick = true; add_main_tick(&loot_tick); }
    return true;
}
} // namespace


// SOW.LOOT_FILTER_OPTIONS {which: kind|rarity}
json sow_cmd_loot_filter_options(const json& a) {
    const std::string w = a.value("which", std::string("kind"));
    return ok({ {"text", w == "rarity" ? "Any\nLegendary\nEpic\nRare\nOther\n" : "Any\nSword\nDagger\nBow\nArmor\nCloak\nRing\n"} });
}
// SOW.LOOT_OPTIONS {kind, rarity} -> {text} captain gear the game can drop, "Rarity Kind: signature effect  [NG_...]"
json sow_cmd_loot_options(const json& a) {
    const std::string fk = a.value("kind", std::string("Any")), fr = a.value("rarity", std::string("Any"));
    return run_on_main([fk, fr]() -> json {
        std::vector<std::string> v;
        for (auto& r : table_records(kTblNemGear)) {
            const uintptr_t it = rec_ref(r.second, kNgItem); if (!it) continue;
            const std::string item = rname(it), kind = kind_label(kind_of(item)), rar = rarity_of(item, r.first);
            if (fk != "Any" && !fk.empty() && fk != kind) continue;
            if (fr != "Any" && !fr.empty() && fr != rar) continue;
            const uintptr_t af = rec_ref(r.second, kNgAffix);
            std::string iname = name_in("item", item); if (iname.empty()) iname = rar + " " + kind;
            std::string fx = af ? effect_text(rname(af), 0, false) : ""; if (fx.size() > 90) fx = fx.substr(0, 87) + "...";
            v.push_back(labelled(iname + (fx.empty() ? "" : ": " + fx), r.first));
        }
        std::sort(v.begin(), v.end()); std::string t; for (auto& x : v) t += x + "\n";
        return ok({ {"text", t}, {"count", v.size()} });
    }, 8000);
}
// SOW.LOOT_SET {on, gear, chance, template}
json sow_cmd_loot_set(const json& a) {
    const bool on = a.value("on", true);
    const std::string gear = strip_label(a.value("gear", std::string()));
    // source "Gear to Drop" = the gear picker only; "Edited Item" (or no source, older panels) = the template if one is picked
    const std::string tmpl = a.value("source", std::string()) == "Gear to Drop" ? std::string() : a.value("template", std::string());
    int chance = 100; if (a.contains("chance")) { const json& c = a["chance"]; chance = c.is_number() ? c.get<int>() : atoi(c.get<std::string>().c_str()); }
    chance = std::max(0, std::min(100, chance));
    return run_on_main([on, gear, tmpl, chance]() -> json {
        if (!on) { L_on = false; relimit(); return ok({ {"on", false} }); }
        std::string err; if (!loot_install(err)) return fail("HOOK", err);
        json st; std::string tname;
        if (!tmpl.empty() && tmpl != kNoTemplate) {
            if (!templates().contains(tmpl)) return fail("NO_TEMPLATE", "no drop template named '" + tmpl + "'");
            st = templates()[tmpl]; tname = tmpl;
        }
        // which gear record drops: the chosen one; with a template it must be the template's kind, else one is picked
        uintptr_t rec = gear.empty() ? 0 : record_in(kTblNemGear, gear);
        if (st.is_object()) {
            const std::string ti = st.value("item", std::string()), tk = kind_of(ti);
            if (!rec || kind_of(rname(rec_ref(rec, kNgItem))) != tk) {
                rec = 0; uintptr_t same_kind = 0;
                for (auto& r : table_records(kTblNemGear)) {
                    const std::string it = rname(rec_ref(r.second, kNgItem));
                    if (it == ti) { rec = r.second; break; }
                    if (!same_kind && kind_of(it) == tk && rarity_of(it, r.first) == rarity_of(ti, "")) same_kind = r.second;
                    if (!same_kind && kind_of(it) == tk) same_kind = r.second;
                }
                if (!rec) rec = same_kind;
            }
        }
        if (!rec) return fail("BAD_ARGS", "choose the gear to drop (or a template)");
        const bool was = L_on;
        L_owner = gq(game_base() + kTblNemGear); L_rec = rec; L_chance = chance; L_tmpl = tname; L_tmpl_state = st;
        if (!was) { L_known.clear(); L_seen = 0; L_hits = 0; L_pending = 0; }
        // every one-copy gear piece stays unlimited while drops are on, so a drop chosen a moment ago (or by an
        // earlier build before a reload) is never thrown away on pickup
        if (!all_unlimited) { all_unlimited = true; for (auto& r : table_records(kTblItems)) if (!kind_of(r.first).empty()) unlimit(r.second); }
        unlimit(rec_ref(rec, kNgItem));
        if (st.is_object()) unlimit(record_in(kTblItems, st.value("item", std::string())));
        L_on = true;
        return ok({ {"on", true}, {"drops", rname(rec)}, {"chance", chance}, {"template", tname} });
    }, 8000);
}
// SOW.LOOT_GIVE {gear, template} - put the chosen captain gear straight into the inventory, no captain needed.
// The game's AddItem (0x1C01E0) makes a bare level-1 piece with only its base effect, so it is then built up like a
// drop: a template (saved edited item) is copied on whole; otherwise the level becomes Talion's level and the gear's
// signature effect (NemesisAffix) is added. Values are re-derived from the level on the next load.
json sow_cmd_loot_give(const json& a) {
    const std::string gear = strip_label(a.value("gear", std::string())), tmpl = a.value("template", std::string());
    return run_on_main([gear, tmpl]() -> json {
        uintptr_t INV; std::string err; if (!player_inventory(INV, err)) return fail("NO_PLAYER", err);
        json st; if (!tmpl.empty() && tmpl != kNoTemplate) { if (!templates().contains(tmpl)) return fail("NO_TEMPLATE", "no saved item named '" + tmpl + "'"); st = templates()[tmpl]; }
        uintptr_t item = 0, sig = 0;
        if (st.is_object()) item = record_in(kTblItems, st.value("item", std::string()));
        else {
            const uintptr_t rec = gear.empty() ? 0 : record_in(kTblNemGear, gear);
            if (!rec) return fail("BAD_ARGS", "choose the gear to give (or a saved edited item)");
            item = rec_ref(rec, kNgItem); sig = rec_ref(rec, kNgAffix);
        }
        if (!item) return fail("NO_RECORD", "that gear has no item record");
        unlimit(item);                                           // one-copy legendaries: keep a second copy
        std::set<uint32_t> before; for (auto& g : gear_list(INV)) before.insert(g.id);
        json r = guarded_call(game_base() + kAddItemFn, { INV, item, 1 }, "u64", true);
        if (!r.value("ok", false)) return r;
        Gear ng{}; bool found = false;
        for (auto& g : gear_list(INV)) if (!before.count(g.id) && gq(g.eq + 0x38) == item) { ng = g; found = true; break; }
        if (!found) return fail("NOT_ADDED", "the game did not add " + rname(item));
        L_known.insert(ng.id);                                   // not a drop: the drop watcher leaves it alone
        json done = json::array();
        if (st.is_object()) {
            const std::string e = apply_state(ng, st, true, true, true, done); if (!e.empty()) return fail("TEMPLATE", e);
        } else {
            json cur = capture(ng);
            const uintptr_t root = gq(game_base() + 0x26FFA98), P = root ? gq(root + 0x888) : 0, lvo = P ? gq(P + 0x26A8) : 0;
            const uint32_t plv = lvo ? gd(lvo + 0x568) + 1 : 0;
            if (plv >= 1 && plv <= 120) cur["level"] = plv;
            // values: the same effect on an owned item of that level (else any level), else the bare value
            auto value_of = [&](const std::string& nm, float dflt) {
                float best = dflt; int bestd = 1 << 30;
                for (auto& o : gear_list(INV)) { if (o.eq == ng.eq) continue; for (uintptr_t af : affixes(o.eq)) if (rname(gq(af)) == nm) {
                    const int dd = abs((int)gd(o.eq + 0x24) - (int)cur.value("level", 1u)); if (dd < bestd) { bestd = dd; best = gf(af + 8); } } }
                return best; };
            json fx = json::array();
            if (sig) fx.push_back(json::array({ 0, rname(sig), value_of(rname(sig), 0.f), 0.f }));
            for (auto& f : cur["fx"]) { const std::string nm = f[1].get<std::string>(); if (sig && nm == rname(sig)) continue; f[2] = value_of(nm, f[2].get<float>()); fx.push_back(f); }
            cur["fx"] = fx;
            const std::string e = apply_state(ng, cur, false, true, true, done); if (!e.empty()) return fail("BUILD", e);
        }
        const std::string iname = name_in("item", rname(item));
        toast("Given: " + (iname.empty() ? rname(item) : iname) + " (item " + std::to_string(ng.id) + ")");
        return ok({ {"id", ng.id}, {"item", rname(item)}, {"level", gd(ng.eq + 0x24)}, {"done", done}, {"template", st.is_object() ? tmpl : ""} });
    }, 8000);
}
// ---- inventory editor: remove / duplicate ONE gear piece
// The game's remove core 0x1C0840(inventory, item record, count, instance id) removes that exact piece through
// 0x4F1ED8(inventory, id) and keeps the record's count right. The plain RemoveItem wrapper (0x4F163C) passes id 0,
// which matches nothing: the count drops but every piece stays (only reaching 0 clears them all).
constexpr uintptr_t kRemoveCore = 0x1C0840;
// Each item record has a stack (0x1C0C74(inventory, record)): +8/+0x10 = begin/end of the u32 piece ids, +0x20 =
// count. An item whose record was changed in the editor stays in its old record's stack, so the stack holding the id
// is looked up first. Given an id, the core does not take it off the id list (it only does that when it picks one
// itself), so the trainer erases it.
constexpr uintptr_t kFindStack = 0x1C0C74;
static bool stack_ids(uintptr_t st, uintptr_t& lo, uintptr_t& hi) { lo = gq(st + 8); hi = gq(st + 0x10); return st && lo && hi >= lo && hi - lo < 0x10000 && (hi - lo) % 4 == 0; }
static bool stack_has(uintptr_t st, uint32_t id) { uintptr_t lo, hi; if (!stack_ids(st, lo, hi)) return false; for (uintptr_t p = lo; p < hi; p += 4) if (gd(p) == id) return true; return false; }
static uintptr_t stack_of(uintptr_t INV, uintptr_t rec) { json r = guarded_call(game_base() + kFindStack, { INV, rec }, "u64", true); return r.value("ok", false) ? (uintptr_t)(uint64_t)r["data"]["ret_i"].get<int64_t>() : 0; }
static void stack_erase(uintptr_t st, uint32_t id) {
    uintptr_t lo, hi; if (!stack_ids(st, lo, hi)) return;
    for (uintptr_t p = lo; p < hi; p += 4) if (gd(p) == id) {
        for (uintptr_t q = p; q + 4 < hi; q += 4) { const uint32_t v = gd(q + 4); safe_write(q, &v, 4); }
        const uintptr_t ne = hi - 4; safe_write(st + 0x10, &ne, 8); return;
    }
}
// SOW.GEAR_DELETE {id} - remove one gear piece from the inventory (not while it is worn)
json sow_cmd_gear_delete(const json& a) {
    const uint32_t id = parse_id(a);
    return run_on_main([id]() -> json {
        Gear g; uintptr_t INV; std::string err; if (!find_gear(id, g, INV, err)) return fail("NO_ITEM", err);
        if (equipped(INV).count(g.eq)) return fail("EQUIPPED", "this item is worn - equip something else in its slot first");
        const uintptr_t cur = gq(g.eq + 0x38); const std::string nm = rname(cur);
        uintptr_t rec = 0, st = stack_of(INV, cur);
        if (stack_has(st, id)) rec = cur;
        else for (auto& r : table_records(kTblItems)) {
            if (kind_of(r.first).empty() || r.second == cur) continue;
            const uintptr_t s2 = stack_of(INV, r.second); if (stack_has(s2, id)) { rec = r.second; st = s2; break; }
        }
        if (!rec) return fail("NOT_TRACKED", "the game has no record of this piece in its item counts - not removed");
        json r = guarded_call(game_base() + kRemoveCore, { INV, rec, 1, (uint64_t)id }, "u64", true);
        if (!r.value("ok", false)) return r;
        for (auto& o : gear_list(INV)) if (o.id == id) return fail("NOT_REMOVED", "the game kept the item");
        const uintptr_t st2 = stack_of(INV, rec); if (st2) stack_erase(st2, id);
        json& o = originals(); if (o.contains(std::to_string(id))) { o.erase(std::to_string(id)); originals_save(); }
        const std::string shown = name_in("item", nm);
        toast("Removed: " + (shown.empty() ? nm : shown) + " (item " + std::to_string(id) + ")");
        return ok({ {"removed", id}, {"item", nm}, {"stack", rname(rec)} });
    }, 15000);
}
// SOW.GEAR_DUPLICATE {id} - a copy of one gear piece (item, level, look, effects with their values)
json sow_cmd_gear_duplicate(const json& a) {
    const uint32_t id = parse_id(a);
    return run_on_main([id]() -> json {
        Gear g; uintptr_t INV; std::string err; if (!find_gear(id, g, INV, err)) return fail("NO_ITEM", err);
        const json st = capture(g); const uintptr_t rec = gq(g.eq + 0x38);
        if (kind_of(rname(rec)).empty()) return fail("BAD_ITEM", "only weapons, armor, cloaks and rings can be copied");
        unlimit(rec);
        std::set<uint32_t> before; for (auto& x : gear_list(INV)) before.insert(x.id);
        json r = guarded_call(game_base() + kAddItemFn, { INV, rec, 1 }, "u64", true); if (!r.value("ok", false)) return r;
        Gear ng{}; bool found = false;
        for (auto& x : gear_list(INV)) if (!before.count(x.id) && gq(x.eq + 0x38) == rec) { ng = x; found = true; break; }
        if (!found) return fail("NOT_ADDED", "the game did not add a copy");
        L_known.insert(ng.id);
        json done = json::array(); const std::string e = apply_state(ng, st, true, true, true, done); if (!e.empty()) return fail("COPY", e);
        toast("Copied item " + std::to_string(id) + " -> item " + std::to_string(ng.id));
        return ok({ {"id", ng.id}, {"from", id}, {"done", done} });
    }, 8000);
}
// SOW.LOOT_STATUS -> {text}
json sow_cmd_loot_status(const json&) {
    return run_on_main([]() -> json {
        std::string t;
        if (!L_on) t = "Off.";
        else {
            const uintptr_t it = L_rec ? rec_ref(L_rec, kNgItem) : 0; std::string nm = it ? name_in("item", rname(it)) : "";
            t = "On: " + (nm.empty() ? rname(L_rec) : nm) + "  [" + rname(L_rec) + "], " + std::to_string((int)L_chance) + "% of gear drops";
            if (!L_tmpl.empty()) t += ", template '" + L_tmpl + "'";
        }
        t += "\nGear drops seen: " + std::to_string((int)L_seen) + ", replaced: " + std::to_string((int)L_hits) + ", waiting to be picked up: " + std::to_string((int)L_pending);
        if (!L_last.empty()) t += "\nLast: " + L_last;
        t += "\nDrop-code calls: " + std::to_string((int)L_calls);
        auto tbl = [](uintptr_t rec) -> std::string { const uintptr_t own = gq(rec + 0x18), ty = own ? gq(own + 0x20) : 0; return ty ? rname(ty - 0) : "?"; };
        const int n = std::min((int)L_ri, 8);
        for (int k = 0; k < n; ++k) {
            const int i = ((int)L_ri - 1 - k) & 7; const uintptr_t p = L_ring[i][0], r = L_ring[i][1];
            char b[64]; snprintf(b, sizeof b, "\n  picker %llx -> ", (unsigned long long)p);
            t += b + (r ? rname(r) + " {" + tbl(r) + "}" : std::string("nothing"));
        }
        return ok({ {"text", t}, {"on", (bool)L_on}, {"seen", (int)L_seen}, {"hits", (int)L_hits}, {"pending", (int)L_pending} });
    }, 3000);
}
// SOW.LOOT_TEMPLATE_SAVE {id, name?} - save an item as it is now (after editing it in the Gear tab) as a drop template
json sow_cmd_loot_template_save(const json& a) {
    const uint32_t id = parse_id(a); const std::string want = a.value("name", std::string());
    return run_on_main([id, want]() -> json {
        Gear g; uintptr_t INV; std::string err; if (!find_gear(id, g, INV, err)) return fail("NO_ITEM", err);
        const json st = capture(g); const std::string item = st.value("item", std::string());
        if (kind_of(item).empty()) return fail("BAD_ARGS", "only swords, daggers, bows, armor, cloaks and rings can drop");
        std::string nm = want;
        if (nm.empty()) {
            const uintptr_t lk = gq(g.eq + 0x40) ? gq(g.eq + 0x40) : gq(g.eq + 0x48);
            std::string ln = lk ? name_in("look", rname(lk)) : ""; if (ln.empty()) ln = name_in("item", item); if (ln.empty()) ln = item;
            nm = ln + " (lvl " + std::to_string(gd(g.eq + 0x24)) + ", " + std::to_string(st["fx"].size()) + " effects, from #" + std::to_string(id) + ")";
        }
        templates()[nm] = st; templates_save();
        return ok({ {"saved", nm}, {"template", st} });
    }, 4000);
}
// SOW.LOOT_TEMPLATES -> {text}
// both on the game thread: the panel poll and the drop tick iterate the same json there (review 2026-11-14)
json sow_cmd_loot_templates(const json&) { return run_on_main([]() -> json {
    std::string t = std::string(kNoTemplate) + "\n";
    for (auto it = templates().begin(); it != templates().end(); ++it) t += it.key() + "\n";
    return ok({ {"text", t}, {"count", templates().size()} });
}, 3000); }
// SOW.LOOT_TEMPLATE_DELETE {template}
json sow_cmd_loot_template_delete(const json& a) { return run_on_main([a]() -> json {
    const std::string nm = a.value("template", std::string());
    if (nm.empty() || nm == kNoTemplate || !templates().contains(nm)) return fail("BAD_ARGS", "choose a saved template");
    templates().erase(nm); templates_save();
    return ok({ {"deleted", nm} });
}, 3000); }

} // namespace wb
