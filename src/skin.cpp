// Shadow of War Trainer - skin textures (DX11).
// The panel skin is a folder of PNGs (<root>\skin\*.png + skin.json). A piece is
// drawn by name, slicing encoded in the name like the BL4 host ABI:
//   "frame"              whole image stretched into the rect
//   "tab#3:24,24"        3-slice: left/right caps (source px) keep their aspect at the rect's height, middle stretches
//   "x#9:l,t,r,b@k"      9-slice: corners scaled by k (default 1), edges + centre stretch
// DX11 makes this simpler than BL4's DX12: CreateTexture2D takes the pixels directly (no upload queue), and the
// ImTextureID is the ID3D11ShaderResourceView*. Loading is lazy (first draw, render thread). Files are re-checked every
// 1.5 s by modified time, so re-running the cutter shows the new art without restarting the game.
#include "common.h"
#include <d3d11.h>
#include <wincodec.h>
#include <unordered_map>
#include "imgui.h"
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

namespace wb {
ID3D11Device* overlay_device();

namespace {
struct Tex { ID3D11ShaderResourceView* srv = nullptr; int w = 0, h = 0; bool ok = false; FILETIME mt{}; ULONGLONG checked = 0; };
std::unordered_map<std::string, Tex> g_tex;
std::vector<ID3D11ShaderResourceView*> g_old;   // replaced views (may still be referenced by this frame's draw data)
IWICImagingFactory* g_wic = nullptr;

bool decode_png(const std::wstring& path, std::vector<uint8_t>& px, UINT& w, UINT& h) {
    if (!g_wic) {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);   // RPC_E_CHANGED_MODE is fine: WIC works in either apartment
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_wic)))) return false;
    }
    IWICBitmapDecoder* dec = nullptr; IWICBitmapFrameDecode* fr = nullptr; IWICFormatConverter* cv = nullptr;
    bool ok = false;
    if (SUCCEEDED(g_wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &fr)) && SUCCEEDED(g_wic->CreateFormatConverter(&cv)) &&
        SUCCEEDED(cv->Initialize(fr, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)) &&
        SUCCEEDED(cv->GetSize(&w, &h)) && w && h && w <= 8192 && h <= 8192) {
        px.resize((size_t)w * h * 4);
        ok = SUCCEEDED(cv->CopyPixels(nullptr, w * 4, (UINT)px.size(), px.data()));
    }
    if (cv) cv->Release(); if (fr) fr->Release(); if (dec) dec->Release();
    return ok;
}

} // namespace
// the overlay moved to a swapchain on another device: drop every texture, they are re-created on first use
void skin_device_lost() {
    for (auto& kv : g_tex) if (kv.second.srv) kv.second.srv->Release();
    g_tex.clear();
    for (auto* v : g_old) if (v) v->Release();
    g_old.clear();
}
namespace {
bool upload(UINT w, UINT h, const uint8_t* px, Tex& out) {
    ID3D11Device* dev = overlay_device(); if (!dev) return false;
    D3D11_TEXTURE2D_DESC td{}; td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{ px, w * 4, 0 };
    ID3D11Texture2D* tex = nullptr;
    if (FAILED(dev->CreateTexture2D(&td, &sd, &tex))) return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC vd{}; vd.Format = td.Format; vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; vd.Texture2D.MipLevels = 1;
    ID3D11ShaderResourceView* srv = nullptr;
    const HRESULT hr = dev->CreateShaderResourceView(tex, &vd, &srv);
    tex->Release();                                // the view keeps the texture alive
    if (FAILED(hr)) return false;
    out.srv = srv; out.w = (int)w; out.h = (int)h; out.ok = true;
    return true;
}

FILETIME mtime_of(const std::wstring& p) {
    WIN32_FILE_ATTRIBUTE_DATA a{}; if (GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a)) return a.ftLastWriteTime; return FILETIME{};
}
const Tex* get(const std::string& name) {
    if (name.find_first_of("\\/:.") != std::string::npos) return nullptr;
    const std::string p = g_cfg.root + "\\skin\\" + name + ".png";
    const std::wstring wp(p.begin(), p.end());
    const ULONGLONG now = GetTickCount64();
    auto it = g_tex.find(name);
    if (it != g_tex.end()) {
        Tex& t = it->second;
        if (now - t.checked < 1500) return t.ok ? &t : nullptr;
        t.checked = now;
        const FILETIME m = mtime_of(wp);
        if (CompareFileTime(&m, &t.mt) == 0) return t.ok ? &t : nullptr;
        if (t.srv) g_old.push_back(t.srv);       // re-cut art: load the new file
        t = Tex{};
    }
    Tex& t = g_tex[name];                        // a miss is cached too (re-checked by file time)
    t.checked = now; t.mt = mtime_of(wp);
    std::vector<uint8_t> px; UINT w = 0, h = 0;
    if (decode_png(wp, px, w, h) && upload(w, h, px.data(), t)) return &t;
    log("[skin] could not load %s", p.c_str());
    return nullptr;
}
void quad(ImDrawList* dl, const Tex& t, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, ImU32 tint) {
    if (x1 - x0 < 0.5f || y1 - y0 < 0.5f) return;
    dl->AddImage((ImTextureID)(intptr_t)t.srv, ImVec2(x0, y0), ImVec2(x1, y1), ImVec2(u0 / t.w, v0 / t.h), ImVec2(u1 / t.w, v1 / t.h), tint);
}
} // namespace

