// Shadow of War Trainer - command table: every trainer action the panel runs, by name.
// Reply envelope: {ok, code, message, data, warnings}
#include "common.h"
#include <map>
#include <thread>
#include <algorithm>
#include <fstream>
#include "MinHook.h"

namespace wb {

json ok(json data, const std::string& msg) {
    return { {"ok", true}, {"code", "OK"}, {"message", msg}, {"data", data}, {"warnings", json::array()} };
}
json fail(const std::string& code, const std::string& msg) {
    return { {"ok", false}, {"code", code}, {"message", msg}, {"data", nullptr}, {"warnings", json::array()} };
}

// ------------------------------------------------------------------ command table
using Handler = std::function<json(const json&)>;
struct Cmd { const char* name; const char* desc; Handler fn; };
static std::vector<Cmd>& commands();


static std::vector<Cmd>& commands() {
    static std::vector<Cmd> c = {
        {"SOW.ITEM", "Count of an inventory item through the game's GetItemCount {name e.g. PC_Mirian | SkillPoint, table}", sow_cmd_item},
        {"SOW.DOMINATE_FAST", "Domination gauge fills instantly on any enemy {on, seconds=0.1}", sow_cmd_dominate_fast},
        {"SOW.INSTANT_DOMINATE", "Grab and dominate captains at full health (no weakening needed) {on}", sow_cmd_instant_dominate},
        {"SOW.RAGE_REFILL", "Elven Rage drains normally, the Wrath meter refills when rage ends {on}", sow_cmd_rage_refill},
        {"SOW.HEALTH_LOCK", "Talion takes no damage: hook on the game damage write {on}", sow_cmd_health_lock},
        {"SOW.CHAIN_TWEAK", "Wraith Chain: {aim_free, no_slowmo, fast} (data tweaks, restored on off / reload)", sow_cmd_chain_tweak},
        {"SOW.SKILL_MENU_UNLOCK", "Skill menu sells every skill (story lock, level, prerequisites off) {on}", sow_cmd_skill_menu_unlock},
        {"SOW.HEALTH", "Talion's health from the player object; fill:true refills it (no script calls) {fill}", sow_cmd_health},
        {"SOW.ITEM_ADD", "Give items through the game's AddItem {name, count}", sow_cmd_item_add},
        {"SOW.ITEM_SET", "Set an item count (adds/removes the difference) {name, value}", sow_cmd_item_set},
        {"SOW.ARMY_OPTIONS", "Follower picker text for the panel {all}", sow_cmd_army_options},
        {"SOW.ORC_CARD", "One-line orc summary for the panel {id}", sow_cmd_orc_card},
        {"SOW.ORC_GEAR", "An orc's weapons and armor by slot {id}", sow_cmd_orc_gear},
        {"SOW.ORC_GIVE_ITEM", "Equip an item on an orc through the game's GiveItem + model rebuild {id, item}", sow_cmd_orc_give_item},
        {"SOW.ORC_ROLE", "Set an orc's role (title + role gear set) through the game's SetRole {id, role}", sow_cmd_orc_role},
        {"SOW.ORC_RENAME", "Rename an orc through the game's SetName {id, name | hash}", sow_cmd_orc_rename},
        {"SOW.ORC_RARITY", "Upgrade rarity with the game's upgrade rewards, keeping the role {id, to: epic|legendary, keep_role}", sow_cmd_orc_rarity},
        {"SOW.ARMY_LISTS", "Picker texts for the panel {list: names|roles|gear, filter}", sow_cmd_army_lists},
        {"SOW.ORC", "One orc in detail: traits, markers, flags, personality, builder {id | ptr, region}", sow_cmd_orc},
        {"SOW.ORC_LEVEL", "Set an orc's level through the game's SetXPLevel (clears the level lock) {id, level, unlock, force}", sow_cmd_orc_level},
        {"SOW.ORC_REWARD", "Apply a Faction reward record (traits, tribe, level, items...) through the game's ApplyReward {id, reward, force}", sow_cmd_orc_reward},
        {"SOW.ORC_FOLLOWER", "Recruit / release through the game's SetPlayerFollower {id, on}", sow_cmd_orc_follower},
        {"SOW.ORC_BODYGUARD", "Bodyguard on/off through the game's SetPlayerBodyguard {id, on}", sow_cmd_orc_bodyguard},
        {"SOW.ORC_INTEL", "Reveal everything about an orc {id}", sow_cmd_orc_intel},
        {"SOW.ORC_BUILD", "Create a new orc from a builder record in the current region {builder}", sow_cmd_orc_build},
        {"SOW.SKILLS_ALL", "Activate every bought Talion skill + upgrade together {buy_missing, prestige, cut, filter} (needs Choose Multiple Skills)", sow_cmd_skills_all},
        {"SOW.GEAR_OPTIONS", "Talion's gear for the editor picker (id: item (slot, lvl) [equipped])", sow_cmd_gear_options},
        {"SOW.GEAR_CARD", "One item: level and numbered effects {id}", sow_cmd_gear_card},
        {"SOW.GEAR_LEVEL", "Set an item's level - saved, stats re-derived on reload {id, level}", sow_cmd_gear_level},
        {"SOW.GEAR_AFFIX_OPTIONS", "All effect definitions, the item's own type first {id}", sow_cmd_gear_affix_options},
        {"SOW.GEAR_AFFIX", "Replace effect #index with another effect - saved {id, index, affix}", sow_cmd_gear_affix},
        {"SOW.GEAR_ADD_AFFIX", "Add an extra effect to an item - saved {id, affix}", sow_cmd_gear_add_affix},
        {"SOW.GEAR_REMOVE_AFFIX", "Remove effect #index from an item - saved {id, index}", sow_cmd_gear_remove_affix},
        {"SOW.GEAR_LOOK_OPTIONS", "Appearances for this kind of item (transmog) {id}", sow_cmd_gear_look_options},
        {"SOW.GEAR_DELETE", "Remove one gear piece from the inventory (not while worn) {id}", sow_cmd_gear_delete},
        {"SOW.GEAR_DUPLICATE", "Copy one gear piece: item, level, look, effects {id}", sow_cmd_gear_duplicate},
        {"SOW.SUMMON_NOCD", "No cooldown on summons (followers, bodyguards, mounts) {on}", sow_cmd_summon_nocd},
        {"SOW.LOOT_GIVE", "Put the chosen captain gear (or a saved edited item) straight into the inventory {gear, template}", sow_cmd_loot_give},
        {"SOW.GEAR_LOOK_PREVIEW", "Show a look on the worn item before applying it (closing the panel undoes it) {id, look}", sow_cmd_gear_look_preview},
        {"SOW.GEAR_LOOK", "Transmog: another appearance of the same kind - saved {id, look}", sow_cmd_gear_look},
        {"SOW.GEAR_ITEM_OPTIONS", "Item records of the same kind (rarity / set / variant) {id}", sow_cmd_gear_item_options},
        {"SOW.GEAR_ITEM", "Change rarity / set / variant (item record) - saved {id, item}", sow_cmd_gear_item},
        {"SOW.LOOT_FILTER_OPTIONS", "Loot picker filters {which: kind|rarity}", sow_cmd_loot_filter_options},
        {"SOW.LOOT_OPTIONS", "Captain gear the game can drop {kind, rarity}", sow_cmd_loot_options},
        {"SOW.LOOT_SET", "Custom captain drops {on, gear, chance 0-100, template}", sow_cmd_loot_set},
        {"SOW.LOOT_STATUS", "Custom drop state and counters", sow_cmd_loot_status},
        {"SOW.LOOT_TEMPLATE_SAVE", "Save an (edited) item as a drop template {id, name?}", sow_cmd_loot_template_save},
        {"SOW.LOOT_TEMPLATES", "Saved drop templates", sow_cmd_loot_templates},
        {"SOW.LOOT_TEMPLATE_DELETE", "Delete a drop template {template}", sow_cmd_loot_template_delete},
        {"SOW.GEAR_RESET", "Back to normal: the item as it was before the trainer edited it {id, what: look|effects|all}", sow_cmd_gear_reset},
        {"SOW.GEAR_REFRESH", "Re-equip a worn item live so edits show right away {id}", sow_cmd_gear_refresh},
        {"SOW.GEAR_VALUE", "Set effect #index's value - live only {id, index, value}", sow_cmd_gear_value},
        {"SOW.SKILL_COMPAT", "Fix upgrades that block each other with Choose Multiple Skills (Shadow Dominate + Shadow Pull) {on}", sow_cmd_skill_compat},
        {"SOW.RAISE_DEAD", "Raise Dead now: skill-menu node in the Story row + PS_Raise_* items + Dark Talion switch {on, dark_talion, upgrades}", sow_cmd_raise_dead},
        {"SOW.LEVEL","Player level (read) or set it through the game's own level setter {set: 1..80}", sow_cmd_level},
    };
    return c;
}

json dispatch(const json& req) {
    if (!req.is_object()) return fail("BAD_REQUEST", "Body must be a JSON object {cmd, args}");
    std::string name = req.value("cmd", "");
    json args = req.contains("args") && req["args"].is_object() ? req["args"] : json::object();
    if (_strnicmp(name.c_str(), "SOW.", 4) == 0 && !sow_game_supported())
        return fail("UNSUPPORTED_GAME", "This game version is not supported (the trainer needs Shadow of War 1.0.8874 on Steam).");
    for (auto& c : commands()) {
        if (_stricmp(c.name, name.c_str()) == 0) {
            try { return c.fn(args); }
            catch (std::exception& e) { return fail("EXCEPTION", std::string(c.name) + ": " + e.what()); }
        }
    }
    return fail("UNKNOWN_COMMAND", "Unknown command '" + name + "'.");
}


} // namespace wb
