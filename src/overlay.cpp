// Shadow of War Trainer - overlay: DX11 Present hook, input capture while the panel is open, ImGui frame.
#include "common.h"
#ifndef QWORD
typedef unsigned __int64 QWORD;
#endif
#include <d3d11.h>
#include <dxgi.h>
#include <atomic>
#include <deque>
#include <fstream>
#include <vector>
#include <initializer_list>
#include <algorithm>
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include "MinHook.h"
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace wb {

ImFont* g_font_body = nullptr;
ImFont* g_font_bold = nullptr;
ImFont* g_font_title = nullptr;
ImFont* g_font_skin = nullptr;        // skin: Palatino Bold (tabs, section headers)        - drawn scaled by the panel
ImFont* g_font_skin_big = nullptr;    // skin: Palatino Bold, large (page title plate)
ImFont* g_font_skin_row = nullptr;    // skin: Segoe UI (row labels)
float g_ui_scale = 1.f;

using Present_t = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT);
using Resize_t = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
static Present_t o_Present = nullptr;
static Resize_t o_Resize = nullptr;
static ID3D11Device* g_dev = nullptr;
static ID3D11DeviceContext* g_ctx = nullptr;
static ID3D11RenderTargetView* g_rtv = nullptr;
static IDXGISwapChain* g_sc = nullptr;
static HWND g_hwnd = nullptr;
static WNDPROC o_WndProc = nullptr;
static bool g_ui_ready = false;
static std::atomic<bool> g_visible{false};
static std::atomic<bool> g_dead{false};
static std::atomic<uint64_t> g_frames{0};
static std::recursive_mutex g_imgui_mtx;
static ULONGLONG g_ready_ms = 0;
static std::string g_note = "not hooked";

ID3D11Device* overlay_device() { return g_dev; }
bool overlay_visible() { return g_visible; }
void overlay_set_visible(bool v) { g_visible = v; }

// ------------------------------------------------------------------ toasts
static std::mutex g_toast_mtx;
static std::deque<std::pair<std::string, ULONGLONG>> g_toasts;
void toast(const std::string& text) {
    std::lock_guard<std::mutex> lk(g_toast_mtx);
    g_toasts.push_back({ text, GetTickCount64() });
    while (g_toasts.size() > 5) g_toasts.pop_front();
    log("[toast] %s", text.c_str());
}

