// Shadow of War Trainer - loader DLL (xinput9_1_0.dll proxy).
//
// The game loads xinput9_1_0.dll by name, and Windows looks in the game's own folder first. This copy, placed next to
// ShadowOfWar.exe:
//   1. passes every XInput call on, unchanged, to the real xinput9_1_0.dll in System32 (gamepads keep working), and
//   2. loads the trainer: <exe folder>\ShadowOfWarTrainer\ShadowOfWarTrainer.dll.
// Nothing runs outside the game and nothing is injected. Uninstall: delete this file and the trainer folder.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>

static HMODULE g_self = nullptr, g_real = nullptr;
static const wchar_t kGameExe[] = L"ShadowOfWar.exe";
static const wchar_t kFolder[] = L"ShadowOfWarTrainer";
static const wchar_t kTrainerDll[] = L"ShadowOfWarTrainer.dll";

// the real DLL, loaded the first time the game calls XInput (never in DllMain: loader lock)
static FARPROC real(const char* name) {
    if (!g_real) {
        wchar_t sys[MAX_PATH]; const UINT n = GetSystemDirectoryW(sys, MAX_PATH);
        if (!n || n >= MAX_PATH) return nullptr;
        g_real = LoadLibraryW((std::wstring(sys) + L"\\xinput9_1_0.dll").c_str());
        if (!g_real) return nullptr;
    }
    return GetProcAddress(g_real, name);
}

typedef UINT_PTR U;
// 1167 = ERROR_DEVICE_NOT_CONNECTED: what XInput itself answers when there is no pad
#define FWD(name, ...) auto f = (DWORD(WINAPI*)(__VA_ARGS__))real(name); if (!f) return 1167;
extern "C" {
DWORD WINAPI p_XInputGetCapabilities(U user, U flags, U caps) { FWD("XInputGetCapabilities", U, U, U) return f(user, flags, caps); }
DWORD WINAPI p_XInputGetDSoundAudioDeviceGuids(U user, U render, U capture) { FWD("XInputGetDSoundAudioDeviceGuids", U, U, U) return f(user, render, capture); }
DWORD WINAPI p_XInputGetState(U user, U state) { FWD("XInputGetState", U, U) return f(user, state); }
DWORD WINAPI p_XInputSetState(U user, U vibration) { FWD("XInputSetState", U, U) return f(user, vibration); }
}

// A thread made in DllMain only starts after the loader lock is released, so LoadLibrary is safe here.
static DWORD WINAPI load_trainer(LPVOID) {
    wchar_t p[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, p, MAX_PATH)) return 0;
    std::wstring exe = p; exe = exe.substr(exe.find_last_of(L"\\/") + 1);
    if (_wcsicmp(exe.c_str(), kGameExe) != 0) return 0;              // only inside the game (not crash reporters etc.)
    if (!GetModuleFileNameW(g_self, p, MAX_PATH)) return 0;
    std::wstring dir = p; dir = dir.substr(0, dir.find_last_of(L"\\/")) + L"\\" + kFolder;
    const std::wstring dll = dir + L"\\" + kTrainerDll;
    if (GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES) return 0;
    // Load a copy from <folder>\loaded so the trainer DLL itself is never locked: it can be replaced (updated, rebuilt)
    // while the game runs. The trainer finds its files next to the folder "loaded". Old copies are removed here;
    // copies still in use by another running game are locked and simply stay.
    const std::wstring ld = dir + L"\\loaded";
    CreateDirectoryW(ld.c_str(), nullptr);
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((ld + L"\\ShadowOfWarTrainer_*.dll").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) { do DeleteFileW((ld + L"\\" + fd.cFileName).c_str()); while (FindNextFileW(h, &fd)); FindClose(h); }
    SYSTEMTIME t; GetLocalTime(&t);
    wchar_t name[96];
    swprintf_s(name, L"\\ShadowOfWarTrainer_%04d%02d%02d_%02d%02d%02d_%lu.dll", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, GetCurrentProcessId());
    const std::wstring copy = ld + name;
    LoadLibraryW(CopyFileW(dll.c_str(), copy.c_str(), FALSE) ? copy.c_str() : dll.c_str());
    return 0;
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = h; DisableThreadLibraryCalls(h);
        HANDLE t = CreateThread(nullptr, 0, load_trainer, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