// size of a skin image in source pixels (0,0 when missing)
ImVec2 skin_image_size(const char* name) { const Tex* t = get(name); return t ? ImVec2((float)t->w, (float)t->h) : ImVec2(0, 0); }

bool skin_draw(ImDrawList* dl, const char* sprite, ImVec2 a, ImVec2 b, ImU32 tint) {
    if (!dl || !sprite || !*sprite) return false;
    std::string s = sprite, name = s, spec;
    const size_t hash = s.find('#');
    if (hash != std::string::npos) { name = s.substr(0, hash); spec = s.substr(hash + 1); }
    const Tex* t = get(name);
    if (!t) return false;
    if (spec.empty()) { quad(dl, *t, a.x, a.y, b.x, b.y, 0, 0, (float)t->w, (float)t->h, tint); return true; }
    float scale = -1.f;
    const size_t at = spec.find('@');
    if (at != std::string::npos) { scale = (float)atof(spec.c_str() + at + 1); spec.resize(at); }
    const char kind = spec[0];
    float v[4] = { 0, 0, 0, 0 }; int n = 0;
    { const char* c = strchr(spec.c_str(), ':'); while (c && n < 4) { v[n++] = (float)atof(c + 1); c = strchr(c + 1, ','); } }
    const float W = (float)t->w, H = (float)t->h;
    if (kind == '3') {                              // left cap, middle, right cap - caps keep their aspect at the rect's height
        const float k = scale > 0 ? scale : (b.y - a.y) / H;
        float l = v[0] * k, r = v[1] * k;
        const float room = b.x - a.x;
        if (l + r > room) { const float f = room / (l + r); l *= f; r *= f; }
        quad(dl, *t, a.x, a.y, a.x + l, b.y, 0, 0, v[0], H, tint);
        quad(dl, *t, a.x + l, a.y, b.x - r, b.y, v[0], 0, W - v[1], H, tint);
        quad(dl, *t, b.x - r, a.y, b.x, b.y, W - v[1], 0, W, H, tint);
        return true;
    }
    if (kind == '9') {
        const float k = scale > 0 ? scale : 1.f;
        float L = v[0] * k, T = v[1] * k, R = v[2] * k, B = v[3] * k;
        if (L + R > b.x - a.x) { const float f = (b.x - a.x) / (L + R); L *= f; R *= f; }
        if (T + B > b.y - a.y) { const float f = (b.y - a.y) / (T + B); T *= f; B *= f; }
        const float xs[4] = { a.x, a.x + L, b.x - R, b.x }, ys[4] = { a.y, a.y + T, b.y - B, b.y };
        const float us[4] = { 0, v[0], W - v[2], W }, vs[4] = { 0, v[1], H - v[3], H };
        for (int j = 0; j < 3; ++j) for (int i = 0; i < 3; ++i)
            quad(dl, *t, xs[i], ys[j], xs[i + 1], ys[j + 1], us[i], vs[j], us[i + 1], vs[j + 1], tint);
        return true;
    }
    quad(dl, *t, a.x, a.y, b.x, b.y, 0, 0, W, H, tint);
    return true;
}
} // namespace wb