// ------------------------------------------------------------------ fonts
// Every font gets its own letters first; letters it does not have (Cyrillic, Chinese, ...) are merged in from Windows
// fonts. Only letters that appear in the panel and the chosen language are loaded (lang_glyph_text).
static void load_fonts() {
    const float k = g_ui_scale;
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;                  // layout lives in panel.json
    char win[MAX_PATH]; GetWindowsDirectoryA(win, MAX_PATH);
    const std::string f = std::string(win) + "\\Fonts\\";
    auto exists = [](const std::string& p) { return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES; };
    static const ImWchar ranges[] = { 0x0020, 0x00FF, 0x2013, 0x2026, 0x2190, 0x21FF, 0x2600, 0x26FF, 0 };   // latin, dashes, arrows, stars

    // letters beyond that set
    ImFontGlyphRangesBuilder gb; gb.AddText(lang_glyph_text().c_str());
    bool any = false, han = false, kana = false, hangul = false, thai = false;
    for (unsigned c = 0x20; c <= 0xFFFF; ++c) {
        if (!gb.GetBit(c)) continue;
        bool base = false; for (const ImWchar* r = ranges; *r; r += 2) if (c >= r[0] && c <= r[1]) base = true;
        if (base) continue;
        any = true;
        if (c >= 0x3040 && c <= 0x30FF) kana = true;
        else if ((c >= 0x1100 && c <= 0x11FF) || (c >= 0xAC00 && c <= 0xD7AF)) hangul = true;
        else if (c >= 0x0E00 && c <= 0x0E7F) thai = true;
        else if (c >= 0x2E80 && c <= 0xFAFF) han = true;
        else if (c >= 0xFF00 && c <= 0xFFEF) han = true;
    }
    ImVector<ImWchar> extra; if (any) gb.BuildRanges(&extra);
    std::vector<std::string> fb;               // fallback fonts, best first
    if (any) {
        auto first = [&](std::initializer_list<const char*> names) { for (auto n : names) if (exists(f + n)) { fb.push_back(f + n); return; } };
        const std::string lc = lang_code(), own = lang_font_file();
        if (!own.empty() && exists(own)) fb.push_back(own);
        first({ "segoeui.ttf" });
        const bool ja = lc.rfind("ja", 0) == 0, tw = lc.rfind("zh-TW", 0) == 0 || lc.rfind("zh-HK", 0) == 0;
        if (ja) first({ "YuGothM.ttc", "meiryo.ttc", "msgothic.ttc" });
        if (han && tw) first({ "msjh.ttc", "mingliu.ttc" });
        if (hangul) first({ "malgun.ttf" });
        if (han) first({ "msyh.ttc", "simsun.ttc" });
        if (kana && !ja) first({ "YuGothM.ttc", "meiryo.ttc", "msgothic.ttc" });
        if (thai) first({ "LeelawUI.ttf", "tahoma.ttf" });
    }
    // each font file is read once and shared (the Chinese ones are ~20 MB); all of it is freed after Build()
    std::vector<std::pair<std::string, std::vector<char>>> files; files.reserve(32);
    auto add = [&](const std::string& path, float px, const ImWchar* r, bool merge) -> ImFont* {
        std::vector<char>* d = nullptr;
        for (auto& x : files) if (x.first == path) d = &x.second;
        if (!d) {
            std::ifstream in(path, std::ios::binary); if (!in) return nullptr;
            files.push_back({ path, std::vector<char>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()) });
            d = &files.back().second;
        }
        if (d->size() < 256) return nullptr;
        ImFontConfig c; c.FontDataOwnedByAtlas = false; c.MergeMode = merge;
        return io.Fonts->AddFontFromMemoryTTF(d->data(), (int)d->size(), px, &c, r);
    };
    auto font = [&](const std::string& path, float px, bool symbols = false) -> ImFont* {
        ImFont* o = add(path, px, ranges, false);
        if (!o) return nullptr;
        if (symbols && exists(f + "seguisym.ttf")) add(f + "seguisym.ttf", px, ranges, true);
        if (any) { add(path, px, extra.Data, true); for (auto& p : fb) if (p != path) add(p, px, extra.Data, true); }
        return o;
    };
    g_font_body = g_font_bold = g_font_title = g_font_skin = g_font_skin_big = g_font_skin_row = nullptr;
    if (exists(f + "segoeui.ttf")) g_font_body = font(f + "segoeui.ttf", 17.f * k, true);
    if (exists(f + "seguisb.ttf")) { g_font_bold = font(f + "seguisb.ttf", 16.f * k); g_font_title = font(f + "seguisb.ttf", 25.f * k); }
    // skin fonts: loaded at the size they have in the 1620x971 reference times a margin, the panel draws them at K
    {   const float sk = std::max(1.f, k) * 1.25f;
        // Shadow of War skin: bin\skin\font.ttf (titles, tabs) and bin\skin\font_row.ttf (row labels) win when present
        // (e.g. Cinzel / Alegreya); otherwise Palatino Bold + Palatino (serif like the reference art)
        const std::string own = g_cfg.root + "\\skin\\font.ttf", own_row = g_cfg.root + "\\skin\\font_row.ttf";
        const std::string pal = exists(own) ? own : exists(f + "palab.ttf") ? f + "palab.ttf" : exists(f + "georgiab.ttf") ? f + "georgiab.ttf" : "";
        if (!pal.empty()) { g_font_skin = font(pal, 24.f * sk); g_font_skin_big = font(pal, 34.f * sk); }
        const std::string row = exists(own_row) ? own_row : exists(f + "pala.ttf") ? f + "pala.ttf" : exists(f + "segoeui.ttf") ? f + "segoeui.ttf" : "";
        if (!row.empty()) g_font_skin_row = font(row, 19.f * sk); }
    if (!g_font_body) g_font_body = io.Fonts->AddFontDefault();
    if (!g_font_bold) g_font_bold = g_font_body;
    if (!g_font_title) g_font_title = g_font_bold;
    io.Fonts->Build();
    // the shared file buffers go away with this function. Not ClearInputData(): it also drops the custom rects, and the
    // software mouse cursor the open panel draws is one of them (crash)
    for (ImFontConfig& c : io.Fonts->Sources) if (!c.FontDataOwnedByAtlas) c.FontData = nullptr;
    log("[fonts] %s, atlas %dx%d, %zu fallback fonts", lang_code().c_str(), io.Fonts->TexWidth, io.Fonts->TexHeight, fb.size());
}
// between two frames only
static void reload_fonts() {
    ImGui_ImplDX11_InvalidateDeviceObjects();  // drops the old font texture; the next ImplDX11_NewFrame makes the new one
    ImGui::GetIO().Fonts->Clear();
    load_fonts();
}

