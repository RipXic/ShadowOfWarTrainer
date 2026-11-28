// Shadow of War Trainer - shared declarations.
// In-game trainer DLL for Middle-earth: Shadow of War (Steam 1.0.8874, DX11).
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include <mutex>
#include <atomic>
#include "json.hpp"

using json = nlohmann::json;

#define WB_PRODUCT "Shadow of War Trainer"
#define WB_PRODUCT_UP "SHADOW OF WAR TRAINER"
namespace wb {

constexpr const char* kVersion = "1.2.2";
constexpr const char* kGameExe = "ShadowOfWar.exe";

struct Config {
    std::string root;              // folder holding the DLL, config, signatures
    int toggle_key = 0x2D;         // VK_INSERT opens/closes the panel
    bool lock_input = true;        // the game gets no mouse/keys while the panel is open
};
extern Config g_cfg;
void load_config();
void save_config();

// ---- logging
void log(const char* fmt, ...);
std::vector<std::string> log_tail(size_t n);
// crash logs (core.cpp): the previous session ended without a normal exit -> its log was saved in logs\crashes
bool crash_last_session();
int crash_log_count();
std::string crash_log_dir();
// loaded-DLL snapshot: "Name.dll+0x1234" for an address, lock-free (safe inside an exception handler)
void modsnap_refresh();
bool modsnap_find(uintptr_t addr, char* out, size_t n, bool* system);

// ---- languages (lang.cpp, render thread): English text in, translated text out (English when there is none)
struct LangInfo { std::string code, name; };
const char* tr(const char* s);
std::string tr(const std::string& s);
std::string tr_msg(const std::string& s);          // toasts / errors: patterns and "label - message"
std::string upper_utf8(const std::string& s); std::string lower_utf8(const std::string& s);
void lang_init(); const std::vector<LangInfo>& lang_list(); const std::string& lang_code();
void lang_request(const std::string& code); bool lang_apply_pending();
std::string lang_font_file(); std::string lang_glyph_text();

// ---- modules / memory
struct ModuleInfo { std::string name; uintptr_t base = 0; size_t size = 0; };
std::vector<ModuleInfo> list_modules(bool refresh = false);
std::string module_name_of(uintptr_t addr);
uintptr_t module_base(const std::string& name, size_t* size = nullptr);
uintptr_t game_base(size_t* size = nullptr);
bool safe_read(uintptr_t addr, void* out, size_t n);
bool safe_write(uintptr_t addr, const void* in, size_t n);
template <class T> bool read_as(uintptr_t a, T& v) { return safe_read(a, &v, sizeof(T)); }
std::string hexstr(uint64_t v);
std::string to_hex(const uint8_t* p, size_t n);
bool parse_pattern(const std::string& s, std::vector<int>& pat);

// Address expressions: "game+0x1234", "[player]+0x10", "sig:playerFetch", "0x7FF6...", "ShadowOfWar.exe+0x10"
// [x] dereferences. Symbols: game, module names, sig:<name>, player (captured by the anchor).
bool eval_addr(const std::string& expr, uintptr_t& out, std::string* err = nullptr);

struct Region { uintptr_t start; size_t size; bool exec; };
std::vector<Region> readable_regions(uintptr_t base, size_t size);
std::vector<Region> heap_regions();   // committed private RW memory

// ---- main game thread (window thread, PeekMessage hook)
bool install_main_thread_hook();    // engine BeginFrame task tick (sig:engineTask)
bool wait_game_window();
DWORD main_thread_id();
uint64_t main_ticks();
json run_on_main(std::function<json()> fn, DWORD timeout_ms = 3000);
void post_main(std::function<void()> fn);
void add_main_tick(void (*fn)());
// ---- overlay (overlay.cpp) + panel (panel.cpp)
bool install_overlay(); bool overlay_visible(); void overlay_set_visible(bool v); json overlay_status();
void toast(const std::string& text);
void panel_init(); void panel_draw(bool* visible); json panel_reload(); json panel_show(bool visible, const std::string& tab);
json panel_state(); json panel_set(const std::string& id, const json& value); json panel_favorites(const json& a); void panel_main_tick(); void panel_hotkeys_poll();
json dispatch(const json& req);
json sow_cmd_hks_exec(const json& a); json sow_cmd_status(const json& a);   // sow.cpp (Havok Script)
bool hks_run(const std::string& code, std::string* out, std::string* err);  // game thread only
bool hks_run_panel(const std::string& code, std::string* out, std::string* err);  // + waits until the player is stably in the world
bool sow_world_ready(); uint64_t sow_world_epoch();
json sow_cmd_gear_options(const json& a); json sow_cmd_gear_card(const json& a); json sow_cmd_gear_level(const json& a); json sow_cmd_gear_affix_options(const json& a); json sow_cmd_gear_affix(const json& a); json sow_cmd_gear_value(const json& a); json sow_cmd_gear_add_affix(const json& a); json sow_cmd_gear_remove_affix(const json& a); json sow_cmd_gear_look_options(const json& a); json sow_cmd_gear_look(const json& a); json sow_cmd_gear_look_preview(const json& a); json sow_cmd_loot_give(const json& a); json sow_cmd_summon_nocd(const json& a); json sow_cmd_gear_delete(const json& a); json sow_cmd_gear_duplicate(const json& a); void gear_preview_tick(); json sow_cmd_gear_item_options(const json& a); json sow_cmd_gear_item(const json& a); json sow_cmd_gear_refresh(const json& a); json sow_cmd_gear_reset(const json& a);  // sow_gear.cpp
json sow_cmd_loot_filter_options(const json& a); json sow_cmd_loot_options(const json& a); json sow_cmd_loot_set(const json& a); json sow_cmd_loot_status(const json& a); json sow_cmd_loot_template_save(const json& a); json sow_cmd_loot_templates(const json& a); json sow_cmd_loot_template_delete(const json& a);  // sow_gear.cpp loot drops
json sow_cmd_inv(const json& a); json sow_cmd_item(const json& a); json sow_cmd_item_add(const json& a); json sow_cmd_health(const json& a); json sow_cmd_skill_menu_unlock(const json& a); json sow_cmd_chain_tweak(const json& a); json sow_cmd_health_lock(const json& a); json sow_cmd_rage_refill(const json& a); bool sow_game_supported(); json sow_cmd_instant_dominate(const json& a); json sow_cmd_dominate_fast(const json& a); void god_restore(); void chain_boot_tick(); void skill_menu_restore(); json sow_cmd_item_remove(const json& a); json sow_cmd_item_set(const json& a); json sow_cmd_items(const json& a); json sow_cmd_level(const json& a); json sow_cmd_owned(const json& a); json sow_cmd_skill_activate(const json& a); json sow_cmd_skills_all(const json& a); json sow_cmd_raise_dead(const json& a); json sow_cmd_raise_captains(const json& a); json sow_cmd_skill_compat(const json& a);  // sow_prog.cpp
json sow_cmd_army(const json& a); json sow_cmd_orc(const json& a); json sow_cmd_orc_level(const json& a); json sow_cmd_orc_reward(const json& a); json sow_cmd_orc_follower(const json& a); json sow_cmd_orc_bodyguard(const json& a); json sow_cmd_orc_intel(const json& a); json sow_cmd_orc_personality(const json& a); json sow_cmd_orc_build(const json& a); json sow_cmd_rewards(const json& a); json sow_cmd_builders(const json& a); json sow_cmd_army_options(const json& a); json sow_cmd_orc_card(const json& a); json sow_cmd_orc_gear(const json& a); json sow_cmd_orc_give_item(const json& a); json sow_cmd_orc_role(const json& a); json sow_cmd_orc_rename(const json& a); json sow_cmd_orc_rarity(const json& a); json sow_cmd_army_lists(const json& a); json sow_cmd_spawn_captain(const json& a); json sow_cmd_spawn_status(const json& a); json sow_cmd_spawn_options(const json& a);  // sow_army.cpp
json guarded_call(uintptr_t fn, const std::vector<uint64_t>& args, const std::string& ret, bool quiet = false);
// fault details of a guarded game call (calls.cpp): instruction, access kind (0 read, 1 write, 8 exec), address
struct FaultInfo { DWORD code = 0; uintptr_t at = 0; int rw = -1; uintptr_t addr = 0; };
int fault_filter(EXCEPTION_POINTERS* ep, FaultInfo* fi); std::string fault_text(const FaultInfo& f);
HWND find_game_window();
HMODULE self_module();

// ---- command dispatch (server.cpp)
json dispatch(const json& req);
json ok(json data = json::object(), const std::string& msg = "ok");
json fail(const std::string& code, const std::string& msg);

// ---- signatures (sigs.cpp) - named byte patterns from signatures.json
void sigs_init();
uintptr_t sig_addr(const std::string& name);

} // namespace wb