// ------------------------------------------------------------------ input
static bool input_blocked() { return g_visible && g_cfg.lock_input && !g_dead; }

static LRESULT CALLBACK hk_WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (g_dead || !o_WndProc) return o_WndProc ? CallWindowProcW(o_WndProc, h, m, w, l) : DefWindowProcW(h, m, w, l);
    if (g_ui_ready && g_visible) {
        bool want_mouse = false, want_keys = false;
        {
            std::lock_guard<std::recursive_mutex> lk(g_imgui_mtx);
            if (ImGui::GetCurrentContext()) {
                ImGui_ImplWin32_WndProcHandler(h, m, w, l);
                want_mouse = ImGui::GetIO().WantCaptureMouse; want_keys = ImGui::GetIO().WantCaptureKeyboard;
            }
        }
        const bool is_mouse = m >= WM_MOUSEFIRST && m <= WM_MOUSELAST, is_key = m >= WM_KEYFIRST && m <= WM_KEYLAST;
        // key RELEASES always reach the game: a key held when the panel opened (Insert itself at least) must be let go
        // in the game too, or it stays "held" - the gamepad then went dead until the panel was opened/closed again
        // (Fable review 2026-11-13). Presses stay blocked.
        const bool is_up = m == WM_KEYUP || m == WM_SYSKEYUP;
        if (is_key && !is_up && (int)w == g_cfg.toggle_key) return 0;
        if (g_cfg.lock_input && (is_mouse || (is_key && !is_up))) return 0;
        if ((is_mouse && want_mouse) || (is_key && !is_up && want_keys)) return 0;
        // raw input goes to the game: hk_GetRawInputData / hk_GetRawInputBuffer zero presses and movement but keep
        // releases (DefWindowProc here used to drop the releases as well)
        if (m == WM_SETCURSOR) return TRUE;
    }
    return CallWindowProcW(o_WndProc, h, m, w, l);
}

// The game recenters/clips the cursor for mouse-look and reads raw input: while the panel is open both are ignored and
// raw movement/presses are zeroed before the game sees them (button-ups kept so nothing sticks).
using SetCursorPos_t = BOOL(WINAPI*)(int, int);
using ClipCursor_t = BOOL(WINAPI*)(const RECT*);
using GetRawInputData_t = UINT(WINAPI*)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
using GetRawInputBuffer_t = UINT(WINAPI*)(PRAWINPUT, PUINT, UINT);
static SetCursorPos_t o_SetCursorPos = nullptr;
static ClipCursor_t o_ClipCursor = nullptr;
static GetRawInputData_t o_GetRawInputData = nullptr;
static GetRawInputBuffer_t o_GetRawInputBuffer = nullptr;
static BOOL WINAPI hk_SetCursorPos(int x, int y) { if (input_blocked()) return TRUE; return o_SetCursorPos(x, y); }
static BOOL WINAPI hk_ClipCursor(const RECT* r) { if (input_blocked() && r) return o_ClipCursor(nullptr); return o_ClipCursor(r); }
static void neuter_raw(RAWINPUT* ri) {
    if (ri->header.dwType == RIM_TYPEMOUSE) {
        ri->data.mouse.lLastX = 0; ri->data.mouse.lLastY = 0;
        USHORT up = RI_MOUSE_LEFT_BUTTON_UP | RI_MOUSE_RIGHT_BUTTON_UP | RI_MOUSE_MIDDLE_BUTTON_UP | RI_MOUSE_BUTTON_4_UP | RI_MOUSE_BUTTON_5_UP;
        ri->data.mouse.usButtonFlags &= up; ri->data.mouse.usButtonData = 0;
    } else if (ri->header.dwType == RIM_TYPEKEYBOARD) {
        if (!(ri->data.keyboard.Flags & RI_KEY_BREAK)) { ri->data.keyboard.VKey = 0xFF; ri->data.keyboard.MakeCode = 0; }
    }
}
static UINT WINAPI hk_GetRawInputData(HRAWINPUT h, UINT cmd, LPVOID data, PUINT size, UINT hdr) {
    UINT r = o_GetRawInputData(h, cmd, data, size, hdr);
    if (cmd == RID_INPUT && data && r != (UINT)-1 && r >= sizeof(RAWINPUTHEADER) && input_blocked()) neuter_raw((RAWINPUT*)data);
    return r;
}
static UINT WINAPI hk_GetRawInputBuffer(PRAWINPUT data, PUINT size, UINT hdr) {
    UINT n = o_GetRawInputBuffer(data, size, hdr);
    if (data && n != (UINT)-1 && input_blocked()) { RAWINPUT* ri = data; for (UINT i = 0; i < n; ++i) { neuter_raw(ri); ri = NEXTRAWINPUTBLOCK(ri); } }
    return n;
}
static void install_input_hooks() {
    HMODULE u32 = GetModuleHandleA("user32.dll");
    struct H { const char* n; void* d; void** o; } hs[] = {
        { "SetCursorPos", (void*)&hk_SetCursorPos, (void**)&o_SetCursorPos },
        { "ClipCursor", (void*)&hk_ClipCursor, (void**)&o_ClipCursor },
        { "GetRawInputData", (void*)&hk_GetRawInputData, (void**)&o_GetRawInputData },
        { "GetRawInputBuffer", (void*)&hk_GetRawInputBuffer, (void**)&o_GetRawInputBuffer },
    };
    for (auto& h : hs) {
        void* p = GetProcAddress(u32, h.n);
        if (!(p && MH_CreateHook(p, h.d, h.o) == MH_OK && MH_EnableHook(p) == MH_OK)) log("[overlay] could not hook %s", h.n);
    }
}

// mouse for the panel: free the cursor, read its real position, let ImGui draw it (the game hides the OS cursor)
static void panel_mouse(bool opened_now) {
    ImGuiIO& io = ImGui::GetIO();
    io.MouseDrawCursor = true;
    if (!g_hwnd || GetForegroundWindow() != g_hwnd) return;
    if (o_ClipCursor) o_ClipCursor(nullptr); else ClipCursor(nullptr);
    RECT rc; GetClientRect(g_hwnd, &rc);
    POINT pt; GetCursorPos(&pt); ScreenToClient(g_hwnd, &pt);
    if (opened_now) {
        POINT sp{ (LONG)(rc.right * 0.25f), (LONG)(rc.bottom * 0.35f) }; ClientToScreen(g_hwnd, &sp);
        if (o_SetCursorPos) o_SetCursorPos(sp.x, sp.y); else SetCursorPos(sp.x, sp.y);
        pt = { (LONG)(rc.right * 0.25f), (LONG)(rc.bottom * 0.35f) };
    }
    io.AddMousePosEvent((float)pt.x, (float)pt.y);
    io.AddMouseButtonEvent(0, (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0);
    io.AddMouseButtonEvent(1, (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0);
}

// ------------------------------------------------------------------ present
static void init_ui(IDXGISwapChain* sc) {
    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&g_dev))) return;
    g_dev->GetImmediateContext(&g_ctx);
    DXGI_SWAP_CHAIN_DESC d; sc->GetDesc(&d);
    g_hwnd = d.OutputWindow; g_sc = sc;
    g_ui_scale = std::min(2.5f, std::max(1.f, d.BufferDesc.Height / 1080.f));
    ImGui::CreateContext();
    lang_init();
    load_fonts();
    ImGui::StyleColorsDark();
    ImGui::GetStyle().ScaleAllSizes(g_ui_scale);
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_dev, g_ctx);
    o_WndProc = (WNDPROC)SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, (LONG_PTR)hk_WndProc);
    install_input_hooks();
    panel_init();
    g_ui_ready = true; g_ready_ms = GetTickCount64(); g_note = "ready";
    log("[overlay] ready %ux%u (format %d, scale %.2f)", d.BufferDesc.Width, d.BufferDesc.Height, (int)d.BufferDesc.Format, g_ui_scale);
}

static void draw_toasts() {
    ULONGLONG now = GetTickCount64();
    std::vector<std::pair<std::string, float>> show;
    if (now - g_ready_ms < 8000) show.push_back({ tr(WB_PRODUCT " attached - press INSERT for the trainer"), 1.f - std::max(0.f, (float)(now - g_ready_ms) - 6500.f) / 1500.f });
    {
        std::lock_guard<std::mutex> lk(g_toast_mtx);
        while (!g_toasts.empty() && now - g_toasts.front().second > 5000) g_toasts.pop_front();
        for (auto& t : g_toasts) show.push_back({ t.first, 1.f - std::max(0.f, (float)(now - t.second) - 4000.f) / 1000.f });
    }
    float y = 24 * g_ui_scale;
    for (size_t i = 0; i < show.size(); ++i) {
        float a = std::max(0.f, std::min(1.f, show[i].second));
        ImGui::SetNextWindowPos(ImVec2(24 * g_ui_scale, y)); ImGui::SetNextWindowBgAlpha(0.9f * a);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, a);
        char id[32]; sprintf_s(id, "##toast%zu", i);
        ImGui::Begin(id, nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings);
        ImVec2 p = ImGui::GetWindowPos(), sz = ImGui::GetWindowSize();
        ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + 3 * g_ui_scale, p.y + sz.y), IM_COL32(0xC8, 0xA4, 0x64, (int)(255 * a)));
        ImGui::PushFont(g_font_bold); ImGui::TextUnformatted(tr_msg(show[i].first).c_str()); ImGui::PopFont();
        y += ImGui::GetWindowSize().y + 6 * g_ui_scale;
        ImGui::End();
        ImGui::PopStyleVar();
    }
}

// Shadow of War presents the splash/intro on one swapchain and the game on a new one (2026-11-11: overlay "ready",
// frames counting, but nothing drawn - bound to the dead splash swapchain). When another swapchain presents to the
// game window while the bound one has been silent for 500 ms, move the overlay to it (new device -> rebuild the
// DX11 backend and the skin textures).
void skin_device_lost();
static ULONGLONG g_bound_last = 0;
static void rebind_ui(IDXGISwapChain* sc) {
    std::lock_guard<std::recursive_mutex> lk(g_imgui_mtx);
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
    ID3D11Device* nd = nullptr;
    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&nd))) return;
    const bool new_dev = nd != g_dev;
    if (new_dev) {
        ImGui_ImplDX11_Shutdown();
        skin_device_lost();
        if (g_ctx) g_ctx->Release();
        if (g_dev) g_dev->Release();
        g_dev = nd; g_dev->GetImmediateContext(&g_ctx);
        ImGui_ImplDX11_Init(g_dev, g_ctx);
    } else nd->Release();
    g_sc = sc; g_bound_last = GetTickCount64();
    DXGI_SWAP_CHAIN_DESC d{}; sc->GetDesc(&d);
    if (d.OutputWindow && d.OutputWindow != g_hwnd) {       // new game window: move input + WndProc hook too
        if (g_hwnd && o_WndProc && IsWindow(g_hwnd)) SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, (LONG_PTR)o_WndProc);
        ImGui_ImplWin32_Shutdown();
        g_hwnd = d.OutputWindow;
        ImGui_ImplWin32_Init(g_hwnd);
        o_WndProc = (WNDPROC)SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, (LONG_PTR)hk_WndProc);
        log("[overlay] game window changed");
    }
    log("[overlay] moved to the game's live swapchain %ux%u (format %d%s)", d.BufferDesc.Width, d.BufferDesc.Height, (int)d.BufferDesc.Format, new_dev ? ", new device" : "");
}

static HRESULT __stdcall hk_Present(IDXGISwapChain* sc, UINT sync, UINT flags) {
    ++g_frames;
    if (g_dead) return o_Present(sc, sync, flags);
    if (!g_ui_ready) {
        DXGI_SWAP_CHAIN_DESC d;
        if (SUCCEEDED(sc->GetDesc(&d)) && d.OutputWindow == find_game_window()) { init_ui(sc); g_bound_last = GetTickCount64(); }
    } else if (sc == g_sc) g_bound_last = GetTickCount64();
    else if (GetTickCount64() - g_bound_last > 500) {
        DXGI_SWAP_CHAIN_DESC d;
        if (SUCCEEDED(sc->GetDesc(&d)) && (d.OutputWindow == g_hwnd || d.OutputWindow == find_game_window())) rebind_ui(sc);
    }
    if (g_ui_ready && sc == g_sc) try {
        std::lock_guard<std::recursive_mutex> lk(g_imgui_mtx);
        // INSERT (rising edge, polled: the game reads raw input, so the key message may never arrive)
        static bool was = false; static bool vis_was = false;
        bool down = (GetAsyncKeyState(g_cfg.toggle_key) & 0x8000) != 0 && GetForegroundWindow() == g_hwnd;
        if (down && !was) g_visible = !g_visible;
        was = down;
        if (!g_rtv) {
            ID3D11Texture2D* bb = nullptr;
            if (SUCCEEDED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb))) { g_dev->CreateRenderTargetView(bb, nullptr, &g_rtv); bb->Release(); }
        }
        if (lang_apply_pending()) reload_fonts();
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        bool vis = g_visible;
        if (vis) panel_mouse(!vis_was); else ImGui::GetIO().MouseDrawCursor = false;
        vis_was = vis;
        ImGui::NewFrame();
        if (GetForegroundWindow() == g_hwnd) panel_hotkeys_poll();
        draw_toasts();
        if (vis) { panel_draw(&vis); if (!vis) g_visible = false; }
        ImGui::Render();
        if (g_rtv) {
            ID3D11RenderTargetView* old_rtv = nullptr; ID3D11DepthStencilView* old_dsv = nullptr;
            g_ctx->OMGetRenderTargets(1, &old_rtv, &old_dsv);
            g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            g_ctx->OMSetRenderTargets(1, &old_rtv, old_dsv);
            if (old_rtv) old_rtv->Release();
            if (old_dsv) old_dsv->Release();
        }
    } catch (std::exception& e) { log("[overlay] frame threw: %s - panel hidden", e.what()); g_visible = false; }
    catch (...) { log("[overlay] frame threw - panel hidden"); g_visible = false; }
    return o_Present(sc, sync, flags);
}

static HRESULT __stdcall hk_Resize(IDXGISwapChain* sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT fl) {
    { std::lock_guard<std::recursive_mutex> lk(g_imgui_mtx); if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; } }
    return o_Resize(sc, n, w, h, f, fl);
}

bool install_overlay() {
    WNDCLASSEXA wc{ sizeof wc, CS_CLASSDC, DefWindowProcA, 0, 0, GetModuleHandleA(nullptr), nullptr, nullptr, nullptr, nullptr, "OB_Dummy", nullptr };
    RegisterClassExA(&wc);
    HWND dw = CreateWindowA("OB_Dummy", "", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 1; sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = dw; sd.SampleDesc.Count = 1; sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    IDXGISwapChain* sc = nullptr; ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &sc, &dev, nullptr, &ctx);
    if (FAILED(hr)) { log("[overlay] dummy D3D11 device failed (0x%08X)", hr); DestroyWindow(dw); return false; }
    void** vt = *(void***)sc;
    void* present = vt[8]; void* resize = vt[13];
    sc->Release(); dev->Release(); ctx->Release(); DestroyWindow(dw); UnregisterClassA("OB_Dummy", wc.hInstance);
    bool okh = MH_CreateHook(present, &hk_Present, (void**)&o_Present) == MH_OK && MH_CreateHook(resize, &hk_Resize, (void**)&o_Resize) == MH_OK &&
               MH_EnableHook(present) == MH_OK && MH_EnableHook(resize) == MH_OK;
    g_note = okh ? "waiting for first frame" : "hook failed";
    log("[overlay] DX11 hooks %s", okh ? "installed" : "FAILED");
    return okh;
}

json overlay_status() { return { {"state", g_note}, {"frames", g_frames.load()}, {"visible", g_visible.load()} }; }

} // namespace wb
